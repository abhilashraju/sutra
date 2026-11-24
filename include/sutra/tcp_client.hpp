/**
 * @file tcp_client.hpp
 * @brief Plain TCP client built on Connection.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/connection.hpp"

#include <string>

namespace sutra
{

/**
 * @brief Resolves `host` and connects the lowest layer of `conn` to `port`.
 * @throws system_error on resolve/connect failure or timeout.
 */
template <class Stream>
Task<void> connectStream(
    Connection<Stream>& conn, net::any_io_executor executor, std::string host,
    std::string port, std::optional<std::chrono::steady_clock::duration> limit)
{
    tcp::resolver resolver(executor);
    auto endpoints = co_await resolver.async_resolve(host, port, useSender);
    conn.arm(limit);
    auto result =
        co_await net::async_connect(conn.lowest(), endpoints, useSenderNoThrow);
    co_await conn.finishOp(std::get<0>(result));
}

class TcpClient : public TcpConnection
{
  public:
    explicit TcpClient(net::any_io_executor ioExecutor) :
        TcpConnection(tcp::socket(ioExecutor)), executor(std::move(ioExecutor))
    {}

    /**
     * @brief Resolves `host` and connects to `port` (a number or a service).
     * @throws system_error on resolve/connect failure or timeout.
     */
    Task<void> connect(std::string host, std::string port)
    {
        co_await connectStream(*this, executor, std::move(host),
                               std::move(port), connectTimeout);
    }

    /** @brief Timeout for connect(); read/write use setTimeout(). */
    void setConnectTimeout(std::optional<Duration> value)
    {
        connectTimeout = value;
    }

  private:
    net::any_io_executor executor;
    std::optional<Duration> connectTimeout;
};

} // namespace sutra
