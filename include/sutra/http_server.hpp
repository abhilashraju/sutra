/**
 * @file http_server.hpp
 * @brief HTTP/1.1 server (plain or TLS) driven by an HttpRouter.
 *
 * Every accepted connection runs as one Task: read request, dispatch through
 * the router, write response, repeat while keep-alive. SSE routes switch the
 * connection to a chunked `text/event-stream` and hand an SseWriter to the
 * route handler.
 *
 * Lifetime: the router (and the ssl::context for TLS) must outlive the
 * server. stop() cancels the accept loop and every session; keep running the
 * io_context until it drains.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/connection.hpp"
#include "sutra/http_router.hpp"
#include "sutra/log.hpp"
#include "sutra/spawn.hpp"
#include "sutra/tcp_server.hpp"

#include <format>
#include <memory>
#include <optional>

namespace sutra
{

struct HttpServerOptions
{
    /** Limit for reading one request and for each response write. */
    std::chrono::steady_clock::duration requestTimeout = 30s;
    /** Limit for the TLS handshake. */
    std::chrono::steady_clock::duration handshakeTimeout = 10s;
    /** Largest accepted request body. */
    size_t maxBodyBytes = 16 * 1024 * 1024;
};

class HttpServer
{
  public:
    /** @brief Plain HTTP server. */
    HttpServer(net::any_io_executor executor, const tcp::endpoint& endpoint,
               const HttpRouter& httpRouter, HttpServerOptions opts = {}) :
        router(httpRouter), options(opts),
        listener(std::move(executor), endpoint, [this](TcpConnection conn) {
            return session(std::move(conn));
        })
    {}

    /** @brief HTTPS server (TLS 1.2+ as configured in `context`). */
    HttpServer(net::any_io_executor executor, const tcp::endpoint& endpoint,
               const HttpRouter& httpRouter, ssl::context& context,
               HttpServerOptions opts = {}) :
        router(httpRouter), options(opts), tlsContext(&context),
        listener(std::move(executor), endpoint, [this](TcpConnection conn) {
            return session(std::move(conn));
        })
    {}

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start()
    {
        listener.start();
    }

    void stop()
    {
        listener.stop();
    }

    tcp::endpoint localEndpoint() const
    {
        return listener.localEndpoint();
    }

  private:
    Task<void> session(TcpConnection plain)
    {
        error_code ec;
        tcp::endpoint remote = plain.lowest().remote_endpoint(ec);
        if (tlsContext == nullptr)
        {
            co_await serve(plain, remote);
            co_return;
        }
        TlsConnection tls(
            ssl::stream<tcp::socket>(std::move(plain.stream()), *tlsContext));
        bool handshakeOk = true;
        try
        {
            co_await tls.handshake(ssl::stream_base::server,
                                   options.handshakeTimeout);
        }
        catch (const system_error& e)
        {
            logDebug("tls handshake with {} failed: {}",
                     remote.address().to_string(), e.code().message());
            handshakeOk = false;
        }
        if (handshakeOk)
        {
            co_await serve(tls, remote);
        }
    }

    enum class ReadOutcome
    {
        request,
        closed,
        tooLarge,
        badRequest,
    };

    template <class Stream>
    Task<ReadOutcome> readRequest(Connection<Stream>& conn,
                                  http::request_parser<http::string_body>& p)
    {
        ReadOutcome outcome = ReadOutcome::request;
        try
        {
            co_await conn.run([&](auto token) {
                return http::async_read(conn.stream(), conn.buffer(), p, token);
            });
        }
        catch (const system_error& e)
        {
            const auto code = e.code();
            if (code == http::error::body_limit)
            {
                outcome = ReadOutcome::tooLarge;
            }
            else if (code == http::error::end_of_stream ||
                     code.category() !=
                         http::make_error_code(http::error::bad_method)
                             .category())
            {
                // Peer went away, timed out, or the transport failed.
                outcome = ReadOutcome::closed;
            }
            else
            {
                outcome = ReadOutcome::badRequest;
            }
        }
        co_return outcome;
    }

