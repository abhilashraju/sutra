/**
 * @file web_client.hpp
 * @brief Fluent HTTP/HTTPS client on top of ConnectionPool.
 *
 * @code
 * auto pool = HttpConnectionPool::create(ioc.get_executor());
 * WebClient client(pool);
 * auto res = co_await client.withUrl("http://127.0.0.1:8080/items?id=3")
 *                           .withMethod(http::verb::get)
 *                           .execute();
 * @endcode
 *
 * execute() snapshots the request, so the WebClient may be a temporary or
 * be reused (and modified) while earlier requests are still in flight.
 *
 * Errors are values: execute() returns `Expected<HttpResponse>`. The fluent
 * `with*` setters cannot report a failure without breaking the chain, so a
 * bad URL is remembered and returned by the next execute()/buildRequest().
 * HTTP error statuses are responses, not errors.
 *
 * Retries: a request is attempted up to `maxTries` times with exponential
 * backoff. Retried are connect failures (always), transport failures while
 * exchanging (idempotent verbs; other verbs only when a reused keep-alive
 * connection turned out to be stale) and HTTP 5xx responses of idempotent
 * verbs. Protocol errors and TLS failures, in particular certificate
 * verification failures, are never retried. After the last attempt the last
 * 5xx response is returned, or the last error.
 *
 * TLS peers are always verified: the pool's ssl::context decides the trust
 * anchors (makeClientContext) and the certificate name is checked against the
 * host of the URL.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/connection_pool.hpp"
#include "sutra/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sutra
{

/** @brief Result of parseUrl(). */
struct ParsedUrl
{
    bool tls = false;
    std::string host;
    std::string port;
    /** Path plus query, always starting with '/'. */
    std::string target;
};

namespace details
{

inline bool hasControlOrSpace(std::string_view text)
{
    return std::ranges::any_of(text, [](char c) {
        return static_cast<unsigned char>(c) <= 0x20 ||
               static_cast<unsigned char>(c) == 0x7f;
    });
}

/** @brief Escapes everything except RFC 3986 unreserved characters. */
inline std::string percentEncode(std::string_view in)
{
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    for (char c : in)
    {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '-' || c == '.' || c == '_' ||
            c == '~')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 0x0f]);
        }
    }
    return out;
}

inline bool isIdempotent(http::verb method)
{
    switch (method)
    {
        case http::verb::get:
        case http::verb::head:
        case http::verb::put:
        case http::verb::delete_:
        case http::verb::options:
        case http::verb::trace:
            return true;
        default:
            return false;
    }
}

} // namespace details

/**
 * @brief Splits an http(s) URL into its parts.
 * @return the parts, or `Errc::unsupportedScheme` for other schemes,
 *         `Errc::missingHost` for an empty host, `Errc::invalidUrl` for user
 *         info, bad ports, bad authorities or control characters.
 */
inline Expected<ParsedUrl> parseUrl(std::string_view url)
{
    ParsedUrl out;
    if (details::hasControlOrSpace(url))
    {
        return std::unexpected(make_error_code(Errc::invalidUrl));
    }
    constexpr std::string_view httpScheme = "http://";
    constexpr std::string_view httpsScheme = "https://";
    if (url.starts_with(httpsScheme))
    {
        out.tls = true;
        url.remove_prefix(httpsScheme.size());
    }
    else if (url.starts_with(httpScheme))
    {
        url.remove_prefix(httpScheme.size());
    }
    else
    {
        return std::unexpected(make_error_code(Errc::unsupportedScheme));
    }
    if (auto fragment = url.find('#'); fragment != std::string_view::npos)
    {
        url = url.substr(0, fragment);
    }
    auto authorityEnd = url.find_first_of("/?");
    std::string_view authority = url.substr(0, authorityEnd);
    std::string_view rest =
        authorityEnd == std::string_view::npos ? "" : url.substr(authorityEnd);
    if (authority.find('@') != std::string_view::npos)
    {
        return std::unexpected(make_error_code(Errc::invalidUrl));
    }

    std::string_view portText;
    if (authority.starts_with('['))
    {
        auto close = authority.find(']');
        if (close == std::string_view::npos)
        {
            return std::unexpected(make_error_code(Errc::invalidUrl));
        }
        out.host = std::string(authority.substr(1, close - 1));
        authority.remove_prefix(close + 1);
        if (!authority.empty())
        {
            if (!authority.starts_with(':'))
            {
                return std::unexpected(make_error_code(Errc::invalidUrl));
            }
            portText = authority.substr(1);
        }
    }
    else
    {
        auto colon = authority.find(':');
        out.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos)
        {
            portText = authority.substr(colon + 1);
        }
    }
    if (out.host.empty())
    {
        return std::unexpected(make_error_code(Errc::missingHost));
    }
    if (portText.empty())
    {
        out.port = out.tls ? "443" : "80";
    }
    else
    {
        unsigned value = 0;
        auto [end, ec] = std::from_chars(
            portText.data(), portText.data() + portText.size(), value);
        if (ec != std::errc() || end != portText.data() + portText.size() ||
            value == 0 || value > 65535)
        {
            return std::unexpected(make_error_code(Errc::invalidUrl));
        }
        out.port = std::string(portText);
    }

    out.target = rest.empty() ? "/" : std::string(rest);
    if (out.target.front() == '?')
    {
        out.target.insert(0, "/");
    }
    return out;
}

