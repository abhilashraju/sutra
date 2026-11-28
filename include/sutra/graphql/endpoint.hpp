#pragma once

#include "sutra/config.hpp"
#include "sutra/graphql/executor.hpp"
#include "sutra/http_router.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <memory>
#include <string>

namespace sutra::graphql
{

struct EndpointOptions
{
    /** Route registered for POST (and GET). */
    std::string path = "/graphql";
    /** Route registered for SSE subscriptions. */
    std::string subscribePath = "/graphql/subscribe";
    /** Route registered for external event injection. */
    std::string eventsPath = "/graphql/events";
    /** Route registered for subscription stats. */
    std::string statsPath = "/graphql/subscriptions/stats";
    /** Largest accepted query text, in bytes. */
    std::size_t maxQueryBytes = 64 * 1024;
    /** Largest accepted `variables` document, in bytes. */
    std::size_t maxVariablesBytes = 64 * 1024;
    /** Also serve `GET <path>?query=...&variables=...`. */
    bool allowGet = true;
    /** Also serve subscriptions over SSE. */
    bool allowSubscriptions = true;
    /** Minimum poll interval in seconds for timer trigger. */
    int minIntervalSecs = 5;
    /** Maximum poll interval in seconds for timer trigger. */
    int maxIntervalSecs = 300;
    /** Default poll interval in seconds for timer trigger. */
    int defaultIntervalSecs = 5;
};

namespace details
{

inline HttpResponse badRequest(const std::string& message)
{
    nlohmann::json body{
        {"errors", nlohmann::json::array({{{"message", message}}})}};
    return jsonResponse(http::status::bad_request, body.dump());
}

inline std::expected<int, std::string> parseInterval(
    const std::string& intervalStr, int minSecs, int maxSecs, int defaultSecs)
{
    if (intervalStr.empty())
    {
        return defaultSecs;
    }
    const char* p = intervalStr.c_str();
    if (*p == '-' || *p == '+')
    {
        ++p;
    }
    if (*p == '\0')
    {
        return std::unexpected("Invalid 'interval': must be an integer");
    }
    for (; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
        {
            return std::unexpected("Invalid 'interval': must be an integer");
        }
    }
    // A leading '+' is accepted by the check above but not by from_chars.
    const char* first = intervalStr.c_str();
    if (*first == '+')
    {
        ++first;
    }
    int v = 0;
    const char* last = intervalStr.c_str() + intervalStr.size();
    if (std::from_chars(first, last, v).ec != std::errc())
    {
        return std::unexpected("Invalid 'interval': integer out of range");
    }
    return std::clamp(v, minSecs, maxSecs);
}

/**
 * Validates the pieces of a request and runs the query. Failures of the
 * request itself (not of the query) yield 400. Query failures are reported
 * inside a 200 response as `{"errors":[...]}`.
 */
inline Task<HttpResponse> runRequest(std::shared_ptr<Executor> executor,
                                     EndpointOptions options, std::string query,
                                     nlohmann::json variables)
{
    if (query.empty())
    {
        co_return badRequest("Missing 'query'");
    }
    if (query.size() > options.maxQueryBytes)
    {
        co_return badRequest("Query too large");
    }
    if (!variables.is_null() && !variables.is_object())
    {
        co_return badRequest("'variables' must be a JSON object");
    }
    if (variables.is_null())
    {
        variables = nlohmann::json::object();
    }
    nlohmann::json result =
        co_await executor->execute(std::move(query), std::move(variables));
    co_return jsonResponse(
        http::status::ok,
        result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

} // namespace details

/**
 * @brief Registers the GraphQL HTTP endpoint on `router`.
 *
 * - `POST path` with a JSON body `{"query": "...", "variables": {...}}`
 *   (or `Content-Type: application/graphql` with the query as the body).
 * - `GET path?query=...&variables=<json>` when `options.allowGet` is set.
 *
 * The response is `{"data":...}` or `{"errors":[{"message":...}]}`.
 */
inline void registerGraphqlEndpoint(HttpRouter& router,
                                    std::shared_ptr<Executor> executor,
                                    EndpointOptions options = {})
{
    router.post(
        options.path,
        [executor,
         options](HttpRequest& req, RequestContext&) -> Task<HttpResponse> {
            const std::string& body = req.body();
            if (body.size() > options.maxQueryBytes + options.maxVariablesBytes)
            {
                co_return details::badRequest("Request too large");
            }
            auto contentType = req[http::field::content_type];
            if (contentType.starts_with("application/graphql"))
            {
                co_return co_await details::runRequest(
                    executor, options, body, nlohmann::json::object());
            }
            nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object())
            {
                co_return details::badRequest(
                    "Body must be a JSON object with a 'query' member");
            }
            auto queryIt = doc.find("query");
            if (queryIt == doc.end() || !queryIt->is_string())
            {
                co_return details::badRequest("Missing 'query' string");
            }
            nlohmann::json variables = doc.value("variables", nlohmann::json());
            if (variables.dump().size() > options.maxVariablesBytes)
            {
                co_return details::badRequest("Variables too large");
            }
            co_return co_await details::runRequest(
                executor, options, queryIt->get<std::string>(),
                std::move(variables));
        });

    if (options.allowGet)
    {
        router.get(
            options.path,
            [executor,
             options](HttpRequest&, RequestContext& ctx) -> Task<HttpResponse> {
                nlohmann::json variables;
                std::string raw = ctx.queryParam("variables");
                if (raw.size() > options.maxVariablesBytes)
                {
                    co_return details::badRequest("Variables too large");
                }
                if (!raw.empty())
                {
                    variables = nlohmann::json::parse(raw, nullptr, false);
                    if (variables.is_discarded())
                    {
                        co_return details::badRequest(
                            "'variables' is not valid JSON");
                    }
                }
                co_return co_await details::runRequest(
                    executor, options, ctx.queryParam("query"),
                    std::move(variables));
            });
    }

    if (options.allowSubscriptions)
    {
        router.sse(
            options.subscribePath,
            [executor, options](HttpRequest&, RequestContext& ctx,
                                SseWriter& writer) -> Task<void> {
                std::string query = ctx.queryParam("query");
                std::string triggerParam = ctx.queryParam("trigger");
                std::string intervalStr = ctx.queryParam("interval");

                if (query.empty())
                {
                    nlohmann::json err{
                        {"errors",
                         nlohmann::json::array(
                             {{{"message",
                                "Missing 'query' query-string parameter"}}})}};
                    co_await writer.write(err.dump());
                    co_return;
                }

                if (query.size() > options.maxQueryBytes)
                {
                    nlohmann::json err{
                        {"errors", nlohmann::json::array(
                                       {{{"message", "Query too large"}}})}};
                    co_await writer.write(err.dump());
                    co_return;
                }

                std::shared_ptr<SubscriptionTrigger> trigger;
                auto exec = executor->getAnyExecutor();
                if (!exec)
                {
                    nlohmann::json err{
                        {"errors",
                         nlohmann::json::array({{{"message", exec.error()}}})}};
                    co_await writer.write(err.dump());
                    co_return;
                }
                if (triggerParam == "event")
                {
                    trigger = std::make_shared<EventTrigger>(*exec);
                }
                else
                {
                    auto maybeInterval = details::parseInterval(
                        intervalStr, options.minIntervalSecs,
                        options.maxIntervalSecs, options.defaultIntervalSecs);
                    if (!maybeInterval)
                    {
                        nlohmann::json err{
                            {"errors",
                             nlohmann::json::array(
                                 {{{"message", maybeInterval.error()}}})}};
                        co_await writer.write(err.dump());
                        co_return;
                    }
                    trigger = std::make_shared<TimerTrigger>(
                        *exec, std::chrono::seconds(*maybeInterval));
                }

                nlohmann::json variables;
                std::string rawVars = ctx.queryParam("variables");
                if (rawVars.size() > options.maxVariablesBytes)
                {
                    nlohmann::json err{
                        {"errors",
                         nlohmann::json::array(
                             {{{"message", "Variables too large"}}})}};
                    co_await writer.write(err.dump());
                    co_return;
                }
                if (!rawVars.empty())
                {
                    variables = nlohmann::json::parse(rawVars, nullptr, false);
                    if (variables.is_discarded() || !variables.is_object())
                    {
                        nlohmann::json err{
                            {"errors",
                             nlohmann::json::array(
                                 {{{"message",
                                    "'variables' is not a valid JSON object"}}})}};
                        co_await writer.write(err.dump());
                        co_return;
                    }
                }
                else
                {
                    variables = nlohmann::json::object();
                }

                co_await executor->executeSubscription(
                    std::move(query), std::move(variables), std::move(trigger),
                    [&writer](nlohmann::json event) -> Task<bool> {
                        co_return co_await writer.write(event.dump());
                    });
            });

        router.post(
            options.eventsPath,
            [executor](HttpRequest& req,
                       RequestContext&) -> Task<HttpResponse> {
                nlohmann::json body =
                    nlohmann::json::parse(req.body(), nullptr, false);

                if (body.is_discarded() || !body.is_object() ||
                    !body.contains("fields") || !body["fields"].is_array())
                {
                    co_return details::badRequest(
                        "Request body must be JSON object with a 'fields' array");
                }

                std::unordered_set<std::string> fields;
                for (const auto& item : body["fields"])
                {
                    if (item.is_string())
                    {
                        fields.insert(item.get<std::string>());
                    }
                }

                if (fields.empty())
                {
                    co_return details::badRequest(
                        "'fields' array must contain at least one string");
                }

                executor->notifyFieldsChanged(fields);

                nlohmann::json response = {{"fired", true},
                                           {"fields", nlohmann::json(fields)}};
                co_return jsonResponse(http::status::ok, response.dump());
            });

        router.get(
            options.statsPath,
            [executor](HttpRequest&, RequestContext&) -> Task<HttpResponse> {
                nlohmann::json stats = executor->getSubscriptionStats();
                co_return jsonResponse(http::status::ok, stats.dump());
            });
    }
}

} // namespace sutra::graphql