    template <class Stream>
    Task<void> serve(Connection<Stream>& conn, tcp::endpoint remote)
    {
        conn.setTimeout(options.requestTimeout);
        for (;;)
        {
            http::request_parser<http::string_body> parser;
            parser.body_limit(options.maxBodyBytes);
            ReadOutcome outcome = co_await readRequest(conn, parser);
            if (outcome == ReadOutcome::closed)
            {
                break;
            }
            if (outcome != ReadOutcome::request)
            {
                auto res = makeResponse(outcome == ReadOutcome::tooLarge
                                            ? http::status::payload_too_large
                                            : http::status::bad_request,
                                        "request rejected");
                res.keep_alive(false);
                co_await send(conn, res);
                break;
            }

            HttpRequest req = parser.release();
            RequestContext ctx;
            ctx.remote = remote;
            parseTarget(req.target(), ctx);

            if (const auto* sse = router.findSse(req, ctx))
            {
                co_await streamEvents(conn, req, ctx, *sse);
                co_return;
            }

            HttpResponse res;
            bool failed = false;
            try
            {
                res = co_await router.dispatch(req, ctx);
            }
            catch (const std::exception& e)
            {
                logError("handler for {} failed: {}", ctx.path, e.what());
                failed = true;
            }
            if (failed)
            {
                res = makeResponse(http::status::internal_server_error,
                                   "Internal Server Error");
            }
            res.version(req.version());
            res.keep_alive(req.keep_alive() && res.keep_alive());
            res.prepare_payload();
            const bool keepAlive = res.keep_alive();
            if (!co_await send(conn, res) || !keepAlive)
            {
                break;
            }
        }
        co_await conn.gracefulClose();
    }

    template <class Stream>
    static Task<bool> send(Connection<Stream>& conn, HttpResponse& res)
    {
        bool ok = true;
        try
        {
            co_await conn.run([&](auto token) {
                return http::async_write(conn.stream(), res, token);
            });
        }
        catch (const system_error& e)
        {
            logDebug("response write failed: {}", e.code().message());
            ok = false;
        }
        co_return ok;
    }

    template <class Stream>
    Task<void> streamEvents(Connection<Stream>& conn, HttpRequest& req,
                            RequestContext& ctx,
                            const HttpRouter::SseHandler& handler)
    {
        static constexpr std::string_view head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n";
        bool headSent = true;
        try
        {
            co_await conn.write(head);
        }
        catch (const system_error&)
        {
            headSent = false;
        }
        if (!headSent)
        {
            co_return;
        }

        SseWriter writer([&conn](std::string data) -> Task<bool> {
            std::string frame;
            size_t start = 0;
            for (;;)
            {
                auto eol = data.find('\n', start);
                frame += "data: ";
                frame.append(data, start,
                             eol == std::string::npos ? eol : eol - start);
                frame += '\n';
                if (eol == std::string::npos)
                {
                    break;
                }
                start = eol + 1;
            }
            frame += '\n';
            std::string chunk = std::format("{:x}\r\n", frame.size());
            chunk += frame;
            chunk += "\r\n";
            bool ok = true;
            try
            {
                co_await conn.write(chunk);
            }
            catch (const system_error&)
            {
                ok = false;
            }
            co_return ok;
        });

        try
        {
            co_await handler(req, ctx, writer);
        }
        catch (const std::exception& e)
        {
            logDebug("sse handler for {} ended: {}", ctx.path, e.what());
        }
        try
        {
            co_await conn.write("0\r\n\r\n");
        }
        catch (const system_error&)
        {}
        co_await conn.gracefulClose();
    }

    const HttpRouter& router;
    HttpServerOptions options;
    ssl::context* tlsContext = nullptr;
    TcpServer listener;
};

} // namespace sutra
