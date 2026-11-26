#pragma once

/**
 * @file redfish_source.hpp
 * @brief GraphQL DataSource that fills root fields from a Redfish service.
 *
 * Behaviour is described by the field's `sourceConfig`:
 * @code
 * "source": "redfish",
 * "sourceConfig": {
 *   "path": "/redfish/v1/Chassis/{chassisId}/Sensors",
 *   "expandMembers": true
 * }
 * @endcode
 *
 * - `path` (required): the resource to GET. `{name}` placeholders are
 *   replaced by the field's arguments (percent-encoded, so an argument can
 *   never add path segments or a query). Every placeholder must name a
 *   declared argument; schema defaults are applied before the source runs.
 * - `expandMembers` (list fields only): appends `?$expand=*($levels=1)` so the
 *   service inlines the collection members in one response.
 *
 * A list field returns `{"Members":[...]}`. Members that arrive as bare
 * `{"@odata.id": ...}` links (no expand, or a service that ignores it) are
 * fetched individually, a few at a time; a member that cannot be fetched is
 * skipped with a warning. An object field returns the resource body. The
 * executor then projects the requested fields, so the schema's `responseKey`
 * values are the Redfish property names.
 *
 * RedfishWebBackend talks to a real service through WebClient; tests supply
 * their own RedfishBackend.
 */