template <class Stream>
class WebClientT
{
  public:
    using Pool = ConnectionPoolT<Stream>;
    using Duration = std::chrono::steady_clock::duration;
    using EventHandler = typename HttpClientT<Stream>::EventHandler;

    static constexpr bool isTls = Pool::isTls;

    struct RetryPolicy
    {
        /** Total attempts including the first one. */
        int maxTries = 3;
        Duration initialBackoff = 100ms;
        Duration maxBackoff = 2s;
        /** Retry 5xx responses of idempotent verbs. */
        bool retryServerErrors = true;
    };

    /** @throws std::invalid_argument for a null pool (a programming error). */
    explicit WebClientT(std::shared_ptr<Pool> connectionPool) :
        pool(std::move(connectionPool))
    {
        if (!pool)
        {
            throw std::invalid_argument("WebClient needs a pool");
        }
        port = isTls ? "443" : "80";
    }

    WebClientT& withHost(std::string value)
    {
        host = std::move(value);
        return *this;
    }

    WebClientT& withPort(std::string value)
    {
        port = std::move(value);
        return *this;
    }

    WebClientT& withMethod(http::verb value)
    {
        method = value;
        return *this;
    }

    /** @brief Path (and optional query); must start with '/'. */
    WebClientT& withTarget(std::string value)
    {
        target = std::move(value);
        return *this;
    }

    /** @brief Query parameters, percent-encoded and appended to the target. */
    WebClientT& withParams(std::map<std::string, std::string> value)
    {
        params = std::move(value);
        return *this;
    }

    /** @brief Replaces all extra headers. */
    WebClientT& withHeaders(std::map<std::string, std::string> value)
    {
        headers = std::move(value);
        return *this;
    }

    WebClientT& withHeader(std::string name, std::string value)
    {
        headers[std::move(name)] = std::move(value);
        return *this;
    }

    WebClientT& withBody(std::string value)
    {
        body = std::move(value);
        return *this;
    }

    /** @brief Sets the body to `value.dump()` and Content-Type JSON. */
    WebClientT& withJsonBody(const nlohmann::json& value)
    {
        body = value.dump();
        if (!headers.contains("Content-Type"))
        {
            headers["Content-Type"] = "application/json";
        }
        return *this;
    }

    WebClientT& withKeepAlive(bool value)
    {
        keepAlive = value;
        return *this;
    }

    /** @brief Total attempts, at least 1. */
    WebClientT& withRetries(int maxTries)
    {
        retry.maxTries = std::max(1, maxTries);
        return *this;
    }

    WebClientT& withRetryPolicy(RetryPolicy policy)
    {
        policy.maxTries = std::max(1, policy.maxTries);
        retry = policy;
        return *this;
    }

    /** @brief Per-operation timeout (default: the pool's requestTimeout). */
    WebClientT& withTimeout(Duration value)
    {
        timeout = value;
        return *this;
    }

    /**
     * @brief Sets host, port and target from an http(s) URL.
     *
     * An invalid URL, or one whose scheme does not match this client (https
     * needs the TLS client, `Errc::schemeMismatch`), leaves the request
     * unchanged and is returned by the next execute() or buildRequest().
     */
    WebClientT& withUrl(std::string_view url)
    {
        auto parsed = parseUrl(url);
        if (!parsed)
        {
            urlError = parsed.error();
            return *this;
        }
        if (parsed->tls != isTls)
        {
            urlError = make_error_code(Errc::schemeMismatch);
            return *this;
        }
        urlError = error_code();
        host = std::move(parsed->host);
        port = std::move(parsed->port);
        target = std::move(parsed->target);
        return *this;
    }

