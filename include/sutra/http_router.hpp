/**
 * @file http_router.hpp
 * @brief Path router with `{name}` parameters, plain and SSE routes.
 *
 * Handlers are either synchronous (`HttpResponse(HttpRequest&,
 * RequestContext&)`) or tasks (`Task<HttpResponse>(...)`). The router and the
 * handlers it stores must outlive the server that uses them.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/http_target.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sutra
{

/** @brief Builds a response with a body and content type. */
inline HttpResponse makeResponse(http::status status, std::string body,
                                 std::string_view contentType = "text/plain")
{
    HttpResponse res{status, 11};
    res.set(http::field::content_type, contentType);
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

inline HttpResponse jsonResponse(http::status status, std::string body)
{
    return makeResponse(status, std::move(body), "application/json");
}

/** @brief Write-only handle given to SSE handlers. */
class SseWriter
{
  public:
    using WriteFn = std::function<Task<bool>(std::string)>;

    explicit SseWriter(WriteFn fn) : writeFn(std::move(fn)) {}

    /**
     * @brief Sends one event whose data is `data` (multi-line data is split
     *        into several `data:` lines).
     * @return false when the client is gone.
     */
    Task<bool> write(std::string data)
    {
        return writeFn(std::move(data));
    }

  private:
    WriteFn writeFn;
};

class HttpRouter
{
  public:
    using Handler =
        std::function<Task<HttpResponse>(HttpRequest&, RequestContext&)>;
    using SseHandler =
        std::function<Task<void>(HttpRequest&, RequestContext&, SseWriter&)>;

    /** @brief Registers `handler` for `verb` + `pattern` (`/a/{id}/b`). */
    template <class F>
    void add(http::verb verb, std::string_view pattern, F handler)
    {
        Route route{verb, split(pattern), {}, {}};
        if constexpr (std::is_invocable_r_v<HttpResponse, const F&,
                                            HttpRequest&, RequestContext&>)
        {
            route.handler = [handler = std::move(handler)](HttpRequest& req,
                                                           RequestContext& ctx)
                -> Task<HttpResponse> { co_return handler(req, ctx); };
        }
        else
        {
            route.handler = std::move(handler);
        }
        routes.push_back(std::move(route));
    }

    template <class F>
    void get(std::string_view pattern, F handler)
    {
        add(http::verb::get, pattern, std::move(handler));
    }

    template <class F>
    void post(std::string_view pattern, F handler)
    {
        add(http::verb::post, pattern, std::move(handler));
    }

    template <class F>
    void put(std::string_view pattern, F handler)
    {
        add(http::verb::put, pattern, std::move(handler));
    }

    template <class F>
    void del(std::string_view pattern, F handler)
    {
        add(http::verb::delete_, pattern, std::move(handler));
    }

    /** @brief Registers a GET Server-Sent-Events stream. */
    void sse(std::string_view pattern, SseHandler handler)
    {
        sseRoutes.push_back(
            {http::verb::get, split(pattern), {}, std::move(handler)});
    }

    /** @brief Handler used when no route matches (default: 404). */
    template <class F>
    void setFallback(F handler)
    {
        Route route{http::verb::get, {}, {}, {}};
        if constexpr (std::is_invocable_r_v<HttpResponse, const F&,
                                            HttpRequest&, RequestContext&>)
        {
            route.handler = [handler = std::move(handler)](HttpRequest& req,
                                                           RequestContext& ctx)
                -> Task<HttpResponse> { co_return handler(req, ctx); };
        }
        else
        {
            route.handler = std::move(handler);
        }
        fallback = std::move(route);
    }

    /** @brief Finds the SSE route for `ctx.path`; fills `ctx.params`. */
    const SseHandler* findSse(const HttpRequest& req, RequestContext& ctx) const
    {
        if (req.method() != http::verb::get)
        {
            return nullptr;
        }
        for (const auto& route : sseRoutes)
        {
            if (match(route, ctx))
            {
                return &route.sse;
            }
        }
        return nullptr;
    }

    /** @brief Runs the matching handler (404/405 when there is none). */
    Task<HttpResponse> dispatch(HttpRequest& req, RequestContext& ctx) const
    {
        bool pathKnown = false;
        for (const auto& route : routes)
        {
            RequestContext probe;
            probe.path = ctx.path;
            if (!match(route, probe))
            {
                continue;
            }
            if (route.verb == req.method())
            {
                ctx.params = std::move(probe.params);
                co_return co_await route.handler(req, ctx);
            }
            pathKnown = true;
        }
        if (pathKnown)
        {
            co_return makeResponse(http::status::method_not_allowed,
                                   "Method Not Allowed");
        }
        if (fallback && fallback->handler)
        {
            co_return co_await fallback->handler(req, ctx);
        }
        co_return makeResponse(http::status::not_found,
                               "Not Found " + ctx.path);
    }

  private:
    struct Route
    {
        http::verb verb;
        std::vector<std::string> segments;
        Handler handler;
        SseHandler sse;
    };

    static std::vector<std::string> split(std::string_view path)
    {
        std::vector<std::string> out;
        while (!path.empty())
        {
            auto slash = path.find('/');
            auto part = path.substr(0, slash);
            if (!part.empty())
            {
                out.emplace_back(part);
            }
            path = slash == std::string_view::npos ? std::string_view()
                                                   : path.substr(slash + 1);
        }
        return out;
    }

    static bool match(const Route& route, RequestContext& ctx)
    {
        auto parts = split(ctx.path);
        if (parts.size() != route.segments.size())
        {
            return false;
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const auto& pattern = route.segments[i];
            if (pattern.size() > 2 && pattern.front() == '{' &&
                pattern.back() == '}')
            {
                ctx.params[pattern.substr(1, pattern.size() - 2)] = parts[i];
            }
            else if (pattern != parts[i])
            {
                return false;
            }
        }
        return true;
    }

    std::vector<Route> routes;
    std::vector<Route> sseRoutes;
    std::optional<Route> fallback;
};

} // namespace sutra
