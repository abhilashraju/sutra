/**
 * @file dbus.hpp
 * @brief Task based D-Bus calls on top of sdbusplus::asio::connection.
 *
 * Every function returns a `Task<T>`: the value on success, a thrown
 * `system_error` on failure. D-Bus error replies arrive as the errno-style
 * code sdbusplus extracts from the reply (for example `no_such_file_or_
 * directory` for an unknown object); a reply that cannot be decoded as the
 * requested type becomes `invalid_argument`.
 *
 * @code
 * auto state = co_await getProperty<std::string>(
 *     bus, "xyz.openbmc_project.State.Host",
 *     "/xyz/openbmc_project/state/host0", "xyz.openbmc_project.State.Host",
 *     "CurrentHostState");
 * @endcode
 *
 * Lifetime: the connection must outlive every call. All arguments are copied
 * into the call, so temporaries are fine.
 *
 * Cancellation: a pending D-Bus call cannot be revoked. A stop request is
 * noticed when the reply (or sdbusplus' own timeout) arrives.
 *
 * Only available when SUTRA_HAVE_DBUS is defined (link `sutra_dbus_dep`).
 */
#pragma once

#ifdef SUTRA_HAVE_DBUS

#include "sutra/config.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace sutra
{

// Common D-Bus service and interface names.
constexpr auto objectMapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto objectMapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr auto dbusPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto dbusObjectManagerInterface =
    "org.freedesktop.DBus.ObjectManager";
constexpr auto dbusIntrospectableInterface =
    "org.freedesktop.DBus.Introspectable";
constexpr auto associationInterface = "xyz.openbmc_project.Association";

using ObjectPath = sdbusplus::object_path;

/**
 * @brief Any value a D-Bus property commonly carries.
 *
 * `std::monostate` comes first on purpose: sdbusplus leaves the first
 * alternative in place when the wire type matches none of the others, so an
 * unsupported property type shows up as monostate instead of a bogus `false`.
 */
using DbusVariant = std::variant<std::monostate, bool, uint8_t, int16_t,
                                 uint16_t, int32_t, uint32_t, int64_t, uint64_t,
                                 double, std::string, std::vector<std::string>>;

/** @brief Property name to value. */
using PropertyMap = std::map<std::string, DbusVariant>;

/** @brief Interface name to its properties. */
using InterfaceMap = std::map<std::string, PropertyMap>;

/** @brief GetManagedObjects reply: a{oa{sa{sv}}}. */
using ManagedObjectMap = std::map<ObjectPath, InterfaceMap>;

/** @brief GetSubTree reply: a{sa{sas}} (path -> service -> interfaces). */
using SubTreeMap =
    std::map<std::string, std::map<std::string, std::vector<std::string>>>;

/** @brief GetObject reply: a{sas} (service -> interfaces). */
using ObjectMap = std::map<std::string, std::vector<std::string>>;

namespace details
{

/** void for no values, the value for one, a tuple for several. */
template <class... Ret>
struct DbusResultOf
{
    using type = std::tuple<Ret...>;
};

template <>
struct DbusResultOf<>
{
    using type = void;
};

template <class Ret>
struct DbusResultOf<Ret>
{
    using type = Ret;
};

template <class... Ret>
using DbusResult = typename DbusResultOf<Ret...>::type;

/** Initiation function handed to Asio: starts the actual D-Bus call. */
template <class Result, class... Ret>
struct DbusCallInitiation
{
    template <class Handler, class... InputArgs>
    void operator()(Handler handler, sdbusplus::asio::connection* bus,
                    std::string service, std::string path, std::string iface,
                    std::string method, std::tuple<InputArgs...> inputs) const
    {
        auto reply = [handler = std::move(handler)](error_code ec,
                                                    Ret... values) mutable {
            if constexpr (std::is_void_v<Result>)
            {
                handler(ec);
            }
            else
            {
                handler(ec, Result(std::move(values)...));
            }
        };
        std::apply(
            [&](const InputArgs&... args) {
                bus->async_method_call(std::move(reply), service, path, iface,
                                       method, args...);
            },
            inputs);
    }
};

} // namespace details

/**
 * @brief Calls a D-Bus method.
 *
 * @tparam Ret values the method returns: none gives `Task<void>`, one gives
 *         `Task<Ret>`, several give `Task<std::tuple<Ret...>>`.
 * @param args arguments of the method call, copied.
 */
template <class... Ret, class... InputArgs>
Task<details::DbusResult<Ret...>> dbusMethodCall(
    sdbusplus::asio::connection& bus, std::string service, std::string path,
    std::string iface, std::string method, InputArgs... args)
{
    using Result = details::DbusResult<Ret...>;
    using Token = const std::remove_cvref_t<decltype(useSender)>&;
    details::DbusCallInitiation<Result, Ret...> initiation;
    auto inputs = std::make_tuple(std::move(args)...);
    if constexpr (std::is_void_v<Result>)
    {
        co_await net::async_initiate<Token, void(error_code)>(
            initiation, useSender, &bus, std::move(service), std::move(path),
            std::move(iface), std::move(method), std::move(inputs));
    }
    else
    {
        co_return co_await net::async_initiate<Token, void(error_code, Result)>(
            initiation, useSender, &bus, std::move(service), std::move(path),
            std::move(iface), std::move(method), std::move(inputs));
    }
}

/**
 * @brief org.freedesktop.DBus.Properties.Get.
 * @throws system_error `invalid_argument` when the property is not a `T`.
 */
template <class T>
Task<T> getProperty(sdbusplus::asio::connection& bus, std::string service,
                    std::string path, std::string iface, std::string property)
{
    auto value = co_await dbusMethodCall<std::variant<std::monostate, T>>(
        bus, std::move(service), std::move(path), dbusPropertiesInterface,
        "Get", std::move(iface), std::move(property));
    if (!std::holds_alternative<T>(value))
    {
        throw system_error(net::error::invalid_argument);
    }
    co_return std::get<T>(std::move(value));
}

/** @brief org.freedesktop.DBus.Properties.Set. */
template <class T>
Task<void> setProperty(sdbusplus::asio::connection& bus, std::string service,
                       std::string path, std::string iface,
                       std::string property, T value)
{
    co_await dbusMethodCall<>(
        bus, std::move(service), std::move(path), dbusPropertiesInterface,
        "Set", std::move(iface), std::move(property),
        std::variant<T>(std::move(value)));
}

/** @brief org.freedesktop.DBus.Properties.GetAll. */
inline Task<PropertyMap> getAllProperties(sdbusplus::asio::connection& bus,
                                          std::string service, std::string path,
                                          std::string iface)
{
    return dbusMethodCall<PropertyMap>(bus, std::move(service), std::move(path),
                                       dbusPropertiesInterface, "GetAll",
                                       std::move(iface));
}

/** @brief Calls `method` of the OpenBMC ObjectMapper. */
template <class Ret, class... InputArgs>
Task<Ret> callObjectMapperMethod(sdbusplus::asio::connection& bus,
                                 std::string method, InputArgs... args)
{
    return dbusMethodCall<Ret>(bus, objectMapperService, objectMapperPath,
                               objectMapperInterface, std::move(method),
                               std::move(args)...);
}

/** @brief ObjectMapper GetSubTree: path -> service -> interfaces. */
inline Task<SubTreeMap> getSubTree(sdbusplus::asio::connection& bus,
                                   std::string path, int32_t depth,
                                   std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<SubTreeMap>(
        bus, "GetSubTree", std::move(path), depth, std::move(interfaces));
}

/** @brief ObjectMapper GetObject: services (and interfaces) at `path`. */
template <class DictType = ObjectMap>
Task<DictType> getObjects(sdbusplus::asio::connection& bus, std::string path,
                          std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(bus, "GetObject", std::move(path),
                                            std::move(interfaces));
}

/** @brief Same as getObjects(); name kept from the coroserver API. */
template <class DictType = ObjectMap>
Task<DictType> getDbusObject(sdbusplus::asio::connection& bus, std::string path,
                             std::vector<std::string> interfaces = {})
{
    return getObjects<DictType>(bus, std::move(path), std::move(interfaces));
}

/** @brief ObjectMapper GetSubTreePaths. */
template <class DictType = std::vector<std::string>>
Task<DictType> getSubTreePaths(sdbusplus::asio::connection& bus,
                               std::string path, int32_t depth,
                               std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetSubTreePaths", std::move(path), depth, std::move(interfaces));
}

/** @brief ObjectMapper GetAssociatedSubTree. */
template <class DictType = SubTreeMap>
Task<DictType> getAssociatedSubTree(
    sdbusplus::asio::connection& bus, ObjectPath associatedPath,
    ObjectPath path, int32_t depth, std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAssociatedSubTree", std::move(associatedPath), std::move(path),
        depth, std::move(interfaces));
}

