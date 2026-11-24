/**
 * @file spawn.hpp
 * @brief Helpers for running fire-and-forget tasks inside an async_scope.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/log.hpp"

#include <string>

namespace sutra
{

/** @brief Returns the what() text of an exception_ptr. */
inline std::string describe(const std::exception_ptr& eptr)
{
    try
    {
        std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

/**
 * @brief Spawns `sender` into `scope`, logging errors and swallowing stops.
 *
 * async_scope::spawn needs a sender that cannot fail; this adapts a Task (or
 * any sender with an exception_ptr error channel) to that contract.
 */
template <class Scope, class Sender>
void spawnTask(Scope& scope, Sender&& sender, std::string what)
{
    scope.spawn(std::forward<Sender>(sender) |
                ex::upon_error([what = std::move(what)](std::exception_ptr e) {
                    logWarning("{} failed: {}", what, describe(e));
                }) |
                ex::upon_stopped([]() {}));
}

} // namespace sutra
