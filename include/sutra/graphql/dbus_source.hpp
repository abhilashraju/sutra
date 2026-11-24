#pragma once

#ifdef SUTRA_HAVE_DBUS

/**
 * @file dbus_source.hpp
 * @brief GraphQL DataSource that fills root fields straight from D-Bus.
 *
 * Behaviour is described by the field's `sourceConfig`:
 * @code
 * "source": "dbus",
 * "sourceConfig": {
 *   "subtree": "/xyz/openbmc_project/sensors",
 *   "depth": 0,
 *   "interfaces": ["xyz.openbmc_project.Sensor.Value"],
 *   "idKey": "Id", "idArg": "id",
 *   "fields": {
 *     "Id":   {"from": "path", "transform": "sensorId"},
 *     "Reading": {"interface": "xyz.openbmc_project.Sensor.Value",
 *                 "property": "Value"}
 *   }
 * }
 * @endcode
 *
 * Only available when SUTRA_HAVE_DBUS is defined.
 */

#include "sutra/dbus.hpp"
#include "sutra/gather.hpp"
#include "sutra/graphql/data_source.hpp"
#include "sutra/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sutra::graphql
{

// Name under which DbusSource registers in the SourceRegistry and which
// schema fields select with "source": "dbus".
inline constexpr const char* dbusSourceName = "dbus";

using DbusValue = sutra::DbusVariant;
using DbusPropertyMap = sutra::PropertyMap;
using DbusSubTree = sutra::SubTreeMap;

/**
 * @brief The two D-Bus operations DbusSource needs. Tests supply a fake.
 *
 * Failures are reported as `Result` errors, not exceptions.
 */
class DbusBackend
{
  public:
    virtual ~DbusBackend() = default;

    virtual Task<Result<DbusSubTree>> getSubTree(
        std::string path, int depth, std::vector<std::string> interfaces) = 0;

    virtual Task<Result<DbusPropertyMap>> getAll(
        std::string service, std::string path, std::string interface) = 0;
};

/** @brief DbusBackend that talks to a real bus through sutra/dbus.hpp. */
class SutraDbusBackend : public DbusBackend
{
  public:
    explicit SutraDbusBackend(std::shared_ptr<sdbusplus::asio::connection> c) :
        connection(std::move(c))
    {}

    Task<Result<DbusSubTree>> getSubTree(
        std::string path, int depth,
        std::vector<std::string> interfaces) override
    {
        try
        {
            co_return co_await sutra::getSubTree(*connection, std::move(path),
                                                 depth, std::move(interfaces));
        }
        catch (const std::system_error& e)
        {
            co_return std::unexpected(e.code().message());
        }
    }

    Task<Result<DbusPropertyMap>> getAll(std::string service, std::string path,
                                         std::string interface) override
    {
        try
        {
            co_return co_await sutra::getAllProperties(
                *connection, std::move(service), std::move(path),
                std::move(interface));
        }
        catch (const std::system_error& e)
        {
            co_return std::unexpected(e.code().message());
        }
    }

  private:
    std::shared_ptr<sdbusplus::asio::connection> connection;
};

inline nlohmann::json dbusValueToJson(const DbusValue& value)
{
    return std::visit(
        [](const auto& v) -> nlohmann::json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return nullptr;
            }
            else
            {
                if constexpr (std::is_floating_point_v<T>)
                {
                    if (!std::isfinite(v))
                    {
                        return nullptr;
                    }
                }
                return v;
            }
        },
        value);
}