/** @brief ObjectMapper GetAssociatedSubTreePaths. */
template <class DictType = std::vector<std::string>>
Task<DictType> getAssociatedSubTreePaths(
    sdbusplus::asio::connection& bus, ObjectPath associatedPath,
    ObjectPath path, int32_t depth, std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAssociatedSubTreePaths", std::move(associatedPath),
        std::move(path), depth, std::move(interfaces));
}

namespace details
{
inline std::vector<std::string> toStrings(
    std::span<const std::string_view> views)
{
    return std::vector<std::string>(views.begin(), views.end());
}
} // namespace details

/** @brief ObjectMapper GetAssociatedSubTreeById. */
template <class DictType = SubTreeMap>
Task<DictType> getAssociatedSubTreeById(
    sdbusplus::asio::connection& bus, std::string id, std::string path,
    std::span<const std::string_view> subtreeInterfaces,
    std::string_view association,
    std::vector<std::string> endpointInterfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAssociatedSubTreeById", std::move(id), std::move(path),
        details::toStrings(subtreeInterfaces), std::string(association),
        std::move(endpointInterfaces));
}

/** @brief ObjectMapper GetAssociatedSubTreePathsById. */
template <class DictType = std::vector<std::string>>
Task<DictType> getAssociatedSubTreePathsById(
    sdbusplus::asio::connection& bus, std::string id, std::string path,
    std::span<const std::string_view> subtreeInterfaces,
    std::string_view association,
    std::vector<std::string> endpointInterfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAssociatedSubTreePathsById", std::move(id), std::move(path),
        details::toStrings(subtreeInterfaces), std::string(association),
        std::move(endpointInterfaces));
}

