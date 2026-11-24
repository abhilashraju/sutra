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
        std::string data = co_await conn.readSome();
        if (data.empty())
        {
            break;
        }
        co_await conn.write(data);
    }
}

} // namespace

TEST(Tcp, EchoRoundTrip)
{
    net::io_context ioc;
    TcpServer server(ioc.get_executor(), "127.0.0.1", 0, echoSession);
    server.start();
    auto port = std::to_string(server.localEndpoint().port());

    std::string reply1;
    std::string reply2;
    exec::async_scope scope;
    // The closure must outlive the coroutine: its captures live in it.
    auto clientTask = [&]() -> Task<void> {
        TcpClient client(ioc.get_executor());
        client.setConnectTimeout(5s);
        client.setTimeout(5s);
        co_await client.connect("127.0.0.1", port);
        co_await client.write("hello\nworld\n");
        reply1 = co_await client.readUntil("\n");
        reply2 = co_await client.readUntil("\n");
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
    EXPECT_THROW(TcpServer(ioc.get_executor(), "0.0.0.0", 0,
                           [](TcpConnection) -> Task<void> { co_return; }),
                 std::invalid_argument);
}

TEST(Tcp, ConnectFailureThrows)
{
    net::io_context ioc;
    bool threw = false;
    exec::async_scope scope;
    auto connectTask = [&]() -> Task<void> {
        TcpClient client(ioc.get_executor());
        client.setConnectTimeout(2s);
        try
        {
            // Port 1 on loopback is not listening.
            co_await client.connect("127.0.0.1", "1");
        }
        catch (const system_error&)
        {
            threw = true;
        }
    };
    spawnTask(scope, connectTask(), "connect failure");
    ioc.run();
    EXPECT_TRUE(threw);
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
