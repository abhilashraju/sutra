#include "sutra/gather.hpp"
#include "sutra/spawn.hpp"

#include <gtest/gtest.h>

using namespace sutra;

namespace
{

struct Probe
{
    int running = 0;
    int peak = 0;
    int finished = 0;
};

Task<int> delayed(net::io_context& ioc, Probe& probe, int value,
                  std::chrono::milliseconds wait)
{
    ++probe.running;
    probe.peak = std::max(probe.peak, probe.running);
    net::steady_timer timer(ioc, wait);
    co_await timer.async_wait(useSender);
    --probe.running;
    ++probe.finished;
    co_return value;
}

Task<int> failing(net::io_context& ioc, Probe& probe, std::string message)
{
    net::steady_timer timer(ioc, 1ms);
    co_await timer.async_wait(useSender);
    ++probe.finished;
    throw std::runtime_error(message);
    co_return 0;
}

template <class Body>
void runTask(net::io_context& ioc, Body& body)
{
    exec::async_scope scope;
    spawnTask(scope, body(), "gather test");
    ioc.run();
}

} // namespace

TEST(Gather, RunsConcurrentlyAndKeepsOrder)
{
    net::io_context ioc;
    Probe probe;
    std::vector<int> values;
    auto begin = std::chrono::steady_clock::now();
    auto body = [&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        // Later tasks finish first; the result order must not change.
        for (int i = 0; i < 5; ++i)
        {
            tasks.push_back(delayed(ioc, probe, i, 100ms - i * 15ms));
        }
        values = co_await gather(std::move(tasks));
    };
    runTask(ioc, body);
    auto took = std::chrono::steady_clock::now() - begin;
    EXPECT_EQ(values, (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_EQ(probe.peak, 5);
    // Sequential execution would need 100+85+70+55+40 = 350 ms.
    EXPECT_LT(took, 250ms);
}

TEST(Gather, HonoursConcurrencyLimit)
{
    net::io_context ioc;
    Probe probe;
    std::vector<int> values;
    auto body = [&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        for (int i = 0; i < 7; ++i)
        {
            tasks.push_back(delayed(ioc, probe, i, 5ms));
        }
        values = co_await gather(std::move(tasks), 3);
    };
    runTask(ioc, body);
    EXPECT_EQ(values.size(), 7u);
    EXPECT_EQ(probe.peak, 3);
}

TEST(Gather, EmptyInput)
{
    net::io_context ioc;
    bool done = false;
    auto body = [&]() -> Task<void> {
        auto values = co_await gather(std::vector<Task<int>>{});
        EXPECT_TRUE(values.empty());
        done = true;
    };
    runTask(ioc, body);
    EXPECT_TRUE(done);
}

TEST(Gather, FailureWaitsForSiblingsThenRethrowsFirstInInputOrder)
{
    net::io_context ioc;
    Probe probe;
    std::string caught;
    auto body = [&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back(delayed(ioc, probe, 0, 30ms));
        tasks.push_back(failing(ioc, probe, "second"));
        tasks.push_back(failing(ioc, probe, "third"));
        try
        {
            co_await gather(std::move(tasks));
        }
        catch (const std::exception& e)
        {
            caught = e.what();
        }
    };
    runTask(ioc, body);
    EXPECT_EQ(caught, "second");
    // The slow sibling was not abandoned.
    EXPECT_EQ(probe.finished, 3);
}

TEST(Gather, StopIsForwardedToChildren)
{
    net::io_context ioc;
    Probe probe;
    bool stopObserved = false;
    bool reachedEnd = false;
    exec::async_scope scope;
    auto body = [&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        for (int i = 0; i < 3; ++i)
        {
            tasks.push_back(delayed(ioc, probe, i, 10s));
        }
        co_await gather(std::move(tasks));
        reachedEnd = true;
    };
    scope.spawn(body() | ex::upon_error([](std::exception_ptr) {}) |
                ex::upon_stopped([&]() { stopObserved = true; }));
    net::steady_timer stopper(ioc, 30ms);
    stopper.async_wait([&](error_code) { scope.request_stop(); });

    auto begin = std::chrono::steady_clock::now();
    ioc.run();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
    EXPECT_TRUE(stopObserved);
    EXPECT_FALSE(reachedEnd);
}

TEST(Gather, VoidTasks)
{
    net::io_context ioc;
    int counter = 0;
    auto body = [&]() -> Task<void> {
        std::vector<Task<void>> tasks;
        for (int i = 0; i < 4; ++i)
        {
            tasks.push_back([](net::io_context& c, int& n) -> Task<void> {
                net::steady_timer timer(c, 1ms);
                co_await timer.async_wait(useSender);
                ++n;
            }(ioc, counter));
        }
        co_await gatherAll(std::move(tasks));
    };
    runTask(ioc, body);
    EXPECT_EQ(counter, 4);
}