namespace dbus_transform
{

using Fn = std::function<nlohmann::json(const nlohmann::json&)>;

inline std::string lastComponent(const std::string& text, char separator)
{
    auto pos = text.rfind(separator);
    return pos == std::string::npos ? text : text.substr(pos + 1);
}

// Object path -> final path component.
inline nlohmann::json leaf(const nlohmann::json& in)
{
    if (!in.is_string())
    {
        return nullptr;
    }
    return lastComponent(in.get<std::string>(), '/');
}

// Object path /.../<type>/<name> -> "<type>_<name>", the id bmcweb derives
// for a sensor (underscores removed from the type, rotational_position
// shortened to rotation).
inline nlohmann::json sensorId(const nlohmann::json& in)
{
    if (!in.is_string())
    {
        return nullptr;
    }
    const std::string path = in.get<std::string>();
    auto namePos = path.rfind('/');
    if (namePos == std::string::npos || namePos == 0)
    {
        return nullptr;
    }
    auto typePos = path.rfind('/', namePos - 1);
    if (typePos == std::string::npos)
    {
        return nullptr;
    }
    std::string type = path.substr(typePos + 1, namePos - typePos - 1);
    if (type == "rotational_position")
    {
        type = "rotation";
    }
    type.erase(std::remove(type.begin(), type.end(), '_'), type.end());
    return type + "_" + path.substr(namePos + 1);
}

// Object path -> sensor display name (final component, '_' -> ' ').
inline nlohmann::json sensorName(const nlohmann::json& in)
{
    nlohmann::json name = leaf(in);
    if (!name.is_string())
    {
        return nullptr;
    }
    std::string text = name.get<std::string>();
    std::replace(text.begin(), text.end(), '_', ' ');
    return text;
}

// Sensor.Value.Unit enum string -> Redfish ReadingUnits.
inline nlohmann::json sensorUnit(const nlohmann::json& in)
{
    if (!in.is_string())
    {
        return nullptr;
    }
    static const std::unordered_map<std::string, std::string> units = {
        {"Amperes", "A"}, {"CFM", "cft_i/min"}, {"DegreesC", "Cel"},
        {"Joules", "J"},  {"LPM", "L/min"},     {"Meters", "m"},
        {"Percent", "%"}, {"PercentRH", "%"},   {"Pascals", "Pa"},
        {"RPMS", "RPM"},  {"Volts", "V"},       {"Watts", "W"}};
    auto it = units.find(lastComponent(in.get<std::string>(), '.'));
    if (it == units.end())
    {
        return nullptr;
    }
    return it->second;
}

// D-Bus enum string -> its final dotted component (".Foo.Bar" -> "Bar").
inline nlohmann::json enumLeaf(const nlohmann::json& in)
{
    if (!in.is_string())
    {
        return nullptr;
    }
    return lastComponent(in.get<std::string>(), '.');
}

// OperationalStatus.Functional -> Redfish Status.Health.
inline nlohmann::json functionalToHealth(const nlohmann::json& in)
{
    if (!in.is_boolean())
    {
        return nullptr;
    }
    return in.get<bool>() ? "OK" : "Critical";
}

// Availability.Available -> Redfish Status.State.
inline nlohmann::json availableToState(const nlohmann::json& in)
{
    if (!in.is_boolean())
    {
        return nullptr;
    }
    return in.get<bool>() ? "Enabled" : "UnavailableOffline";
}

inline const Fn* find(const std::string& name)
{
    static const std::unordered_map<std::string, Fn> registry = {
        {"leaf", leaf},
        {"sensorId", sensorId},
        {"sensorName", sensorName},
        {"sensorUnit", sensorUnit},
        {"enumLeaf", enumLeaf},
        {"functionalToHealth", functionalToHealth},
        {"availableToState", availableToState}};
    auto it = registry.find(name);
    return it == registry.end() ? nullptr : &it->second;
}

} // namespace dbus_transform

// One entry of sourceConfig.fields: how to compute one response key.
struct DbusFieldMapping
{
    // Response key. '/' nests: "Status/Health" -> {"Status":{"Health":..}}.
    std::string key;
    bool fromPath = false; // value is the object path (then transformed)
    std::string interface; // property source, when !fromPath
    std::string property;
    std::string transform; // optional, name in dbus_transform registry
};

struct DbusSourceConfig
{
    std::string subtree;                 // may contain {arg} placeholders
    int depth = 0;
    std::vector<std::string> interfaces; // selects the objects
    std::string idKey;                   // response key identifying an object
    std::string idArg;                   // query argument matched against it
    std::vector<DbusFieldMapping> fields;
};

