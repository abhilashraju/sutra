/**
 * @file dbus.hpp
 * @brief Task based D-Bus calls on top of sdbusplus::asio::connection.
 *
 * Every function returns a `Task<DbusExpected<T>>`: a `std::expected` holding
 * the value on success and an `error_code` on failure; nothing is thrown for a
 * failed call. D-Bus error replies arrive as the errno-style code sdbusplus
 * extracts from the reply (for example `no_such_file_or_directory` for an
 * unknown object); a reply that cannot be decoded as the requested type
 * becomes `invalid_argument`.
 *
 * @code
 * auto state = co_await getProperty<std::string>(
 *     bus, "xyz.openbmc_project.State.Host",
 *     "/xyz/openbmc_project/state/host0", "xyz.openbmc_project.State.Host",
 *     "CurrentHostState");
 * if (!state)
 * {
 *     logError("{}", state.error().message());
 *     co_return;
 * }
 * use(*state);
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

#include <systemd/sd-bus.h>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
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

/** @brief Same as `Expected<T>`; kept as the D-Bus flavoured name. */
template <class T>
using DbusExpected = Expected<T>;

/**
 * @brief Opens the session or system bus and wraps it in an asio connection.
 *
 * Kept for backward compatibility with older sdbusplus versions, whose
 * `asio::connection` has no constructor taking a `bus_t`. The
 * `(io_context&, sd_bus*)` constructor exists in every version. The
 * connection takes its own reference, so the local one is dropped on return.
 *
 * sdbusplus reports a failure to reach the bus by throwing; that is caught
 * here and returned as the errno-style `error_code` it carries.
 */
inline Expected<std::shared_ptr<sdbusplus::asio::connection>> makeConnection(
    net::io_context& ioc, bool session)
{
    try
    {
        auto bus = session ? sdbusplus::bus::new_user()
                           : sdbusplus::bus::new_system();
        std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> raw(bus.release(),
                                                             &sd_bus_unref);
        return std::make_shared<sdbusplus::asio::connection>(ioc, raw.get());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        return std::unexpected(
            error_code(e.get_errno(), boost::system::system_category()));
    }
    catch (const std::bad_alloc&)
    {
        return std::unexpected(
            make_error_code(boost::system::errc::not_enough_memory));
    }
}

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
 * @tparam Ret values the method returns: none gives
 *         `Task<DbusExpected<void>>`, one gives `Task<DbusExpected<Ret>>`,
 *         several give `Task<DbusExpected<std::tuple<Ret...>>>`.
 * @param args arguments of the method call, copied.
 */
template <class... Ret, class... InputArgs>
Task<DbusExpected<details::DbusResult<Ret...>>> dbusMethodCall(
    sdbusplus::asio::connection& bus, std::string service, std::string path,
    std::string iface, std::string method, InputArgs... args)
{
    using Result = details::DbusResult<Ret...>;
    using Token = const std::remove_cvref_t<decltype(useSenderNoThrow)>&;
    details::DbusCallInitiation<Result, Ret...> initiation;
    auto inputs = std::make_tuple(std::move(args)...);
    if constexpr (std::is_void_v<Result>)
    {
        auto [ec] = co_await net::async_initiate<Token, void(error_code)>(
            initiation, useSenderNoThrow, &bus, std::move(service),
            std::move(path), std::move(iface), std::move(method),
            std::move(inputs));
        if (ec)
        {
            co_return std::unexpected(ec);
        }
        co_return DbusExpected<void>{};
    }
    else
    {
        auto [ec, value] =
            co_await net::async_initiate<Token, void(error_code, Result)>(
                initiation, useSenderNoThrow, &bus, std::move(service),
                std::move(path), std::move(iface), std::move(method),
                std::move(inputs));
        if (ec)
        {
            co_return std::unexpected(ec);
        }
        co_return DbusExpected<Result>(std::move(value));
    }
}

/**
 * @brief org.freedesktop.DBus.Properties.Get.
 *
 * Fails with `invalid_argument` when the property is not a `T`.
 */
template <class T>
Task<DbusExpected<T>> getProperty(sdbusplus::asio::connection& bus,
                                  std::string service, std::string path,
                                  std::string iface, std::string property)
{
    auto value = co_await dbusMethodCall<std::variant<std::monostate, T>>(
        bus, std::move(service), std::move(path), dbusPropertiesInterface,
        "Get", std::move(iface), std::move(property));
    if (!value)
    {
        co_return std::unexpected(value.error());
    }
    if (!std::holds_alternative<T>(*value))
    {
        co_return std::unexpected(error_code(net::error::invalid_argument));
    }
    co_return DbusExpected<T>(std::get<T>(std::move(*value)));
}

/** @brief org.freedesktop.DBus.Properties.Set. */
template <class T>
Task<DbusExpected<void>> setProperty(
    sdbusplus::asio::connection& bus, std::string service, std::string path,
    std::string iface, std::string property, T value)
{
    return dbusMethodCall<>(bus, std::move(service), std::move(path),
                            dbusPropertiesInterface, "Set", std::move(iface),
                            std::move(property),
                            std::variant<T>(std::move(value)));
}

/** @brief org.freedesktop.DBus.Properties.GetAll. */
inline Task<DbusExpected<PropertyMap>> getAllProperties(
    sdbusplus::asio::connection& bus, std::string service, std::string path,
    std::string iface)
{
    return dbusMethodCall<PropertyMap>(bus, std::move(service), std::move(path),
                                       dbusPropertiesInterface, "GetAll",
                                       std::move(iface));
}

