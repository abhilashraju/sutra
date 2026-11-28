#include "sutra/spawn.hpp"
#include "sutra/tcp_client.hpp"
#include "sutra/tcp_server.hpp"
#include "sutra/wait_queue.hpp"

#include <gtest/gtest.h>

using namespace sutra;

namespace
{

Task<void> echoSession(TcpConnection conn)
{
    for (;;)
    {
        auto data = co_await conn.readSome();
        if (!data || data->empty())
        {
            break;
        }
        if (!co_await conn.write(*data))
        {
            break;
        }
    }
}

} // namespace

TEST(Tcp, EchoRoundTrip)
{
    net::io_context ioc;
    TcpServer server(ioc.get_executor(), "127.0.0.1", 0, echoSession);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    std::string reply1;
    std::string reply2;
    exec::async_scope scope;
    // The closure must outlive the coroutine: its captures live in it.
    auto clientTask = [&]() -> Task<void> {
        TcpClient client(ioc.get_executor());
        client.setConnectTimeout(5s);
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("127.0.0.1", port));
        EXPECT_TRUE(co_await client.write("hello\nworld\n"));
        reply1 = (co_await client.readUntil("\n")).value_or("");
        reply2 = (co_await client.readUntil("\n")).value_or("");
        client.close();
        server.stop();
    };
    spawnTask(scope, clientTask(), "echo client");

    ioc.run();
    EXPECT_EQ(reply1, "hello\n");
    EXPECT_EQ(reply2, "world\n");
}

TEST(Tcp, RefusesUnspecifiedAddress)
{
    net::io_context ioc;
    TcpServer server(ioc.get_executor(), "0.0.0.0", 0,
                     [](TcpConnection) -> Task<void> { co_return; });
    auto started = server.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), make_error_code(Errc::unspecifiedAddress));
}

TEST(Tcp, MalformedAddressIsReportedByStart)
{
    net::io_context ioc;
    TcpServer server(ioc.get_executor(), "not-an-address", 0,
                     [](TcpConnection) -> Task<void> { co_return; });
    EXPECT_FALSE(server.start());
}

TEST(Tcp, ConnectFailureIsReturned)
{
    net::io_context ioc;
    bool failed = false;
    exec::async_scope scope;
    auto connectTask = [&]() -> Task<void> {
        TcpClient client(ioc.get_executor());
        client.setConnectTimeout(2s);
        // Port 1 on loopback is not listening.
        auto connected = co_await client.connect("127.0.0.1", "1");
        failed = !connected;
    };
    spawnTask(scope, connectTask(), "connect failure");
    ioc.run();
    EXPECT_TRUE(failed);
}

TEST(Tcp, ReadAfterPeerCloseIsEof)
{
    net::io_context ioc;
    TcpServer server(ioc.get_executor(), "127.0.0.1", 0,
                     [](TcpConnection conn) -> Task<void> {
                         co_await conn.write("ab");
                         conn.close();
                     });
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    error_code ec;
    exec::async_scope scope;
    auto clientTask = [&]() -> Task<void> {
        TcpClient client(ioc.get_executor());
        client.setConnectTimeout(5s);
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("127.0.0.1", port));
        // The peer closes after two bytes: asking for more is an error value.
        auto data = co_await client.readExactly(10);
        if (!data)
        {
            ec = data.error();
        }
        server.stop();
    };
    spawnTask(scope, clientTask(), "eof client");
    ioc.run();
    EXPECT_EQ(ec, net::error::eof);
}

TEST(WaitQueue, NotifyWakesWaiter)
{
    net::io_context ioc;
    WaitQueue queue(ioc.get_executor());
    bool woken = false;
    bool timedOut = true;
    exec::async_scope scope;
    auto waiterTask = [&]() -> Task<void> {
        woken = co_await queue.wait();
        timedOut = !co_await queue.wait(20ms);
    };
    spawnTask(scope, waiterTask(), "waiter");
    net::steady_timer kick(ioc, 10ms);
    kick.async_wait([&](error_code) { queue.notifyAll(); });
    ioc.run();
    EXPECT_TRUE(woken);
    EXPECT_TRUE(timedOut);
}
