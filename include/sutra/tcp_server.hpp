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
#include <stdexcept>

namespace sutra
{

class TcpServer
{
  public:
    using Handler = std::function<Task<void>(TcpConnection)>;

    /**
     * @param executor  io_context executor the listener and sessions use.
     * @param endpoint  address to listen on; an unspecified address (0.0.0.0
     *                  or ::) is rejected, bind a concrete interface.
     * @param sessionHandler called once per accepted connection.
     */
    TcpServer(net::any_io_executor executor, const tcp::endpoint& endpoint,
              Handler sessionHandler) :
        acceptor(executor), handler(std::move(sessionHandler)),
        timerExecutor(executor)
    {
        if (endpoint.address().is_unspecified())
        {
            throw std::invalid_argument(
                "refusing to listen on an unspecified address");
        }
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(net::socket_base::max_listen_connections);
    }

    TcpServer(net::any_io_executor executor, const std::string& address,
              uint16_t port, Handler sessionHandler) :
        TcpServer(std::move(executor),
                  tcp::endpoint(net::ip::make_address(address), port),
                  std::move(sessionHandler))
    {}

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    ~TcpServer()
    {
        stop();
    }

    /** @brief Begins accepting. */
    void start()
    {
        spawnTask(scope, acceptLoop(), "tcp accept loop");
    }

    /** @brief Closes the listener and asks all sessions to stop. */
    void stop()
    {
        error_code ec;
        acceptor.close(ec);
        scope.request_stop();
    }

    tcp::endpoint localEndpoint() const
    {
        return acceptor.local_endpoint();
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
    exec::async_scope scope;
};

} // namespace sutra
