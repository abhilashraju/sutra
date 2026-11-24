/**
 * @file wait_queue.hpp
 * @brief A condition-variable-like primitive for tasks on one io_context.
 *
 * `wait()` suspends the calling task until `notifyOne()` / `notifyAll()` or a
 * timeout. It replaces the "steady_timer set to max, cancel to wake" trick with
 * a named, reusable type. Not thread safe: use it from the io_context thread
 * (or a strand) only, like every other sutra component.
 */
#pragma once

#include "sutra/config.hpp"

#include <algorithm>
#include <deque>
#include <memory>

namespace sutra
{

class WaitQueue
{
  public:
    explicit WaitQueue(net::any_io_executor executor) :
        executor(std::move(executor))
    {}

    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;

    ~WaitQueue()
    {
        notifyAll();
    }

    /**
     * @brief Suspends until notified or until `timeout` elapses.
     * @return true when woken by notifyOne()/notifyAll() (or by a stop
     *         request, which cancels the timer), false on timeout.
     */
    Task<bool> wait(std::chrono::steady_clock::duration timeout =
                        std::chrono::steady_clock::duration::max())
    {
        auto timer = std::make_shared<net::steady_timer>(executor);
        if (timeout == std::chrono::steady_clock::duration::max())
        {
            timer->expires_at(net::steady_timer::time_point::max());
        }
        else
        {
            timer->expires_after(timeout);
        }
        waiters.push_back(timer);

        auto [ec] = co_await timer->async_wait(useSenderNoThrow);

        std::erase(waiters, timer);
        co_return ec == net::error::operation_aborted;
    }

    /** @brief Wakes the longest waiting task, if any. */
    void notifyOne()
    {
        if (waiters.empty())
        {
            return;
        }
        auto timer = std::move(waiters.front());
        waiters.pop_front();
        timer->cancel();
    }

    /** @brief Wakes every waiting task. */
    void notifyAll()
    {
        auto pending = std::move(waiters);
        waiters.clear();
        for (auto& timer : pending)
        {
            timer->cancel();
        }
    }

    size_t waiting() const
    {
        return waiters.size();
    }

  private:
    net::any_io_executor executor;
    std::deque<std::shared_ptr<net::steady_timer>> waiters;
};

} // namespace sutra
