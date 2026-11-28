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
 * @return success, or the resolve/connect failure (`timed_out` on timeout).
 */
template <class Stream>
Task<Expected<void>> connectStream(
    Connection<Stream>& conn, net::any_io_executor executor, std::string host,
    std::string port, std::optional<std::chrono::steady_clock::duration> limit)
{
    tcp::resolver resolver(executor);
    auto [resolveEc, endpoints] =
        co_await resolver.async_resolve(host, port, useSenderNoThrow);
    if (resolveEc == net::error::operation_aborted)
    {
        co_await ex::just_stopped();
    }
    if (resolveEc)
    {
        co_return std::unexpected(resolveEc);
    }
    conn.arm(limit);
    auto result =
        co_await net::async_connect(conn.lowest(), endpoints, useSenderNoThrow);
    if (auto failure = co_await conn.checkOp(std::get<0>(result)))
    {
        co_return std::unexpected(failure);
    }
    co_return Expected<void>();
}

class TcpClient : public TcpConnection
{
  public:
    explicit TcpClient(net::any_io_executor ioExecutor) :
        TcpConnection(tcp::socket(ioExecutor)), executor(std::move(ioExecutor))
    {}

    /**
     * @brief Resolves `host` and connects to `port` (a number or a service).
     * @return success, or the resolve/connect failure (`timed_out` on
     *         timeout).
     */
    Task<Expected<void>> connect(std::string host, std::string port)
    {
        return connectStream(*this, executor, std::move(host), std::move(port),
                             connectTimeout);
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