#include "sutra/gather.hpp"
#include "sutra/graphql/data_source.hpp"
#include "sutra/log.hpp"
#include "sutra/web_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sutra::graphql
{

/**
 * @brief The one Redfish operation RedfishSource needs. Tests supply a fake.
 *
 * Failures are reported as `Result` errors, not exceptions.
 */
class RedfishBackend
{
  public:
    virtual ~RedfishBackend() = default;

    /** @param target Absolute path plus optional query, e.g. `/redfish/v1`. */
    virtual Task<Result<nlohmann::json>> get(std::string target) = 0;
};

/** @brief Credentials sent with every request; at most one kind is used. */
struct RedfishCredentials
{
    /** Session token, sent as `X-Auth-Token`. Wins over user/password. */
    std::string token;
    /** HTTP Basic authentication. */
    std::string username;
    std::string password;
};

namespace redfish_source_detail
{

/** Members fetched at the same time when a collection has only links. */
inline constexpr size_t maxConcurrentMembers = 4;

inline std::string base64(std::string_view in)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i + 1 == in.size())
    {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == in.size())
    {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

/** @brief Names of the `{placeholders}` in `pathTemplate`, or an error. */
inline Result<std::vector<std::string>> placeholders(
    const std::string& pathTemplate)
{
    std::vector<std::string> names;
    size_t pos = 0;
    while ((pos = pathTemplate.find_first_of("{}", pos)) != std::string::npos)
    {
        if (pathTemplate[pos] == '}')
        {
            return std::unexpected(
                "unbalanced '}' in path '" + pathTemplate + "'");
        }
        const size_t close = pathTemplate.find('}', pos);
        if (close == std::string::npos ||
            pathTemplate.find('{', pos + 1) < close)
        {
            return std::unexpected(
                "unbalanced '{' in path '" + pathTemplate + "'");
        }
        std::string name = pathTemplate.substr(pos + 1, close - pos - 1);
        if (name.empty())
        {
            return std::unexpected(
                "empty placeholder in path '" + pathTemplate + "'");
        }
        names.push_back(std::move(name));
        pos = close + 1;
    }
    return names;
}

/** @brief Substitutes the percent-encoded arguments into `pathTemplate`. */
inline Result<std::string> expandPath(const std::string& pathTemplate,
                                      const nlohmann::json& args)
{
    std::string out;
    size_t pos = 0;
    while (pos < pathTemplate.size())
    {
        const size_t open = pathTemplate.find('{', pos);
        if (open == std::string::npos)
        {
            out.append(pathTemplate, pos, std::string::npos);
            break;
        }
        out.append(pathTemplate, pos, open - pos);
        const size_t close = pathTemplate.find('}', open);
        if (close == std::string::npos)
        {
            return std::unexpected("unbalanced '{' in path");
        }
        const std::string name =
            pathTemplate.substr(open + 1, close - open - 1);
        auto it = args.find(name);
        if (it == args.end() || it->is_null())
        {
            return std::unexpected("missing argument '" + name + "'");
        }
        if (it->is_string())
        {
            out += sutra::details::percentEncode(it->get<std::string>());
        }
        else if (it->is_number() || it->is_boolean())
        {
            out += sutra::details::percentEncode(it->dump());
        }
        else
        {
            return std::unexpected(
                "argument '" + name + "' must be a string or a number");
        }
        pos = close + 1;
    }
    return out;
}

struct RedfishSourceConfig
{
    std::string path;
    bool expandMembers = false;
};

inline Result<RedfishSourceConfig> parseConfig(const FieldSpec& fieldSpec)
{
    const nlohmann::json& json = fieldSpec.sourceConfig;
    if (!json.is_object())
    {
        return std::unexpected("redfish source needs a sourceConfig object");
    }
    RedfishSourceConfig config;
    auto path = json.find("path");
    if (path == json.end() || !path->is_string() ||
        path->get<std::string>().empty())
    {
        return std::unexpected("sourceConfig.path must be a non-empty string");
    }
    config.path = path->get<std::string>();
    if (config.path.front() != '/')
    {
        return std::unexpected("sourceConfig.path must start with '/'");
    }
    auto expand = json.find("expandMembers");
    if (expand != json.end())
    {
        if (!expand->is_boolean())
        {
            return std::unexpected("sourceConfig.expandMembers must be a bool");
        }
        config.expandMembers = expand->get<bool>();
    }

    auto names = placeholders(config.path);
    if (!names)
    {
        return std::unexpected(names.error());
    }
    for (const auto& name : *names)
    {
        bool declared = false;
        for (const ArgumentSpec& argument : fieldSpec.arguments)
        {
            declared = declared || argument.name == name;
        }
        if (!declared)
        {
            return std::unexpected(
                "path placeholder '{" + name +
                "}' is not an argument of field '" + fieldSpec.name + "'");
        }
    }
    return config;
}

inline bool isLinkOnly(const nlohmann::json& member)
{
    return member.is_object() && member.size() == 1 &&
           member.contains("@odata.id") && member["@odata.id"].is_string();
}

/**
 * @brief GET through the request cache: a URL already fetched during this
 *        request is answered from `cache`; only successes are cached.
 *
 * A free coroutine: all arguments live in its frame (a lambda coroutine's
 * closure could be destroyed before the frame uses it).
 */
inline Task<Result<nlohmann::json>> cachedGet(
    std::shared_ptr<RedfishBackend> backend, RequestCache& cache,
    std::string target)
{
    const std::string key = "redfish:" + target;
    if (auto it = cache.responses.find(key); it != cache.responses.end())
    {
        co_return it->second;
    }
    auto result = co_await backend->get(target);
    if (result)
    {
        cache.responses.emplace(key, *result);
    }
    co_return result;
}

// A free coroutine: all arguments live in its frame (a lambda coroutine's
// closure could be destroyed before the frame uses it).
inline Task<std::optional<nlohmann::json>> fetchMember(
    std::shared_ptr<RedfishBackend> backend, RequestCache& cache,
    std::string target)
{
    auto result = co_await cachedGet(std::move(backend), cache, target);
    if (!result)
    {
        logWarning("redfish: skipping member '{}': {}", target, result.error());
        co_return std::nullopt;
    }
    co_return std::move(*result);
}

} // namespace redfish_source_detail

/** @brief DataSource that fills a root field from a Redfish service. */
class RedfishSource : public DataSource
{
  public:
    explicit RedfishSource(std::shared_ptr<RedfishBackend> redfishBackend) :
        backend(std::move(redfishBackend))
    {}

    Result<void> validate(const FieldSpec& fieldSpec) const override
    {
        auto config = redfish_source_detail::parseConfig(fieldSpec);
        if (!config)
        {
            return std::unexpected(config.error());
        }
        return {};
    }

    Task<Result<nlohmann::json>> fetch(const FieldSpec& fieldSpec,
                                       const nlohmann::json& args,
                                       RequestCache& cache) override
    {
        auto config = redfish_source_detail::parseConfig(fieldSpec);
        if (!config)
        {
            co_return std::unexpected(config.error());
        }
        auto target = redfish_source_detail::expandPath(config->path, args);
        if (!target)
        {
            co_return std::unexpected(target.error());
        }
        if (fieldSpec.isList && config->expandMembers)
        {
            *target += target->find('?') == std::string::npos ? '?' : '&';
            *target += "$expand=*($levels=1)";
        }

        auto payload =
            co_await redfish_source_detail::cachedGet(backend, cache, *target);
        if (!payload)
        {
            co_return std::unexpected(payload.error());
        }
        if (!fieldSpec.isList)
        {
            co_return std::move(*payload);
        }

        auto membersIt = payload->find("Members");
        if (!payload->is_object() || membersIt == payload->end() ||
            !membersIt->is_array())
        {
            co_return std::unexpected(
                "Redfish collection '" + *target + "' has no Members array");
        }
        nlohmann::json& members = *membersIt;

        // Members that are bare links are fetched; the rest are used as is.
        std::vector<size_t> linkIndexes;
        std::vector<Task<std::optional<nlohmann::json>>> fetches;
        for (size_t i = 0; i < members.size(); ++i)
        {
            if (redfish_source_detail::isLinkOnly(members[i]))
            {
                linkIndexes.push_back(i);
                fetches.push_back(redfish_source_detail::fetchMember(
                    backend, cache,
                    members[i]["@odata.id"].get<std::string>()));
            }
        }
        auto fetched = co_await gather(
            std::move(fetches), redfish_source_detail::maxConcurrentMembers);
        for (size_t i = 0; i < linkIndexes.size(); ++i)
        {
            // A failed link becomes null and is dropped below.
            members[linkIndexes[i]] =
                fetched[i] ? std::move(*fetched[i]) : nlohmann::json(nullptr);
        }

        nlohmann::json result = nlohmann::json::array();
        for (auto& member : members)
        {
            if (!member.is_null())
            {
                result.push_back(std::move(member));
            }
        }
        co_return nlohmann::json{{"Members", std::move(result)}};
    }

    Task<Result<nlohmann::json>> fetchLink(const std::string& target,
                                           RequestCache& cache) override
    {
        auto payload =
            co_await redfish_source_detail::cachedGet(backend, cache, target);
        if (!payload)
        {
            co_return std::unexpected(payload.error());
        }
        if (payload->is_object() && payload->contains("Members") &&
            (*payload)["Members"].is_array())
        {
            nlohmann::json& members = (*payload)["Members"];
            std::vector<size_t> linkIndexes;
            std::vector<Task<std::optional<nlohmann::json>>> fetches;
            for (size_t i = 0; i < members.size(); ++i)
            {
                if (redfish_source_detail::isLinkOnly(members[i]))
                {
                    linkIndexes.push_back(i);
                    fetches.push_back(redfish_source_detail::fetchMember(
                        backend, cache,
                        members[i]["@odata.id"].get<std::string>()));
                }
            }
            if (!fetches.empty())
            {
                auto fetched = co_await gather(
                    std::move(fetches),
                    redfish_source_detail::maxConcurrentMembers);
                for (size_t i = 0; i < linkIndexes.size(); ++i)
                {
                    members[linkIndexes[i]] =
                        fetched[i] ? std::move(*fetched[i])
                                   : nlohmann::json(nullptr);
                }
            }
            nlohmann::json result = nlohmann::json::array();
            for (auto& member : members)
            {
                if (!member.is_null())
                {
                    result.push_back(std::move(member));
                }
            }
            (*payload)["Members"] = std::move(result);
        }
        co_return std::move(*payload);
    }

  private:
    std::shared_ptr<RedfishBackend> backend;
};

/**
 * @brief RedfishBackend over a pooled WebClient.
 *
 * TLS peers are always verified (see makeClientContext). Use the HTTPS
 * variant for any service that is not on a trusted local network.
 */
template <class Stream>
class RedfishWebBackend : public RedfishBackend
{
  public:
    using Client = WebClientT<Stream>;
    using Pool = typename Client::Pool;

    /**
     * @param pool       Connection pool on the io_context that runs queries.
     * @param baseUrl    `http(s)://host[:port]`; the scheme must match
     *                   `Stream` and no path is allowed.
     * @param credentials Optional authentication.
     * @throws std::invalid_argument for a bad URL or credentials.
     */
    RedfishWebBackend(std::shared_ptr<Pool> pool, std::string_view baseUrl,
                      const RedfishCredentials& credentials = {}) :
        pool(std::move(pool))
    {
        ParsedUrl parsed = parseUrl(baseUrl);
        if (parsed.tls != Client::isTls)
        {
            throw std::invalid_argument("url scheme does not match backend");
        }
        if (parsed.target != "/")
        {
            throw std::invalid_argument("base url must not have a path");
        }
        host = std::move(parsed.host);
        port = std::move(parsed.port);
        headers["Accept"] = "application/json";
        if (!credentials.token.empty())
        {
            headers["X-Auth-Token"] = credentials.token;
        }
        else if (!credentials.username.empty())
        {
            headers["Authorization"] =
                "Basic " +
                redfish_source_detail::base64(
                    credentials.username + ":" + credentials.password);
        }
    }

    Task<Result<nlohmann::json>> get(std::string target) override
    {
        try
        {
            Client client(pool);
            client.withHost(host).withPort(port).withTarget(target).withHeaders(
                headers);
            HttpResponse res = co_await client.execute();
            if (res.result_int() < 200 || res.result_int() > 299)
            {
                co_return std::unexpected(
                    "Redfish GET " + target + " returned HTTP " +
                    std::to_string(res.result_int()));
            }
            nlohmann::json body =
                nlohmann::json::parse(res.body(), nullptr, false);
            if (body.is_discarded())
            {
                co_return std::unexpected(
                    "Redfish GET " + target + " returned invalid JSON");
            }
            co_return body;
        }
        catch (const std::exception& e)
        {
            co_return std::unexpected(
                "Redfish GET " + target + " failed: " + e.what());
        }
    }

  private:
    std::shared_ptr<Pool> pool;
    std::string host;
    std::string port;
    std::map<std::string, std::string> headers;
};

/** @brief Plain HTTP Redfish backend. */
using RedfishHttpBackend = RedfishWebBackend<tcp::socket>;

/** @brief HTTPS Redfish backend (certificates are always verified). */
using RedfishHttpsBackend = RedfishWebBackend<ssl::stream<tcp::socket>>;

} // namespace sutra::graphql