/** @brief ObjectMapper GetAncestors. */
template <class DictType = SubTreeMap>
Task<DictType> getAncestors(sdbusplus::asio::connection& bus, std::string path,
                            std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAncestors", std::move(path), std::move(interfaces));
}

/** @brief The `endpoints` property of an association object. */
template <class DictType = std::vector<std::string>>
Task<DictType> getAssociationEndPoints(sdbusplus::asio::connection& bus,
                                       std::string path)
{
    return getProperty<DictType>(bus, objectMapperService, std::move(path),
                                 associationInterface, "endpoints");
}

/** @brief org.freedesktop.DBus.ObjectManager.GetManagedObjects. */
template <class DictType = ManagedObjectMap>
Task<DictType> getManagedObjects(sdbusplus::asio::connection& bus,
                                 std::string service, std::string path)
{
    return dbusMethodCall<DictType>(bus, std::move(service), std::move(path),
                                    dbusObjectManagerInterface,
                                    "GetManagedObjects");
}

/** @brief org.freedesktop.DBus.Introspectable.Introspect (XML text). */
inline Task<std::string> introspect(sdbusplus::asio::connection& bus,
                                    std::string service, std::string path)
{
    return dbusMethodCall<std::string>(bus, std::move(service), std::move(path),
                                       dbusIntrospectableInterface,
                                       "Introspect");
}

/** @brief Default value for a property type; specialise when `{}` is wrong. */
template <class T>
T getDefaultValue()
{
    return {};
}

/**
 * @brief Extracts property `name` of type `T` from `propMap`.
 * @throws system_error `not_found` when absent, `invalid_argument` when the
 *         value has another type.
 */
template <class T>
T getPropertyFromMap(const PropertyMap& propMap, const std::string& name)
{
    auto it = propMap.find(name);
    if (it == propMap.end())
    {
        throw system_error(net::error::not_found);
    }
    if (!std::holds_alternative<T>(it->second))
    {
        throw system_error(net::error::invalid_argument);
    }
    return std::get<T>(it->second);
}

/**
 * @brief Extracts several properties at once.
 *
 * `auto [enabled, speed] = getPropertiesFromMap<bool, uint32_t>(map,
 * "Enabled", "Speed");`
 */
template <class... ArgTypes, class... Names>
std::tuple<ArgTypes...> getPropertiesFromMap(const PropertyMap& propMap,
                                             const Names&... names)
{
    static_assert(sizeof...(ArgTypes) == sizeof...(Names),
                  "one property name per requested type");
    return std::tuple<ArgTypes...>{
        getPropertyFromMap<ArgTypes>(propMap, std::string(names))...};
}

} // namespace sutra

#endif // SUTRA_HAVE_DBUS