// Parses and validates a field's sourceConfig for the "dbus" source.
inline Result<DbusSourceConfig> parseDbusSourceConfig(
    const nlohmann::json& json, bool isList)
{
    if (!json.is_object())
    {
        return std::unexpected("sourceConfig must be an object");
    }
    DbusSourceConfig config;
    try
    {
        config.subtree = json.at("subtree").get<std::string>();
        config.depth = json.value("depth", 0);
        config.interfaces =
            json.value("interfaces", std::vector<std::string>{});
        config.idKey = json.value("idKey", std::string{});
        config.idArg = json.value("idArg", std::string{});

        const nlohmann::json& fields = json.at("fields");
        if (!fields.is_object() || fields.empty())
        {
            return std::unexpected("'fields' must be a non-empty object");
        }
        for (auto it = fields.begin(); it != fields.end(); ++it)
        {
            DbusFieldMapping mapping;
            mapping.key = it.key();
            const nlohmann::json& spec = it.value();
            mapping.fromPath = spec.value("from", std::string{}) == "path";
            mapping.interface = spec.value("interface", std::string{});
            mapping.property = spec.value("property", std::string{});
            mapping.transform = spec.value("transform", std::string{});

            if (mapping.fromPath == !mapping.interface.empty())
            {
                return std::unexpected(
                    "field '" + mapping.key +
                    "' needs exactly one of \"from\":\"path\" or "
                    "interface+property");
            }
            if (!mapping.fromPath && mapping.property.empty())
            {
                return std::unexpected("field '" + mapping.key +
                                       "' has an interface but no property");
            }
            if (!mapping.transform.empty() &&
                dbus_transform::find(mapping.transform) == nullptr)
            {
                return std::unexpected(
                    "field '" + mapping.key + "' uses unknown transform '" +
                    mapping.transform + "'");
            }
            config.fields.push_back(std::move(mapping));
        }
    }
    catch (const std::exception& e)
    {
        return std::unexpected(
            std::string("invalid sourceConfig: ") + e.what());
    }

    if (config.subtree.empty() || config.subtree.front() != '/')
    {
        return std::unexpected("'subtree' must be an absolute object path");
    }
    if (config.idKey.empty() != config.idArg.empty())
    {
        return std::unexpected("'idKey' and 'idArg' must be set together");
    }
    if (!isList && config.idArg.empty())
    {
        return std::unexpected(
            "a non-list field needs 'idKey' and 'idArg' to select one object");
    }

    // A key may not be both a leaf and the parent of another key.
    for (const auto& a : config.fields)
    {
        for (const auto& b : config.fields)
        {
            if (b.key.size() > a.key.size() &&
                b.key.compare(0, a.key.size() + 1, a.key + "/") == 0)
            {
                return std::unexpected(
                    "field '" + a.key + "' conflicts with nested field '" +
                    b.key + "'");
            }
        }
    }

    if (!config.idKey.empty() &&
        std::ranges::none_of(config.fields, [&](const auto& m) {
            return m.key == config.idKey;
        }))
    {
        return std::unexpected(
            "'idKey' '" + config.idKey + "' is not one of the mapped fields");
    }

    // Objects are selected by the listed interfaces; default to every
    // interface the property mappings read.
    if (config.interfaces.empty())
    {
        std::set<std::string> seen;
        for (const auto& m : config.fields)
        {
            if (!m.interface.empty() && seen.insert(m.interface).second)
            {
                config.interfaces.push_back(m.interface);
            }
        }
    }
    if (config.interfaces.empty())
    {
        return std::unexpected(
            "no 'interfaces' given and no property mapping to infer from");
    }
    return config;
}

