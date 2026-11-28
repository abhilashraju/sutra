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
#include "sutra/log.hpp"
#include "sutra/tcp_client.hpp"

#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
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
     *        certificate-name verification against `host`, or against
     *        `tlsServerName` when that is not empty. An explicit
     *        `tlsServerName` also accepts a certificate that has no DNS/IP
     *        SAN and whose subject CN equals it.
     * @return success or the failure. When a certificate is rejected the
     *         code is the TLS error and the reason (OpenSSL's, or the name
     *         that did not match) is logged as a warning.
     */
    Task<Expected<void>> connect(
        std::string host, std::string port,
        [[maybe_unused]] std::string tlsServerName = {})
    {
        auto verifyFailure = std::make_shared<std::string>();
        if constexpr (isTls)
        {
            // Set SNI and make the handshake verify the certificate name.
            const std::string name =
                tlsServerName.empty() ? host : tlsServerName;
            if (!SSL_set_tlsext_host_name(this->stream().native_handle(),
                                          name.c_str()))
            {
                co_return std::unexpected(
                    error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category()));
            }
            this->stream().set_verify_callback(verifyAndRecordReason(
                name, verifyFailure, !tlsServerName.empty()));
        }
        auto connected = co_await connectStream(
            *this, executor, host, std::move(port), connectTimeout);
        if (!connected)
        {
            co_return std::unexpected(connected.error());
        }
        if constexpr (isTls)
        {
            auto shaken = co_await this->handshake(ssl::stream_base::client,
                                                   connectTimeout);
            if (!shaken)
            {
                if (!verifyFailure->empty())
                {
                    logWarning("tls handshake with {} failed: {}", host,
                               *verifyFailure);
                }
                co_return std::unexpected(shaken.error());
            }
        }
        peerHost = std::move(host);
        co_return Expected<void>();
    }

    /**
     * @brief Sends `req` and returns the full response.
     *
     * Adds `Host` when missing and prepares the payload. The connection stays
     * usable afterwards only when `response.keep_alive()` is true.
     * @return the response, or the transport/protocol failure.
     */
    Task<Expected<HttpResponse>> send(HttpRequest req)
    {
        prepare(req);
        auto written = co_await this->run([&](auto token) {
            return http::async_write(this->stream(), req, token);
        });
        if (!written)
        {
            co_return std::unexpected(written.error());
        }
        http::response_parser<http::string_body> parser;
        parser.body_limit(maxBodyBytes);
        auto read = co_await this->run([&](auto token) {
            return http::async_read(this->stream(), this->buffer(), parser,
                                    token);
        });
        if (!read)
        {
            co_return std::unexpected(read.error());
        }
        co_return parser.release();
    }

    /**
     * @brief Sends `req`, then delivers server-sent events to `onEvent` until
     *        it returns false or the stream ends.
     * @return the response status, or the failure.
     */
    Task<Expected<http::status>> subscribe(HttpRequest req,
                                           EventHandler onEvent)
    {
        prepare(req);
        req.set(http::field::accept, "text/event-stream");
        auto written = co_await this->run([&](auto token) {
            return http::async_write(this->stream(), req, token);
        });
        if (!written)
        {
            co_return std::unexpected(written.error());
        }

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

        auto header = co_await this->run([&](auto token) {
            return http::async_read_header(this->stream(), this->buffer(),
                                           parser, token);
        });
        if (!header)
        {
            co_return std::unexpected(header.error());
        }
        while (!parser.is_done() && !stop)
        {
            auto chunk = co_await this->run([&](auto token) {
                return http::async_read_some(this->stream(), this->buffer(),
                                             parser, token);
            });
            if (!chunk)
            {
                co_return std::unexpected(chunk.error());
            }
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

    /** @return "subject=<one line> san=<DNS names>" of `cert`, for errors. */
    static std::string describeCertificate(X509* cert)
    {
        std::string out = "subject=";
        char* subject =
            X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        if (subject != nullptr)
        {
            out += subject;
            OPENSSL_free(subject);
        }
        out += " san=";
        GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
        if (names == nullptr)
        {
            return out + "none";
        }
        for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i)
        {
            const GENERAL_NAME* gen = sk_GENERAL_NAME_value(names, i);
            if (gen->type == GEN_DNS)
            {
                const ASN1_IA5STRING* dns = gen->d.dNSName;
                out.append(
                    reinterpret_cast<const char*>(ASN1_STRING_get0_data(dns)),
                    static_cast<size_t>(ASN1_STRING_length(dns)));
                out += ',';
            }
            else if (gen->type == GEN_IPADD)
            {
                out += "IP,";
            }
        }
        GENERAL_NAMES_free(names);
        return out;
    }

    /** @return true when `cert` has a DNS or IP subject alternative name. */
    static bool hasDnsOrIpSan(X509* cert)
    {
        GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
        if (names == nullptr)
        {
            return false;
        }
        bool found = false;
        for (int i = 0; i < sk_GENERAL_NAME_num(names) && !found; ++i)
        {
            const int type = sk_GENERAL_NAME_value(names, i)->type;
            found = type == GEN_DNS || type == GEN_IPADD;
        }
        GENERAL_NAMES_free(names);
        return found;
    }

    /**
     * @brief Legacy subject-CN match for certificates without a DNS/IP SAN.
     *
     * Newer OpenSSL releases (4.1) no longer fall back to the CN, but some
     * BMC certificates are issued with only `CN=<name>`. Used only for a name
     * the operator configured explicitly; exact, case-insensitive, no
     * wildcards. The chain has already been verified by then.
     */
    static bool commonNameEquals(X509* cert, const std::string& name)
    {
        if (hasDnsOrIpSan(cert))
        {
            return false;
        }
        const auto* subject = X509_get_subject_name(cert);
        for (int i = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
             i >= 0; i = X509_NAME_get_index_by_NID(subject, NID_commonName, i))
        {
            unsigned char* utf8 = nullptr;
            const int length = ASN1_STRING_to_UTF8(
                &utf8,
                X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, i)));
            if (length < 0)
            {
                continue;
            }
            const std::string_view cn(reinterpret_cast<char*>(utf8),
                                      static_cast<size_t>(length));
            const bool same =
                std::equal(cn.begin(), cn.end(), name.begin(), name.end(),
                           [](unsigned char a, unsigned char b) {
                               return std::tolower(a) == std::tolower(b);
                           });
            OPENSSL_free(utf8);
            if (same)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Certificate-name verification for `name` that also records why a
     *        certificate was rejected in `failure` (OpenSSL's own reason for a
     *        chain problem, or the name that did not match).
     * @param allowCommonName also accept a SAN-less certificate whose subject
     *        CN equals `name` (see commonNameEquals()).
     */
    static auto verifyAndRecordReason(std::string name,
                                      std::shared_ptr<std::string> failure,
                                      bool allowCommonName)
    {
        return [name, failure, allowCommonName,
                hostCheck = ssl::host_name_verification(
                    name)](bool preverified, ssl::verify_context& ctx) {
            bool ok = hostCheck(preverified, ctx);
            if (!ok && preverified && allowCommonName &&
                X509_STORE_CTX_get_error_depth(ctx.native_handle()) == 0)
            {
                ok = commonNameEquals(
                    X509_STORE_CTX_get_current_cert(ctx.native_handle()), name);
            }
            if (!ok && failure->empty())
            {
                X509_STORE_CTX* store = ctx.native_handle();
                const int depth = X509_STORE_CTX_get_error_depth(store);
                if (!preverified)
                {
                    *failure = std::string("certificate verify failed: ") +
                               X509_verify_cert_error_string(
                                   X509_STORE_CTX_get_error(store)) +
                               " (depth " + std::to_string(depth) + ")";
                }
                else
                {
                    *failure =
                        "certificate does not match the name '" + name + "' (" +
                        describeCertificate(
                            X509_STORE_CTX_get_current_cert(store)) +
                        ")";
                }
            }
            return ok;
        };
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