    std::shared_ptr<Pool> getPool() const
    {
        return pool;
    }

    /**
     * @brief Builds the request that execute() would send.
     * @return the request, or the error left by withUrl(), `missingHost`,
     *         `invalidTarget` or `invalidHeader`.
     */
    Expected<HttpRequest> buildRequest() const
    {
        if (urlError)
        {
            return std::unexpected(urlError);
        }
        if (host.empty())
        {
            return std::unexpected(make_error_code(Errc::missingHost));
        }
        if (target.empty() || target.front() != '/' ||
            details::hasControlOrSpace(target))
        {
            return std::unexpected(make_error_code(Errc::invalidTarget));
        }
        std::string fullTarget = target;
        bool first = fullTarget.find('?') == std::string::npos;
        for (const auto& [key, value] : params)
        {
            fullTarget += first ? '?' : '&';
            first = false;
            fullTarget += details::percentEncode(key);
            fullTarget += '=';
            fullTarget += details::percentEncode(value);
        }

        HttpRequest req(method, fullTarget, 11);
        req.keep_alive(keepAlive);
        const bool defaultPort = port == (isTls ? "443" : "80");
        std::string authority =
            host.find(':') == std::string::npos ? host : "[" + host + "]";
        if (!defaultPort)
        {
            authority += ':' + port;
        }
        req.set(http::field::host, authority);
        for (const auto& [name, value] : headers)
        {
            if (name.empty() || details::hasControlOrSpace(name) ||
                name.find(':') != std::string::npos ||
                value.find_first_of("\r\n") != std::string::npos)
            {
                return std::unexpected(make_error_code(Errc::invalidHeader));
            }
            req.set(name, value);
        }
        req.body() = body;
        req.prepare_payload();
        return req;
    }

    /**
     * @brief Sends the request and returns the response.
     *
     * HTTP error statuses are responses, not errors. A request that cannot be
     * built, or a transport failure after the retries, is the error.
     */
    Task<Expected<HttpResponse>> execute() const
    {
        auto req = buildRequest();
        if (!req)
        {
            return failed<HttpResponse>(req.error());
        }
        return run(pool, host, port, std::move(*req), retry, timeout);
    }

    /**
     * @brief execute() and parse the body as JSON.
     * @return the JSON, `Errc::invalidJson` for a body that is not JSON, or
     *         the execute() error.
     */
    Task<Expected<nlohmann::json>> executeJson() const
    {
        return jsonOf(execute());
    }

    /**
     * @brief Sends the request as an SSE subscription.
     *
     * `onEvent` receives the data of each event until it returns false or
     * the server ends the stream. The connection is never reused. There is
     * no retry; the idle timeout between events is `withTimeout()` (none by
     * default, since event streams may be quiet for long).
     * @return the response status (check it for 2xx), or the failure.
     */
    Task<Expected<http::status>> executeAsStream(EventHandler onEvent) const
    {
        auto req = buildRequest();
        if (!req)
        {
            return failed<http::status>(req.error());
        }
        return stream(pool, host, port, std::move(*req), std::move(onEvent),
                      timeout);
    }

  private:
    enum class Phase
    {
        connecting,
        exchanging,
    };

    struct Attempt
    {
        Phase phase = Phase::connecting;
        bool reused = false;
    };

    template <class T>
    static Task<Expected<T>> failed(error_code ec)
    {
        co_return std::unexpected(ec);
    }

    static Task<Expected<nlohmann::json>> jsonOf(
        Task<Expected<HttpResponse>> response)
    {
        auto res = co_await std::move(response);
        if (!res)
        {
            co_return std::unexpected(res.error());
        }
        auto body = nlohmann::json::parse(res->body(), nullptr, false);
        if (body.is_discarded())
        {
            co_return std::unexpected(make_error_code(Errc::invalidJson));
        }
        co_return body;
    }