namespace dbus_source_detail
{

// Set root["a"]["b"] for key "a/b", creating intermediate objects.
inline void setNested(nlohmann::json& root, const std::string& key,
                      nlohmann::json value)
{
    nlohmann::json* node = &root;
    size_t start = 0;
    while (true)
    {
        auto pos = key.find('/', start);
        if (pos == std::string::npos)
        {
            (*node)[key.substr(start)] = std::move(value);
            return;
        }
        node = &(*node)[key.substr(start, pos - start)];
        start = pos + 1;
    }
}

inline std::string expandPlaceholders(std::string text,
                                      const nlohmann::json& args)
{
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        if (!it.value().is_string())
        {
            continue;
        }
        const std::string placeholder = "{" + it.key() + "}";
        const std::string value = it.value().get<std::string>();
        size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::string::npos)
        {
            text.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return text;
}

inline nlohmann::json applyTransform(const DbusFieldMapping& mapping,
                                     nlohmann::json value)
{
    if (mapping.transform.empty())
    {
        return value;
    }
    return (*dbus_transform::find(mapping.transform))(value);
}

// Value of an object-path mapping, used to skip non-matching objects before
// any property is fetched.
inline std::optional<nlohmann::json> idFromPath(const DbusSourceConfig& config,
                                                const std::string& path)
{
    for (const auto& mapping : config.fields)
    {
        if (mapping.key == config.idKey && mapping.fromPath)
        {
            return applyTransform(mapping, path);
        }
    }
    return std::nullopt;
}

// Upper bound for D-Bus calls in flight per object and per query field.
inline constexpr size_t maxConcurrentReads = 8;

// Reads one interface of one object. A failure is logged and yields nullopt so
// the other interfaces of the object are still used.
inline Task<std::optional<DbusPropertyMap>> readInterface(
    std::shared_ptr<DbusBackend> backend, std::string service, std::string path,
    std::string interface)
{
    Result<DbusPropertyMap> props =
        co_await backend->getAll(service, path, interface);
    if (!props)
    {
        logWarning("GetAll {} {} on {} failed: {}", path, interface, service,
                   props.error());
        co_return std::nullopt;
    }
    co_return std::move(*props);
}

// Fetch the needed interfaces of one object (concurrently) and map them to
// response JSON.
inline Task<nlohmann::json> buildObject(
    std::shared_ptr<DbusBackend> backend,
    std::shared_ptr<const DbusSourceConfig> config, std::string path,
    std::map<std::string, std::string> serviceByInterface)
{
    std::vector<std::string> interfaceNames;
    std::vector<Task<std::optional<DbusPropertyMap>>> reads;
    for (const auto& [interface, service] : serviceByInterface)
    {
        interfaceNames.push_back(interface);
        reads.push_back(readInterface(backend, service, path, interface));
    }
    auto fetched = co_await gather(std::move(reads), maxConcurrentReads);

    std::map<std::string, DbusPropertyMap> propertiesByInterface;
    for (size_t i = 0; i < fetched.size(); ++i)
    {
        if (fetched[i])
        {
            propertiesByInterface.emplace(interfaceNames[i],
                                          std::move(*fetched[i]));
        }
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& mapping : config->fields)
    {
        if (mapping.fromPath)
        {
            setNested(object, mapping.key, applyTransform(mapping, path));
            continue;
        }
        auto ifaceIt = propertiesByInterface.find(mapping.interface);
        if (ifaceIt == propertiesByInterface.end())
        {
            continue;
        }
        auto propIt = ifaceIt->second.find(mapping.property);
        if (propIt == ifaceIt->second.end())
        {
            continue;
        }
        setNested(object, mapping.key,
                  applyTransform(mapping, dbusValueToJson(propIt->second)));
    }
    co_return object;
}

} // namespace dbus_source_detail

/**
 * @brief DataSource that fills a root field from D-Bus.
 *
 * Objects, and the interfaces of each object, are read concurrently (bounded
 * by dbus_source_detail::maxConcurrentReads per level).
 */
class DbusSource : public DataSource
{
  public:
    explicit DbusSource(std::shared_ptr<DbusBackend> dbusBackend) :
        backend(std::move(dbusBackend))
    {}

    Result<void> validate(const FieldSpec& fieldSpec) const override
    {
        auto config =
            parseDbusSourceConfig(fieldSpec.sourceConfig, fieldSpec.isList);
        if (!config)
        {
            return std::unexpected(config.error());
        }
        return {};
    }

