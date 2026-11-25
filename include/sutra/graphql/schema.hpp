#pragma once

#include "sutra/graphql/ast.hpp"
#include "sutra/graphql/error.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sutra::graphql
{

// Source used by root fields that do not name one explicitly.
inline constexpr const char* defaultSourceName = "memory";
inline constexpr const char* redfishSourceName = "redfish";

struct ArgumentSpec
{
    std::string name;
    std::string typeName;
    bool required = false;
    // Value applied when the caller omits the argument (kept as a string).
    std::string defaultValue;
};

struct FieldSpec
{
    std::string name;
    // Key the field's value is stored under in the JSON a source returns.
    std::string responseKey;
    std::string returnType;
    bool isList = false;
    bool scalar = false;
    bool collectionLink = false;
    std::vector<ArgumentSpec> arguments;
    // Name of the data source that fills this root field.
    std::string source = defaultSourceName;
    // Opaque, source specific configuration ("sourceConfig" schema key). Only
    // the named source interprets it.
    nlohmann::json sourceConfig;
};

struct ObjectSpec
{
    std::string name;
    std::unordered_map<std::string, FieldSpec> fields;
};

// Maps a DBus object/interface/property tuple to one or more GraphQL
// subscription field names that should be woken when the signal fires.
// dbusObject and dbusProperty may be empty to act as wildcards (match any).
// dbusInterface is required.
struct DbusWatcher
{
    std::string dbusObject; // e.g. "/xyz/openbmc_project/state/host0" (empty =
                            // any object)
    std::string dbusInterface; // e.g. "xyz.openbmc_project.State.Host"
    std::string dbusProperty;  // e.g. "CurrentHostState" (empty = any property
                               // on interface)
    std::vector<std::string> graphqlFields; // subscription field names to wake
};

// A JSON driven schema: object types, plus root query and subscription fields.
// The JSON layout is:
//   {"objects":[{"name":..,"fields":[{"name","responseKey","returnType",
//                                     "isList","scalar","arguments"}]}],
//    "queries":[{"name","returnType","isList","scalar","source",
//                "sourceConfig","arguments":[{"name","typeName","required",
//                                             "default"}]}],
//    "subscriptions":[{"name","returnType","isList","scalar","source",
//                      "sourceConfig","arguments":[...]}]
//    "dbusWatchers":[{"dbusObject":"","dbusInterface":"","dbusProperty":"",
//                     "graphqlFields":["..."]}]}
class Schema
{
  public:
    static Result<Schema> fromFile(const std::string& path)
    {
        std::ifstream file(path);
        if (!file)
        {
            return std::unexpected("Cannot open schema file: " + path);
        }
        nlohmann::json doc = nlohmann::json::parse(file, nullptr, false);
        if (doc.is_discarded())
        {
            return std::unexpected("Invalid JSON in schema file: " + path);
        }
        return fromJson(doc);
    }

    static Result<Schema> fromJson(const nlohmann::json& doc)
    {
        try
        {
            Schema schema;
            for (const auto& obj : doc.at("objects"))
            {
                ObjectSpec objectSpec;
                objectSpec.name = obj.at("name").get<std::string>();
                for (const auto& f : obj.at("fields"))
                {
                    FieldSpec fs = loadField(f);
                    objectSpec.fields[fs.name] = std::move(fs);
                }
                schema.addObject(std::move(objectSpec));
            }
            for (const auto& f : doc.value("queries", nlohmann::json::array()))
            {
                FieldSpec fs = loadField(f);
                if (fs.source.empty())
                {
                    return std::unexpected(
                        "Query '" + fs.name + "' has an empty source");
                }
                schema.addRootQuery(std::move(fs));
            }
            for (const auto& f :
                 doc.value("subscriptions", nlohmann::json::array()))
            {
                FieldSpec fs = loadField(f);
                if (fs.source.empty())
                {
                    return std::unexpected(
                        "Subscription '" + fs.name + "' has an empty source");
                }
                schema.addRootSubscription(std::move(fs));
            }
            for (const auto& entry :
                 doc.value("dbusWatchers", nlohmann::json::array()))
            {
                DbusWatcher w;
                w.dbusObject = entry.value("dbusObject", std::string{});
                w.dbusInterface = entry.at("dbusInterface").get<std::string>();
                w.dbusProperty = entry.value("dbusProperty", std::string{});
                w.graphqlFields =
                    entry.at("graphqlFields").get<std::vector<std::string>>();
                schema.addDbusWatcher(std::move(w));
            }
            return schema;
        }
        catch (const std::exception& e)
        {
            return std::unexpected(
                std::string("Schema JSON error: ") + e.what());
        }
    }

    void addObject(ObjectSpec objectSpec)
    {
        objects[objectSpec.name] = std::move(objectSpec);
    }

    void addRootQuery(FieldSpec fieldSpec)
    {
        rootQueries[fieldSpec.name] = std::move(fieldSpec);
    }

    void addRootSubscription(FieldSpec fieldSpec)
    {
        rootSubscriptions[fieldSpec.name] = std::move(fieldSpec);
    }

    void addDbusWatcher(DbusWatcher w)
    {
        dbusWatchers.push_back(std::move(w));
    }

    const std::vector<DbusWatcher>& getDbusWatchers() const
    {
        return dbusWatchers;
    }

    // Mutable access for the startup pass that drops fields whose data source
    // is unavailable (see applySourceRegistry).
    std::unordered_map<std::string, FieldSpec>& mutableRootQueries()
    {
        return rootQueries;
    }

    std::unordered_map<std::string, FieldSpec>& mutableRootSubscriptions()
    {
        return rootSubscriptions;
    }

    const FieldSpec* getRootQueryField(const std::string& name) const
    {
        auto it = rootQueries.find(name);
        return it == rootQueries.end() ? nullptr : &it->second;
    }

    const FieldSpec* getRootSubscriptionField(const std::string& name) const
    {
        auto it = rootSubscriptions.find(name);
        return it == rootSubscriptions.end() ? nullptr : &it->second;
    }

    const ObjectSpec* getObject(const std::string& name) const
    {
        auto it = objects.find(name);
        return it == objects.end() ? nullptr : &it->second;
    }

    Result<void> validateOperation(const Operation& operation) const
    {
        if (operation.type != Operation::Type::Query &&
            operation.type != Operation::Type::Subscription)
        {
            return std::unexpected(
                "Only query and subscription operations are supported");
        }
        for (const FieldSelection& selection : operation.selections)
        {
            const FieldSpec* fieldSpec =
                (operation.type == Operation::Type::Subscription)
                    ? getRootSubscriptionField(selection.name)
                    : getRootQueryField(selection.name);
            if (fieldSpec == nullptr)
            {
                return std::unexpected(
                    (operation.type == Operation::Type::Subscription
                         ? "Unknown subscription field: "
                         : "Unknown field: ") +
                    selection.name);
            }
            if (auto r = validateField(selection, *fieldSpec); !r)
            {
                return r;
            }
        }
        return {};
    }

  private:
    static FieldSpec loadField(const nlohmann::json& f)
    {
        FieldSpec fs;
        fs.name = f.at("name").get<std::string>();
        fs.responseKey = f.value("responseKey", fs.name);
        fs.returnType = f.value("returnType", std::string{});
        fs.isList = f.value("isList", false);
        fs.scalar = f.value("scalar", false);
        fs.collectionLink = f.value("collectionLink", false);
        fs.source = f.value("source", std::string{defaultSourceName});
        fs.sourceConfig = f.value("sourceConfig", nlohmann::json());

        if (f.contains("redfishPath"))
        {
            if (fs.source == defaultSourceName && !f.contains("source"))
            {
                fs.source = "redfish";
            }
            if (fs.sourceConfig.is_null() || fs.sourceConfig.empty())
            {
                fs.sourceConfig = nlohmann::json::object();
                fs.sourceConfig["path"] = f["redfishPath"];
                if (f.contains("expandMembers"))
                {
                    fs.sourceConfig["expandMembers"] = f["expandMembers"];
                }
            }
        }
        else if (f.contains("expandMembers") && fs.sourceConfig.is_object() &&
                 !fs.sourceConfig.contains("expandMembers"))
        {
            fs.sourceConfig["expandMembers"] = f["expandMembers"];
        }

        for (const auto& a : f.value("arguments", nlohmann::json::array()))
        {
            ArgumentSpec as;
            as.name = a.at("name").get<std::string>();
            as.typeName = a.value("typeName", std::string{"String"});
            as.required = a.value("required", false);
            as.defaultValue = a.value("default", std::string{});
            fs.arguments.push_back(std::move(as));
        }
        return fs;
    }

    Result<void> validateField(const FieldSelection& selection,
                               const FieldSpec& fieldSpec) const
    {
        if (auto r = validateArguments(selection, fieldSpec); !r)
        {
            return r;
        }
        if (fieldSpec.scalar)
        {
            if (!selection.selections.empty())
            {
                return std::unexpected(
                    "Scalar field cannot have sub-selections: " +
                    selection.name);
            }
            return {};
        }
        return validateSelections(selection.selections, fieldSpec.returnType);
    }

    Result<void> validateSelections(
        const std::vector<FieldSelection>& selections,
        const std::string& objectTypeName) const
    {
        const ObjectSpec* objectSpec = getObject(objectTypeName);
        if (objectSpec == nullptr)
        {
            return std::unexpected("Unknown object type: " + objectTypeName);
        }
        for (const FieldSelection& selection : selections)
        {
            auto fieldIt = objectSpec->fields.find(selection.name);
            if (fieldIt == objectSpec->fields.end())
            {
                return std::unexpected("Unknown field '" + selection.name +
                                       "' on type '" + objectTypeName + "'");
            }
            if (auto r = validateField(selection, fieldIt->second); !r)
            {
                return r;
            }
        }
        return {};
    }

    static Result<void> validateArguments(const FieldSelection& field,
                                          const FieldSpec& fieldSpec)
    {
        for (const Argument& argument : field.arguments)
        {
            bool found = false;
            for (const ArgumentSpec& spec : fieldSpec.arguments)
            {
                if (spec.name == argument.name)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return std::unexpected("Unknown argument '" + argument.name +
                                       "' on field '" + field.name + "'");
            }
        }
        for (const ArgumentSpec& spec : fieldSpec.arguments)
        {
            if (!spec.required)
            {
                continue;
            }
            bool given = false;
            for (const Argument& argument : field.arguments)
            {
                given = given || argument.name == spec.name;
            }
            if (!given)
            {
                return std::unexpected(
                    "Missing required argument '" + spec.name + "' on field '" +
                    field.name + "'");
            }
        }
        return {};
    }

    std::unordered_map<std::string, FieldSpec> rootQueries;
    std::unordered_map<std::string, FieldSpec> rootSubscriptions;
    std::unordered_map<std::string, ObjectSpec> objects;
    std::vector<DbusWatcher> dbusWatchers;
};

} // namespace sutra::graphql
