/**
 * @file tcp_server.hpp
 * @brief Plain TCP accept loop that runs one Task per connection.
 *
 * Lifetime: the server owns an async_scope holding the accept loop and every
 * session. stop() closes the listener and requests stop on all of them; keep
 * the io_context running until it drains. The server object (and the handler
 * it holds) must outlive that.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/connection.hpp"
#include "sutra/log.hpp"
#include "sutra/spawn.hpp"

#include <functional>

namespace sutra
{

class TcpServer
{
  public:
    using Handler = std::function<Task<void>(TcpConnection)>;

    /**
     * Construction never fails; binding happens in start(), which reports
     * the failure.
     *
     * @param executor  io_context executor the listener and sessions use.
     * @param endpoint  address to listen on; an unspecified address (0.0.0.0
     *                  or ::) is rejected by start(), bind a concrete
     *                  interface.
     * @param sessionHandler called once per accepted connection.
     */
    TcpServer(net::any_io_executor executor, const tcp::endpoint& endpoint,
              Handler sessionHandler) :
        acceptor(executor), handler(std::move(sessionHandler)),
        timerExecutor(executor), listenEndpoint(endpoint)
    {}

    /** A malformed `address` is reported by start(). */
    TcpServer(net::any_io_executor executor, const std::string& address,
              uint16_t port, Handler sessionHandler) :
        TcpServer(std::move(executor), tcp::endpoint(),
                  std::move(sessionHandler))
    {
        error_code ec;
        auto parsed = net::ip::make_address(address, ec);
        if (ec)
        {
            startError = ec;
            return;
        }
        listenEndpoint = tcp::endpoint(parsed, port);
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    ~TcpServer()
    {
        stop();
    }

    /**
     * @brief Binds, listens and begins accepting.
     * @return success, `Errc::unspecifiedAddress` for 0.0.0.0 / ::, an
     *         address parse error, or the open/bind/listen failure.
     */
    Expected<void> start()
    {
        if (startError)
        {
            return std::unexpected(startError);
        }
        if (listenEndpoint.address().is_unspecified())
        {
            return std::unexpected(make_error_code(Errc::unspecifiedAddress));
        }
        error_code ec;
        acceptor.open(listenEndpoint.protocol(), ec);
        if (!ec)
        {
            acceptor.set_option(net::socket_base::reuse_address(true), ec);
        }
        if (!ec)
        {
            acceptor.bind(listenEndpoint, ec);
        }
        if (!ec)
        {
            acceptor.listen(net::socket_base::max_listen_connections, ec);
        }
        if (ec)
        {
            error_code ignored;
            acceptor.close(ignored);
            return std::unexpected(ec);
        }
        spawnTask(scope, acceptLoop(), "tcp accept loop");
        return Expected<void>();
    }

    /** @brief Closes the listener and asks all sessions to stop. */
    void stop()
    {
        error_code ec;
        acceptor.close(ec);
        scope.request_stop();
    }

    /** @brief The bound endpoint; a default endpoint before start(). */
    tcp::endpoint localEndpoint() const
    {
        error_code ec;
        auto endpoint = acceptor.local_endpoint(ec);
        return ec ? tcp::endpoint() : endpoint;
    }

  private:
    Task<void> acceptLoop()
    {
        while (acceptor.is_open())
        {
            auto accepted = co_await acceptor.async_accept(useSenderNoThrow);
            auto& socket = std::get<1>(accepted);
            const error_code ec = std::get<0>(accepted);
            if (ec == net::error::operation_aborted)
            {
                break;
            }
            if (ec)
            {
                logWarning("accept failed: {}", ec.message());
                // Avoid spinning on persistent errors such as EMFILE.
                net::steady_timer backoff(timerExecutor, 100ms);
                co_await backoff.async_wait(useSenderNoThrow);
                continue;
            }
            spawnTask(scope, handler(TcpConnection(std::move(socket))),
                      "tcp session");
        }
    }

    tcp::acceptor acceptor;
    Handler handler;
    net::any_io_executor timerExecutor;
    tcp::endpoint listenEndpoint;
    error_code startError;
    exec::async_scope scope;
};

} // namespace sutra