    static bool isRetryable(const error_code& ec, const Attempt& attempt,
                            bool idempotent)
    {
        // Never retry TLS failures such as certificate verification errors.
        if (ec.category() == net::error::get_ssl_category())
        {
            return false;
        }
        if (attempt.phase == Phase::connecting)
        {
            return true;
        }
        const bool endOfStream =
            ec == http::error::end_of_stream || ec == net::error::eof ||
            ec == ssl::error::stream_truncated;
        const bool protocolError =
            ec.category() ==
                http::make_error_code(http::error::bad_method).category() &&
            !endOfStream;
        if (protocolError)
        {
            return false;
        }
        if (idempotent)
        {
            return true;
        }
        // Stale keep-alive connection: the server never saw the request.
        return attempt.reused &&
               (endOfStream || ec == net::error::connection_reset ||
                ec == net::error::broken_pipe);
    }

    static Task<Expected<HttpResponse>> attemptOnce(
        std::shared_ptr<Pool> pool, const std::string& host,
        const std::string& port, const HttpRequest& req,
        std::optional<Duration> timeout, Attempt& attempt)
    {
        attempt = Attempt{};
        auto leased = co_await pool->acquire(host, port);
        if (!leased)
        {
            co_return std::unexpected(leased.error());
        }
        typename Pool::Lease lease = std::move(*leased);
        attempt.phase = Phase::exchanging;
        attempt.reused = lease.reused();
        // Unless the exchange completes cleanly the connection is unusable.
        lease.markBad();
        if (timeout)
        {
            lease->setTimeout(timeout);
        }
        auto res = co_await lease->send(req);
        if (!res)
        {
            co_return std::unexpected(res.error());
        }
        if (res->keep_alive() && req.keep_alive())
        {
            lease.markReusable();
        }
        co_return std::move(*res);
    }

    static Task<Expected<HttpResponse>> run(
        std::shared_ptr<Pool> pool, std::string host, std::string port,
        HttpRequest req, RetryPolicy policy, std::optional<Duration> timeout)
    {
        const bool idempotent = details::isIdempotent(req.method());
        Duration backoff = policy.initialBackoff;
        std::optional<HttpResponse> lastResponse;
        error_code lastError;

        for (int attemptNo = 1; attemptNo <= policy.maxTries; ++attemptNo)
        {
            Attempt attempt;
            auto res =
                co_await attemptOnce(pool, host, port, req, timeout, attempt);
            if (res)
            {
                if (res->result_int() >= 500 && idempotent &&
                    policy.retryServerErrors)
                {
                    lastError = error_code();
                    lastResponse = std::move(*res);
                }
                else
                {
                    co_return res;
                }
            }
            else
            {
                if (!isRetryable(res.error(), attempt, idempotent))
                {
                    co_return res;
                }
                lastResponse.reset();
                lastError = res.error();
            }

            if (attemptNo == policy.maxTries)
            {
                break;
            }
            // target() returns a boost string_view, which std::format cannot
            // format with every Boost version; convert it explicitly.
            logDebug("retry {}/{} for {}:{} {}", attemptNo, policy.maxTries,
                     host, port, std::string_view(req.target()));
            net::steady_timer sleeper(pool->getExecutor());
            sleeper.expires_after(backoff);
            co_await sleeper.async_wait(useSender);
            backoff = std::min(backoff * 2, policy.maxBackoff);
        }

        if (lastError)
        {
            co_return std::unexpected(lastError);
        }
        co_return std::move(*lastResponse);
    }

    static Task<Expected<http::status>> stream(
        std::shared_ptr<Pool> pool, std::string host, std::string port,
        HttpRequest req, EventHandler onEvent, std::optional<Duration> timeout)
    {
        auto leased = co_await pool->acquire(host, port);
        if (!leased)
        {
            co_return std::unexpected(leased.error());
        }
        typename Pool::Lease lease = std::move(*leased);
        lease.markBad();
        lease->setTimeout(timeout);
        req.set(http::field::cache_control, "no-cache");
        co_return co_await lease->subscribe(std::move(req), std::move(onEvent));
    }

    std::shared_ptr<Pool> pool;
    std::string host;
    std::string port;
    http::verb method = http::verb::get;
    std::string target = "/";
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> headers;
    std::string body;
    bool keepAlive = true;
    RetryPolicy retry;
    std::optional<Duration> timeout;
    /** Failure of the last withUrl(); returned by buildRequest(). */
    error_code urlError;
};

/** @brief Plain HTTP client. */
using WebClient = WebClientT<tcp::socket>;

/** @brief HTTPS client (certificates are always verified). */
using SecureWebClient = WebClientT<ssl::stream<tcp::socket>>;

} // namespace sutra