/** @brief Calls `method` of the OpenBMC ObjectMapper. */
template <class Ret, class... InputArgs>
Task<DbusExpected<Ret>> callObjectMapperMethod(
    sdbusplus::asio::connection& bus, std::string method, InputArgs... args)
{
    return dbusMethodCall<Ret>(bus, objectMapperService, objectMapperPath,
                               objectMapperInterface, std::move(method),
                               std::move(args)...);
}

/** @brief ObjectMapper GetSubTree: path -> service -> interfaces. */
inline Task<DbusExpected<SubTreeMap>> getSubTree(
    sdbusplus::asio::connection& bus, std::string path, int32_t depth,
    std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<SubTreeMap>(
        bus, "GetSubTree", std::move(path), depth, std::move(interfaces));
}

/** @brief ObjectMapper GetObject: services (and interfaces) at `path`. */
template <class DictType = ObjectMap>
Task<DbusExpected<DictType>> getObjects(
    sdbusplus::asio::connection& bus, std::string path,
    std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(bus, "GetObject", std::move(path),
                                            std::move(interfaces));
}

/** @brief Same as getObjects(); name kept from the coroserver API. */
template <class DictType = ObjectMap>
Task<DbusExpected<DictType>> getDbusObject(
    sdbusplus::asio::connection& bus, std::string path,
    std::vector<std::string> interfaces = {})
{
    return getObjects<DictType>(bus, std::move(path), std::move(interfaces));
}

/** @brief ObjectMapper GetSubTreePaths. */
template <class DictType = std::vector<std::string>>
Task<DbusExpected<DictType>> getSubTreePaths(
    sdbusplus::asio::connection& bus, std::string path, int32_t depth,
    std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetSubTreePaths", std::move(path), depth, std::move(interfaces));
}

/** @brief ObjectMapper GetAssociatedSubTree. */
template <class DictType = SubTreeMap>
Task<DbusExpected<DictType>> getAssociatedSubTree(
    sdbusplus::asio::connection& bus, ObjectPath associatedPath,
    ObjectPath path, int32_t depth, std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAssociatedSubTree", std::move(associatedPath), std::move(path),
        depth, std::move(interfaces));
}

/** @brief ObjectMapper GetAssociatedSubTreePaths. */
template <class DictType = std::vector<std::string>>
Task<DbusExpected<DictType>> getAssociatedSubTreePaths(
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
Task<DbusExpected<DictType>> getAssociatedSubTreeById(
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
Task<DbusExpected<DictType>> getAssociatedSubTreePathsById(
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
Task<DbusExpected<DictType>> getAncestors(
    sdbusplus::asio::connection& bus, std::string path,
    std::vector<std::string> interfaces = {})
{
    return callObjectMapperMethod<DictType>(
        bus, "GetAncestors", std::move(path), std::move(interfaces));
}

/** @brief The `endpoints` property of an association object. */
template <class DictType = std::vector<std::string>>
Task<DbusExpected<DictType>> getAssociationEndPoints(
    sdbusplus::asio::connection& bus, std::string path)
{
    return getProperty<DictType>(bus, objectMapperService, std::move(path),
                                 associationInterface, "endpoints");
}

/** @brief org.freedesktop.DBus.ObjectManager.GetManagedObjects. */
template <class DictType = ManagedObjectMap>
Task<DbusExpected<DictType>> getManagedObjects(
    sdbusplus::asio::connection& bus, std::string service, std::string path)
{
    return dbusMethodCall<DictType>(bus, std::move(service), std::move(path),
                                    dbusObjectManagerInterface,
                                    "GetManagedObjects");
}

/** @brief org.freedesktop.DBus.Introspectable.Introspect (XML text). */
inline Task<DbusExpected<std::string>> introspect(
    sdbusplus::asio::connection& bus, std::string service, std::string path)
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
 *
 * Fails with `not_found` when absent, `invalid_argument` when the value has
 * another type.
 */
template <class T>
DbusExpected<T> getPropertyFromMap(const PropertyMap& propMap,
                                   const std::string& name)
{
    auto it = propMap.find(name);
    if (it == propMap.end())
    {
        return std::unexpected(error_code(net::error::not_found));
    }
    if (!std::holds_alternative<T>(it->second))
    {
        return std::unexpected(error_code(net::error::invalid_argument));
    }
    return std::get<T>(it->second);
}

/**
 * @brief Extracts several properties at once; fails with the error of the
 *        first property that cannot be extracted.
 *
 * `auto props = getPropertiesFromMap<bool, uint32_t>(map, "Enabled",
 * "Speed"); if (props) { auto [enabled, speed] = *props; }`
 */
template <class... ArgTypes, class... Names>
DbusExpected<std::tuple<ArgTypes...>> getPropertiesFromMap(
    const PropertyMap& propMap, const Names&... names)
{
    static_assert(sizeof...(ArgTypes) == sizeof...(Names),
                  "one property name per requested type");
    auto results = std::tuple<DbusExpected<ArgTypes>...>{
        getPropertyFromMap<ArgTypes>(propMap, std::string(names))...};
    error_code failure;
    std::apply(
        [&](const auto&... result) {
            auto record = [&](const auto& r) {
                if (!failure && !r)
                {
                    failure = r.error();
                }
            };
            (record(result), ...);
        },
        results);
    if (failure)
    {
        return std::unexpected(failure);
    }
    return std::apply(
        [](auto&... result) {
            return std::tuple<ArgTypes...>{std::move(*result)...};
        },
        results);
}

} // namespace sutra

#endif // SUTRA_HAVE_DBUS
