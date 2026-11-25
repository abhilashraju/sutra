#pragma once

#include "sutra/config.hpp"
#include "sutra/graphql/error.hpp"
#include "sutra/graphql/schema.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sutra::graphql
{

/**
 * @brief A pluggable back end that fills a root query field.
 *
 * The executor looks the field's `FieldSpec::source` up in a
 * `SourceRegistry` and delegates. A source returns JSON whose keys equal the
 * object types' `responseKey` values; list fields are returned as
 * `{"Members":[...]}`.
 */
class DataSource
{
  public:
    virtual ~DataSource() = default;

    /**
     * @brief Called once at startup for every field selecting this source.
     * @return An error when `FieldSpec::sourceConfig` is unusable, so a bad
     *         schema fails at startup rather than at query time.
     */
    virtual Result<void> validate(const FieldSpec& fieldSpec) const = 0;

    /**
     * @brief Resolves one root field.
     * @param args Resolved query arguments (schema defaults applied).
     *
     * `fieldSpec` and `args` are owned by the caller and outlive the task.
     */
    virtual Task<Result<nlohmann::json>> fetch(const FieldSpec& fieldSpec,
                                               const nlohmann::json& args) = 0;

    /**
     * @brief Resolves a link (e.g. for collectionLink traversal).
     * @param target Absolute URL/path to fetch.
     */
    virtual Task<Result<nlohmann::json>> fetchLink(
        const std::string& /*target*/)
    {
        co_return std::unexpected(
            "Link traversal not supported by this data source");
    }
};

/** @brief Name to DataSource lookup. */
class SourceRegistry
{
  public:
    void add(const std::string& name, std::shared_ptr<DataSource> source)
    {
        sources[name] = std::move(source);
    }

    std::shared_ptr<DataSource> get(const std::string& name) const
    {
        auto it = sources.find(name);
        return it == sources.end() ? nullptr : it->second;
    }

    bool has(const std::string& name) const
    {
        return sources.contains(name);
    }

  private:
    std::unordered_map<std::string, std::shared_ptr<DataSource>> sources;
};

/**
 * @brief Startup pass over a loaded schema.
 *
 * Root fields whose source is missing from `registry` are either an error
 * (`strict`) or dropped with a warning appended to `warnings`. The remaining
 * fields run their source's `validate()`.
 */
inline Result<void> applySourceRegistry(
    Schema& schema, const SourceRegistry& registry, bool strict,
    std::vector<std::string>& warnings)
{
    auto process = [&](auto& fieldMap, const char* kind) -> Result<void> {
        std::vector<std::string> toDrop;
        for (const auto& [name, fieldSpec] : fieldMap)
        {
            if (!registry.has(fieldSpec.source))
            {
                std::string message =
                    std::string(kind) + " field '" + name +
                    "' uses unavailable source '" + fieldSpec.source + "'";
                if (strict)
                {
                    return std::unexpected(message);
                }
                warnings.push_back(message + "; field dropped");
                toDrop.push_back(name);
                continue;
            }
            if (auto r = registry.get(fieldSpec.source)->validate(fieldSpec);
                !r)
            {
                return std::unexpected(
                    std::string(kind) + " field '" + name + "': " + r.error());
            }
        }
        for (const auto& name : toDrop)
        {
            fieldMap.erase(name);
        }
        return {};
    };

    if (auto r = process(schema.mutableRootQueries(), "query"); !r)
    {
        return r;
    }
    return process(schema.mutableRootSubscriptions(), "subscription");
}

/**
 * @brief Serves fixed JSON documents, keyed by root field name.
 *
 * A list field's stored array is wrapped as `{"Members":[...]}`. A non-list
 * field whose stored value is an array is looked up by
 * `sourceConfig.idArg` (default `id`) against `sourceConfig.idKey`
 * (default `Id`).
 */
class MemorySource : public DataSource
{
  public:
    void set(const std::string& fieldName, nlohmann::json value)
    {
        data[fieldName] = std::move(value);
    }

    Result<void> validate(const FieldSpec& fieldSpec) const override
    {
        if (!data.contains(fieldSpec.name))
        {
            return std::unexpected(
                "no data registered for '" + fieldSpec.name + "'");
        }
        return {};
    }

    Task<Result<nlohmann::json>> fetch(const FieldSpec& fieldSpec,
                                       const nlohmann::json& args) override
    {
        auto it = data.find(fieldSpec.name);
        if (it == data.end())
        {
            co_return std::unexpected("no data for '" + fieldSpec.name + "'");
        }
        const nlohmann::json& stored = it->second;
        if (fieldSpec.isList)
        {
            if (!stored.is_array())
            {
                co_return std::unexpected(
                    "list field '" + fieldSpec.name + "' needs an array");
            }
            co_return nlohmann::json{{"Members", stored}};
        }
        if (!stored.is_array())
        {
            co_return stored;
        }
        std::string idArg = "id";
        std::string idKey = "Id";
        if (fieldSpec.sourceConfig.is_object())
        {
            idArg = fieldSpec.sourceConfig.value("idArg", idArg);
            idKey = fieldSpec.sourceConfig.value("idKey", idKey);
        }
        if (!args.contains(idArg) || !args[idArg].is_string())
        {
            co_return std::unexpected("missing argument '" + idArg + "'");
        }
        const std::string wanted = args[idArg].get<std::string>();
        for (const auto& item : stored)
        {
            if (item.is_object() && item.contains(idKey) &&
                item[idKey].is_string() && item[idKey] == wanted)
            {
                co_return item;
            }
        }
        co_return std::unexpected("'" + wanted + "' not found");
    }

    Task<Result<nlohmann::json>> fetchLink(const std::string& target) override
    {
        auto it = data.find(target);
        if (it != data.end())
        {
            co_return it->second;
        }
        co_return std::unexpected("no data for link '" + target + "'");
    }

  private:
    std::unordered_map<std::string, nlohmann::json> data;
};

} // namespace sutra::graphql