    Task<Result<nlohmann::json>> fetch(const FieldSpec& fieldSpec,
                                       const nlohmann::json& args) override
    {
        auto parsed =
            parseDbusSourceConfig(fieldSpec.sourceConfig, fieldSpec.isList);
        if (!parsed)
        {
            co_return std::unexpected(parsed.error());
        }
        auto config =
            std::make_shared<const DbusSourceConfig>(std::move(*parsed));

        const std::string subtree =
            dbus_source_detail::expandPlaceholders(config->subtree, args);

        auto primary = co_await backend->getSubTree(subtree, config->depth,
                                                    config->interfaces);
        if (!primary)
        {
            co_return std::unexpected("D-Bus GetSubTree failed for '" +
                                      subtree + "': " + primary.error());
        }

        // path -> interface -> service, for every interface we need to read.
        std::map<std::string, std::map<std::string, std::string>> located;
        auto record = [&located](const DbusSubTree& tree) {
            for (const auto& [path, services] : tree)
            {
                for (const auto& [service, interfaces] : services)
                {
                    for (const auto& interface : interfaces)
                    {
                        located[path].emplace(interface, service);
                    }
                }
            }
        };
        record(*primary);

        std::set<std::string> needed;
        for (const auto& mapping : config->fields)
        {
            if (!mapping.interface.empty())
            {
                needed.insert(mapping.interface);
            }
        }
        // GetSubTree only reports the interfaces it was filtered by, so look
        // up the others with one mapper call per interface (not per object).
        for (const auto& interface : needed)
        {
            if (std::ranges::find(config->interfaces, interface) !=
                config->interfaces.end())
            {
                continue;
            }
            auto extra = co_await backend->getSubTree(
                subtree, config->depth, std::vector<std::string>{interface});
            if (!extra)
            {
                co_return std::unexpected("D-Bus GetSubTree failed for '" +
                                          subtree + "': " + extra.error());
            }
            record(*extra);
        }

        std::optional<nlohmann::json> wantedId;
        if (!config->idArg.empty() && args.contains(config->idArg) &&
            args[config->idArg].is_string())
        {
            wantedId = args[config->idArg];
        }

        // Build every object concurrently (bounded); keep the mapper's order.
        std::vector<Task<nlohmann::json>> builds;
        for (const auto& [path, services] : *primary)
        {
            if (wantedId)
            {
                auto pathId = dbus_source_detail::idFromPath(*config, path);
                if (pathId && *pathId != *wantedId)
                {
                    continue;
                }
            }
            std::map<std::string, std::string> serviceByInterface;
            if (auto pathIt = located.find(path); pathIt != located.end())
            {
                for (const auto& interface : needed)
                {
                    auto ifaceIt = pathIt->second.find(interface);
                    if (ifaceIt != pathIt->second.end())
                    {
                        serviceByInterface.emplace(interface, ifaceIt->second);
                    }
                }
            }
            builds.push_back(dbus_source_detail::buildObject(
                backend, config, path, std::move(serviceByInterface)));
        }
        auto objects = co_await gather(std::move(builds),
                                       dbus_source_detail::maxConcurrentReads);

        nlohmann::json members = nlohmann::json::array();
        for (auto& object : objects)
        {
            if (wantedId &&
                object.value(config->idKey, nlohmann::json()) != *wantedId)
            {
                continue;
            }
            members.push_back(std::move(object));
        }

        if (fieldSpec.isList)
        {
            co_return nlohmann::json{{"Members", std::move(members)}};
        }
        if (members.empty())
        {
            co_return std::unexpected(
                "Resource not found for field '" + fieldSpec.name + "' (" +
                config->idArg + "=" + wantedId.value_or("").dump() + ")");
        }
        co_return std::move(members.front());
    }

  private:
    std::shared_ptr<DbusBackend> backend;
};

} // namespace sutra::graphql

#endif // SUTRA_HAVE_DBUS
