/**
 * @file http_client.hpp
 * @brief HTTP/1.1 client over a plain or TLS Connection.
 *
 * One HttpClientT is one connection. For pooling and retries use
 * ConnectionPool / WebClient, which are built on top of it.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/connection.hpp"
#include "sutra/tcp_client.hpp"

#include <functional>
#include <optional>
#include <string>

namespace sutra
{

namespace details
{

/** @brief Plain connections carry no TLS context; TLS ones reference it. */
template <class Stream>
struct StreamFactory;

template <>
struct StreamFactory<tcp::socket>
{
    static tcp::socket make(net::any_io_executor executor, ssl::context*)
    {
        return tcp::socket(executor);
    }
};

template <>
struct StreamFactory<ssl::stream<tcp::socket>>
{
    static ssl::stream<tcp::socket> make(net::any_io_executor executor,
                                         ssl::context* context)
    {
        return ssl::stream<tcp::socket>(executor, *context);
    }
};

} // namespace details

template <class Stream>
class HttpClientT : public Connection<Stream>
{
  public:
    using Base = Connection<Stream>;
    using Duration = typename Base::Duration;
    /** Called once per SSE event; return false to stop streaming. */
    using EventHandler = std::function<bool(std::string)>;

    static constexpr bool isTls = !std::is_same_v<Stream, tcp::socket>;

    /**
     * @param executor io_context executor.
     * @param context  TLS context (required for TLS clients, ignored
     *                 otherwise). It must outlive the client.
     */
    explicit HttpClientT(net::any_io_executor ioExecutor,
                         ssl::context* context = nullptr) :
        Base(details::StreamFactory<Stream>::make(ioExecutor, context)),
        executor(std::move(ioExecutor))
    {
        if constexpr (isTls)
        {
            if (context == nullptr)
            {
                throw std::invalid_argument("TLS client needs an ssl::context");
            }
        }
    }

    void setConnectTimeout(std::optional<Duration> value)
    {
        connectTimeout = value;
    }

    /** @brief Largest accepted response body (default 64 MiB). */
    void setMaxBodyBytes(size_t value)
    {
        maxBodyBytes = value;
    }

    /**
     * @brief Resolves, connects and (for TLS) handshakes with SNI and
     *        certificate-name verification against `host`.
     */
    Task<void> connect(std::string host, std::string port)
    {
        if constexpr (isTls)
        {
            // Set SNI and make the handshake verify the certificate name.
            if (!SSL_set_tlsext_host_name(this->stream().native_handle(),
                                          host.c_str()))
            {
                throw system_error(
                    error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category()));
            }
            this->stream().set_verify_callback(
                ssl::host_name_verification(host));
        }
        co_await connectStream(*this, executor, host, std::move(port),
                               connectTimeout);
        if constexpr (isTls)
        {
            co_await this->handshake(ssl::stream_base::client, connectTimeout);
        }
        peerHost = std::move(host);
    }

    /**
     * @brief Sends `req` and returns the full response.
     *
     * Adds `Host` when missing and prepares the payload. The connection stays
     * usable afterwards only when `response.keep_alive()` is true.
     */
    Task<HttpResponse> send(HttpRequest req)
    {
        prepare(req);
        co_await this->run([&](auto token) {
            return http::async_write(this->stream(), req, token);
        });
        http::response_parser<http::string_body> parser;
        parser.body_limit(maxBodyBytes);
        co_await this->run([&](auto token) {
            return http::async_read(this->stream(), this->buffer(), parser,
                                    token);
        });
        co_return parser.release();
    }

    /**
     * @brief Sends `req`, then delivers server-sent events to `onEvent` until
     *        it returns false or the stream ends.
     * @return the response status.
     */
    Task<http::status> subscribe(HttpRequest req, EventHandler onEvent)
    {
        prepare(req);
        req.set(http::field::accept, "text/event-stream");
        co_await this->run([&](auto token) {
            return http::async_write(this->stream(), req, token);
        });

        http::response_parser<http::string_body> parser;
        parser.body_limit(boost::none);
        std::string pending;
        bool stop = false;
        auto onChunk = [&](uint64_t, std::string_view body, error_code&) {
            pending.append(body);
            for (auto end = pending.find("\n\n");
                 end != std::string::npos && !stop; end = pending.find("\n\n"))
            {
                stop = !onEvent(eventData(pending.substr(0, end)));
                pending.erase(0, end + 2);
            }
            return body.size();
        };
        parser.on_chunk_body(onChunk);

        co_await this->run([&](auto token) {
            return http::async_read_header(this->stream(), this->buffer(),
                                           parser, token);
        });
        while (!parser.is_done() && !stop)
        {
            co_await this->run([&](auto token) {
                return http::async_read_some(this->stream(), this->buffer(),
                                             parser, token);
            });
        }
        co_return parser.get().result();
    }

  private:
    void prepare(HttpRequest& req)
    {
        if (req.find(http::field::host) == req.end())
        {
            req.set(http::field::host, peerHost);
        }
        req.prepare_payload();
    }

    /** @brief Joins the `data:` lines of one SSE event. */
    static std::string eventData(const std::string& event)
    {
        std::string out;
        size_t start = 0;
        while (start <= event.size())
        {
            auto eol = event.find('\n', start);
            std::string_view line(
                event.data() + start,
                (eol == std::string::npos ? event.size() : eol) - start);
            if (line.starts_with("data:"))
            {
                line.remove_prefix(5);
                if (line.starts_with(' '))
                {
                    line.remove_prefix(1);
                }
                if (!out.empty())
                {
                    out += '\n';
                }
                out.append(line);
            }
            if (eol == std::string::npos)
            {
                break;
            }
            start = eol + 1;
        }
        return out;
    }

    net::any_io_executor executor;
    std::string peerHost;
    std::optional<Duration> connectTimeout;
    size_t maxBodyBytes = 64 * 1024 * 1024;
};

using HttpClient = HttpClientT<tcp::socket>;
using HttpsClient = HttpClientT<ssl::stream<tcp::socket>>;

} // namespace sutra
