#include "sutra/config.hpp"
#include "sutra/dbus_property_watcher.hpp"
#include "sutra/graphql/ql_dbus_property_watcher.hpp"
#include "sutra/spawn.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef SUTRA_HAVE_DBUS

using namespace sutra;
using namespace std::chrono_literals;

namespace
{

template <typename T>
struct TestWatcher : DbusWatcher<TestWatcher<T>, T>
{
    using BASE = DbusWatcher<TestWatcher<T>, T>;

  private:
    struct PrivateTag
    {};

  public:
    static std::shared_ptr<TestWatcher<T>> make(net::io_context& ioc)
    {
        return std::make_shared<TestWatcher<T>>(PrivateTag{}, nullptr, ioc);
    }

    TestWatcher(PrivateTag, std::shared_ptr<sdbusplus::asio::connection> conn,
                net::io_context& ioc) : BASE(std::move(conn)), ioc_(ioc)
    {}

    net::io_context& getIoContextImpl()
    {
        return ioc_;
    }

    void fireSignal(T value)
    {
        BASE::notifyChange(error_code{}, std::move(value));
    }

    void fireCancel()
    {
        BASE::cancelWatch();
    }

    void fireError(error_code ec)
    {
        BASE::notifyChange(ec, T{});
    }

    void scheduleSignalAfterDelay(std::chrono::milliseconds delay, T value)
    {
        auto timer = std::make_shared<net::steady_timer>(ioc_);
        timer->expires_after(delay);
        std::weak_ptr<TestWatcher<T>> weak = BASE::derived().shared_from_this();
        timer->async_wait([timer, weak, value = std::move(value)](
                              const error_code& ec) mutable {
            if (!ec)
            {
                if (auto self = weak.lock())
                {
                    self->fireSignal(std::move(value));
                }
            }
        });
    }

    void scheduleCancelAfterDelay(std::chrono::milliseconds delay)
    {
        auto timer = std::make_shared<net::steady_timer>(ioc_);
        timer->expires_after(delay);
        std::weak_ptr<TestWatcher<T>> weak = BASE::derived().shared_from_this();
        timer->async_wait([timer, weak](const error_code& ec) {
            if (!ec)
            {
                if (auto self = weak.lock())
                {
                    self->fireCancel();
                }
            }
        });
    }

  private:
    net::io_context& ioc_;
};

template <typename Coro>
void runTask(net::io_context& ioc, Coro&& coro)
{
    bool finished = false;
    std::exception_ptr err;
    exec::async_scope scope;
    scope.spawn(
        std::forward<Coro>(coro) | ex::then([&]() { finished = true; }) |
        ex::upon_error([&](std::exception_ptr ep) {
            finished = true;
            err = ep;
        }) |
        ex::upon_stopped([&]() { finished = true; }));
    ioc.run();
    ioc.restart();
    if (err)
    {
        std::rethrow_exception(err);
    }
    EXPECT_TRUE(finished);
}

} // namespace

TEST(DbusPropertyWatcherTest, WatchOnceHappyPath)
{
    net::io_context ioc;
    auto watcher = TestWatcher<std::string>::make(ioc);

    watcher->scheduleSignalAfterDelay(10ms, "hello");

    std::optional<std::string> result;
    runTask(ioc, [&]() -> Task<void> {
        result = co_await watcher->watchOnce(5s);
    }());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
    EXPECT_FALSE(watcher->propHandler);
}

TEST(DbusPropertyWatcherTest, WatchOnceTimeout)
{
    net::io_context ioc;
    auto watcher = TestWatcher<std::string>::make(ioc);

    watcher->scheduleCancelAfterDelay(10ms);

    std::optional<std::string> result = "sentinel";
    runTask(ioc, [&]() -> Task<void> {
        result = co_await watcher->watchOnce(5s);
    }());

    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(watcher->propHandler);
}

TEST(DbusPropertyWatcherTest, CancelWatchRobustness)
{
    net::io_context ioc;
    auto watcher = TestWatcher<std::string>::make(ioc);

    EXPECT_FALSE(watcher->propHandler);
    EXPECT_NO_THROW(watcher->fireCancel());

    bool handlerCalled = false;
    runTask(ioc, [&]() -> Task<void> {
        watcher->scheduleCancelAfterDelay(5ms);
        auto result = co_await watcher->watchOnce(5s);
        handlerCalled = !result.has_value();
    }());

    EXPECT_TRUE(handlerCalled);
}

TEST(DbusPropertyWatcherTest, NotifyChangeMoveOutSafety)
{
    net::io_context ioc;
    auto watcher = TestWatcher<int>::make(ioc);

    watcher->scheduleSignalAfterDelay(10ms, 42);
    watcher->scheduleSignalAfterDelay(20ms, 99);

    int received = -1;
    runTask(ioc, [&]() -> Task<void> {
        auto result = co_await watcher->watchOnce(5s);
        if (result)
        {
            received = *result;
        }
        net::steady_timer yield(ioc);
        yield.expires_after(30ms);
        co_await yield.async_wait(useSenderNoThrow);
    }());

    EXPECT_EQ(received, 42);
    EXPECT_FALSE(watcher->propHandler);
}

TEST(DbusPropertyWatcherTest, WatchLoopBoolCallback)
{
    net::io_context ioc;
    auto watcher = TestWatcher<std::string>::make(ioc);

    for (int i = 1; i <= 3; ++i)
    {
        watcher->scheduleSignalAfterDelay(
            std::chrono::milliseconds(i * 10),
            std::string(1, static_cast<char>('A' + i - 1)));
    }

    std::vector<std::string> received;
    runTask(ioc, [&]() -> Task<void> {
        co_await watcher->watch(
            [&received](const error_code&,
                        std::optional<std::string> val) -> Task<bool> {
                if (val)
                {
                    received.push_back(*val);
                }
                co_return received.size() < 3;
            });
    }());

    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], "A");
    EXPECT_EQ(received[1], "B");
    EXPECT_EQ(received[2], "C");
    EXPECT_FALSE(watcher->propHandler);
}

#endif // SUTRA_HAVE_DBUS
