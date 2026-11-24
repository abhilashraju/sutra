#pragma once

#include "sutra/config.hpp"
#include "sutra/graphql/executor.hpp"
#include "sutra/http_router.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace sutra::graphql
{

struct EndpointOptions
{
    /** Route registered for POST (and GET). */
    std::string path = "/graphql";
    /** Largest accepted query text, in bytes. */
    std::size_t maxQueryBytes = 64 * 1024;
    /** Largest accepted `variables` document, in bytes. */
    std::size_t maxVariablesBytes = 64 * 1024;
    /** Also serve `GET <path>?query=...&variables=...`. */
    bool allowGet = true;
};

namespace details
{

inline HttpResponse badRequest(const std::string& message)
{
    nlohmann::json body{
        {"errors", nlohmann::json::array({{{"message", message}}})}};
    return jsonResponse(http::status::bad_request, body.dump());
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

    if (!options.allowGet)
    {
        return;
    }
    router.get(options.path,
               [executor, options](HttpRequest&,
                                   RequestContext& ctx) -> Task<HttpResponse> {
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

} // namespace sutra::graphql
