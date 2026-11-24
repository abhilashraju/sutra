#pragma once

#include "sutra/config.hpp"
#include "sutra/graphql/data_source.hpp"
#include "sutra/graphql/error.hpp"
#include "sutra/graphql/parser.hpp"
#include "sutra/graphql/schema.hpp"
#include "sutra/graphql/util.hpp"
#include "sutra/log.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace sutra::graphql
{

/**
 * @brief Runs GraphQL queries against a Schema and a SourceRegistry.
 *
 * Supports queries with aliases, arguments, variables (with defaults),
 * named and inline fragments and nested objects/lists. `execute()` never
 * throws for query errors: failures are reported as
 * `{"errors":[{"message":...}]}`. Cancellation (a stop request) propagates
 * as a stopped completion.
 *
 * Root fields are resolved sequentially, in query order.
 */
class Executor
{
  public:
    Executor(Schema schemaIn, std::shared_ptr<const SourceRegistry> registry) :
        schema(std::move(schemaIn)), sources(std::move(registry))
    {}

    /**
     * @brief Executes `query`. `variables` must be a JSON object (or null).
     */
    Task<nlohmann::json> execute(
        std::string query, nlohmann::json variables = nlohmann::json::object())
    {
        Result<Operation> parsed = Parser::tryParse(query);
        if (!parsed)
        {
            co_return errorResponse(parsed.error());
        }
        Operation operation = std::move(*parsed);
        try
        {
            expandFragments(operation);
        }
        catch (const std::exception& e)
        {
            co_return errorResponse(e.what());
        }
        if (auto r = schema.validateOperation(operation); !r)
        {
            co_return errorResponse(r.error());
        }

        nlohmann::json merged = initialVariables(operation);
        if (variables.is_object())
        {
            for (auto it = variables.begin(); it != variables.end(); ++it)
            {
                merged[it.key()] = it.value();
            }
        }

        nlohmann::json data = nlohmann::json::object();
        for (const FieldSelection& selection : operation.selections)
        {
            Result<nlohmann::json> field =
                co_await resolveRootField(selection, merged);
            if (!field)
            {
                co_return errorResponse(field.error());
            }
            data[outputName(selection)] = std::move(*field);
        }
        co_return nlohmann::json{{"data", std::move(data)}};
    }

    const Schema& getSchema() const
    {
        return schema;
    }

  private:
    static std::string outputName(const FieldSelection& selection)
    {
        return selection.alias.empty() ? selection.name : selection.alias;
    }

    static nlohmann::json errorResponse(const std::string& message)
    {
        return nlohmann::json{
            {"errors", nlohmann::json::array({{{"message", message}}})}};
    }

    // Schema defaults first, then the caller's non-null values on top.
    static nlohmann::json applyArgumentDefaults(const nlohmann::json& args,
                                                const FieldSpec& fieldSpec)
    {
        nlohmann::json resolved = nlohmann::json::object();
        for (const ArgumentSpec& spec : fieldSpec.arguments)
        {
            if (!spec.defaultValue.empty())
            {
                resolved[spec.name] = spec.defaultValue;
            }
        }
        for (auto it = args.begin(); it != args.end(); ++it)
        {
            if (!it.value().is_null())
            {
                resolved[it.key()] = it.value();
            }
        }
        return resolved;
    }

    Task<Result<nlohmann::json>> resolveRootField(
        const FieldSelection& selection, const nlohmann::json& variables)
    {
        // validateOperation() already proved the field exists.
        const FieldSpec& fieldSpec = *schema.getRootQueryField(selection.name);
        std::shared_ptr<DataSource> source =
            sources ? sources->get(fieldSpec.source) : nullptr;
        if (!source)
        {
            co_return std::unexpected(
                "Data source '" + fieldSpec.source + "' required by field '" +
                fieldSpec.name + "' is not available");
        }

        const nlohmann::json args = applyArgumentDefaults(
            argumentsToJson(selection.arguments, variables), fieldSpec);

        Result<nlohmann::json> payload =
            std::unexpected(std::string("source did not run"));
        try
        {
            payload = co_await source->fetch(fieldSpec, args);
        }
        catch (const std::exception& e)
        {
            logWarning("graphql: source '{}' failed for '{}': {}",
                       fieldSpec.source, fieldSpec.name, e.what());
            co_return std::unexpected("Failed to resolve field '" +
                                      fieldSpec.name + "': " + e.what());
        }
        if (!payload)
        {
            co_return std::unexpected(payload.error());
        }

        if (fieldSpec.isList)
        {
            if (!payload->is_object() || !payload->contains("Members") ||
                !(*payload)["Members"].is_array())
            {
                co_return std::unexpected(
                    "Expected collection Members array for '" + fieldSpec.name +
                    "'");
            }
            co_return projectList((*payload)["Members"], fieldSpec.returnType,
                                  selection.selections);
        }
        if (fieldSpec.scalar)
        {
            co_return std::move(*payload);
        }
        co_return projectObject(*payload, fieldSpec.returnType,
                                selection.selections);
    }

    Result<nlohmann::json> projectList(
        const nlohmann::json& items, const std::string& typeName,
        const std::vector<FieldSelection>& selections) const
    {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& item : items)
        {
            Result<nlohmann::json> projected =
                projectObject(item, typeName, selections);
            if (!projected)
            {
                return projected;
            }
            result.push_back(std::move(*projected));
        }
        return result;
    }

    Result<nlohmann::json> projectField(const FieldSelection& selection,
                                        const FieldSpec& fieldSpec,
                                        const nlohmann::json& source) const
    {
        auto it = source.find(fieldSpec.responseKey);
        if (it == source.end())
        {
            return nlohmann::json(nullptr);
        }
        const nlohmann::json& value = *it;
        if (fieldSpec.scalar)
        {
            return value;
        }
        if (value.is_null())
        {
            return value;
        }
        if (fieldSpec.isList)
        {
            if (!value.is_array())
            {
                return std::unexpected(
                    "Expected array for field '" + selection.name + "'");
            }
            return projectList(value, fieldSpec.returnType,
                               selection.selections);
        }
        return projectObject(value, fieldSpec.returnType, selection.selections);
    }

    Result<nlohmann::json> projectObject(
        const nlohmann::json& source, const std::string& typeName,
        const std::vector<FieldSelection>& selections) const
    {
        const ObjectSpec* objectSpec = schema.getObject(typeName);
        if (objectSpec == nullptr)
        {
            return std::unexpected("Unknown object type: " + typeName);
        }
        if (!source.is_object())
        {
            return std::unexpected(
                "Expected object for type '" + typeName + "'");
        }
        if (selections.empty())
        {
            return source;
        }
        nlohmann::json result = nlohmann::json::object();
        for (const FieldSelection& selection : selections)
        {
            auto fieldIt = objectSpec->fields.find(selection.name);
            if (fieldIt == objectSpec->fields.end())
            {
                return std::unexpected("Unknown field '" + selection.name +
                                       "' on type '" + typeName + "'");
            }
            Result<nlohmann::json> field =
                projectField(selection, fieldIt->second, source);
            if (!field)
            {
                return field;
            }
            result[outputName(selection)] = std::move(*field);
        }
        return result;
    }

    Schema schema;
    std::shared_ptr<const SourceRegistry> sources;
};

} // namespace sutra::graphql
