/**
 * @file gather.hpp
 * @brief Runs a runtime-sized set of Tasks concurrently.
 *
 * `ex::when_all` has a fixed arity, so a vector of tasks (one per D-Bus object,
 * one per GraphQL root field, ...) needs this helper. It starts up to
 * `maxConcurrent` tasks, waits for them, then starts the next batch, which is
 * how coroserver bounded its fan-out.
 *
 * Semantics
 * ---------
 * - Results come back in input order.
 * - Every task runs to completion even if another fails; afterwards the first
 *   failure (in input order) is rethrown.
 * - If the awaiting task is stopped, the stop is forwarded to the children and
 *   the gather completes as stopped once they have unwound.
 * - Everything runs on the calling io_context thread, so no locking is needed.
 */
#pragma once

#include "sutra/config.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <type_traits>
#include <vector>

namespace sutra
{

namespace details
{

struct Void
{};

template <class T>
using GatherSlot = std::conditional_t<std::is_void_v<T>, Void, T>;

} // namespace details

/**
 * @brief Awaits all `tasks`, at most `maxConcurrent` at a time.
 * @return the results in input order (`std::vector<details::Void>` for
 *         `Task<void>`; use gatherAll() when there are no values).
 */
template <class T>
Task<std::vector<details::GatherSlot<T>>> gather(std::vector<Task<T>> tasks,
                                                 size_t maxConcurrent = 8)
{
    using Slot = details::GatherSlot<T>;
    const size_t total = tasks.size();
    std::vector<std::optional<Slot>> results(total);
    std::vector<std::exception_ptr> errors(total);
    bool stopped = false;
    maxConcurrent = std::max<size_t>(maxConcurrent, 1);

    auto token = co_await ex::get_stop_token();
    for (size_t first = 0; first < total && !stopped; first += maxConcurrent)
    {
        const size_t last = std::min(total, first + maxConcurrent);
        exec::async_scope scope;
        auto forward = [&scope]() noexcept { scope.request_stop(); };
        ex::stop_callback_for_t<decltype(token), decltype(forward)> onStop(
            token, forward);

        for (size_t i = first; i < last; ++i)
        {
            auto onValue = [&results, i]<class... V>(V&&... value) {
                if constexpr (sizeof...(V) == 0)
                {
                    results[i].emplace();
                }
                else
                {
                    results[i].emplace(std::forward<V>(value)...);
                }
            };
            scope.spawn(std::move(tasks[i]) | ex::then(std::move(onValue)) |
                        ex::upon_error([&errors, i](std::exception_ptr e) {
                            errors[i] = std::move(e);
                        }) |
                        ex::upon_stopped([&stopped]() { stopped = true; }));
        }
        co_await scope.on_empty();
    }

    if (stopped || token.stop_requested())
    {
        co_await ex::just_stopped();
    }
    for (auto& error : errors)
    {
        if (error)
        {
            std::rethrow_exception(error);
        }
    }
    std::vector<Slot> out;
    out.reserve(total);
    for (auto& slot : results)
    {
        out.push_back(std::move(*slot));
    }
    co_return out;
}

/** @brief Awaits all `Task<void>`s (same rules as gather()). */
inline Task<void> gatherAll(std::vector<Task<void>> tasks,
                            size_t maxConcurrent = 8)
{
    co_await gather<void>(std::move(tasks), maxConcurrent);
}

} // namespace sutra
