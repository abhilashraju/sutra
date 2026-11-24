// Spike: exec::task + use_sender + stop propagation on an io_context.
#include "sutra/config.hpp"

#include <cstdio>
#include <thread>

using namespace sutra;

static std::thread::id ioThread;
static int failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

Task<int> timerThenValue(net::io_context& ioc)
{
    net::steady_timer timer(ioc);
    timer.expires_after(5ms);
    co_await timer.async_wait(useSender);
    CHECK(std::this_thread::get_id() == ioThread);
    co_await onExecutor(ioc.get_executor());
    CHECK(std::this_thread::get_id() == ioThread);
    co_return 42;
}

Task<void> failing(net::io_context& ioc)
{
    net::steady_timer timer(ioc);
    timer.expires_after(5ms);
    co_await timer.async_wait(useSender);
    throw std::runtime_error("boom");
}

Task<void> longWait(net::io_context& ioc, bool& reachedEnd)
{
    net::steady_timer timer(ioc);
    timer.expires_after(10s);
    // A stop request completes this with set_stopped; code after is skipped.
    co_await timer.async_wait(useSender);
    reachedEnd = true;
}

int main()
{
    net::io_context ioc;
    ioThread = std::this_thread::get_id();

    int value = 0;
    exec::async_scope scope;
    scope.spawn(timerThenValue(ioc) | ex::then([&](int v) { value = v; }));

    bool gotError = false;
    scope.spawn(failing(ioc) |
                ex::upon_error([&](std::exception_ptr) { gotError = true; }));

    bool reachedEnd = false;
    bool stopObserved = false;
    scope.spawn(longWait(ioc, reachedEnd) |
                ex::upon_stopped([&]() { stopObserved = true; }));
    net::steady_timer stopper(ioc);
    stopper.expires_after(30ms);
    stopper.async_wait([&](error_code) { scope.request_stop(); });

    auto begin = std::chrono::steady_clock::now();
    ioc.run();
    auto took = std::chrono::steady_clock::now() - begin;

    CHECK(value == 42);
    CHECK(gotError);
    CHECK(stopObserved);
    CHECK(!reachedEnd);
    CHECK(took < 2s);
    std::printf("%s (%lld ms)\n", failures == 0 ? "OK" : "FAILED",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(took)
                        .count()));
    return failures == 0 ? 0 : 1;
}
