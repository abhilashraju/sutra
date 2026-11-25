/**
 * @file dbus_property_watcher.hpp
 * @brief Coroutine-based D-Bus property and signal watchers.
 *
 * Provides DbusWatcher (CRTP base), DbusPropertyWatcher (watches a property on
 * an interface), and DbusSignalWatcher (watches arbitrary D-Bus signals).
 *
 * Uses MatchContext / MatchRegistration with weak_ptr references to safely
 * decouple sd_bus C callback lifetimes from watcher instance lifetimes.
 */
#pragma once

#ifdef SUTRA_HAVE_DBUS

#include "sutra/config.hpp"
#include "sutra/dbus.hpp"
#include "sutra/log.hpp"

#include <systemd/sd-bus.h>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <concepts>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sutra
{

template <typename Handler, typename PropType>
concept WatchHandler = requires(Handler handler, const error_code& ec,
                                std::optional<PropType> result) {
                           { handler(ec, result) } -> std::same_as<Task<void>>;
                       };

// Variant that allows the callback to return bool: true = keep watching,
// false = stop.
template <typename Handler, typename PropType>
concept WatchHandlerBool = requires(Handler handler, const error_code& ec,
                                    std::optional<PropType> result) {
                               {
                                   handler(ec, result)
                               } -> std::same_as<Task<bool>>;
                           };

// Trampoline context for the raw sd_bus C callback.
//
// Owns a weak_ptr to the watcher so that if the watcher has already been
// destroyed the callback is a safe no-op. The context object is
// heap-allocated alongside the sdbusplus::bus::match slot and is freed
// only after sd_bus_slot_unref() returns — guaranteeing the raw pointer
// passed to sd_bus remains valid for the full lifetime of the slot.
template <typename Derived>
struct MatchContext
{
    explicit MatchContext(std::weak_ptr<Derived> w) : weak(std::move(w)) {}

    std::weak_ptr<Derived> weak;

    // C-style trampoline registered with sd_bus_add_match.
    // 'userdata' points to this MatchContext instance.
    static int callback(sd_bus_message* m, void* userdata,
                        sd_bus_error* /*e*/) noexcept
    {
        auto* ctx = static_cast<MatchContext*>(userdata);
        if (auto self = ctx->weak.lock())
        {
            try
            {
                sdbusplus::message_t msg{m};
                self->dispatchMessage(msg);
            }
            catch (...)
            {}
        }
        return 0;
    }
};

// Bundles a match slot with the context object whose raw pointer is held
// by sd_bus for the lifetime of that slot.
//
// Destruction order is critical: the slot (matchObj) must be destroyed
// first so sd_bus_slot_unref() fires before the context pointer becomes
// dangling; then matchCtx is destroyed. The struct members are declared
// in that order, and C++ destroys members in reverse-declaration order,
// so the invariant is upheld automatically.
template <typename Derived>
struct MatchRegistration
{
    // matchObj must be destroyed FIRST (reverse-declaration order).
    // Declare matchCtx first so it is destroyed last.
    std::shared_ptr<MatchContext<Derived>> matchCtx;
    std::optional<sdbusplus::bus::match_t> matchObj;

    MatchRegistration(sdbusplus::asio::connection& conn,
                      const std::string& rule, std::weak_ptr<Derived> weak) :
        matchCtx(std::make_shared<MatchContext<Derived>>(std::move(weak))),
        matchObj(std::in_place, conn, rule, &MatchContext<Derived>::callback,
                 static_cast<void*>(matchCtx.get()))
    {}
};

template <typename Derived, typename PropType>
struct DbusWatcher : std::enable_shared_from_this<Derived>
{
    using PROPERTY_HANDLER =
        std::move_only_function<void(error_code, PropType)>;
    PROPERTY_HANDLER propHandler;
    std::shared_ptr<sdbusplus::asio::connection> conn;

    std::optional<MatchRegistration<Derived>> matchReg;

    DbusWatcher() = delete;
    DbusWatcher(const DbusWatcher&) = delete;
    DbusWatcher& operator=(const DbusWatcher&) = delete;
    DbusWatcher(DbusWatcher&&) = delete;
    DbusWatcher& operator=(DbusWatcher&&) = delete;
    explicit DbusWatcher(std::shared_ptr<sdbusplus::asio::connection> c) :
        conn(std::move(c))
    {}

    virtual ~DbusWatcher()
    {
        logDebug(
            "DbusWatcher destructor called - cleaning up match and handler");
        propHandler = nullptr;
        matchReg.reset();
    }

    Derived& derived()
    {
        return static_cast<Derived&>(*this);
    }

    net::io_context& getIoContext()
    {
        return derived().getIoContextImpl();
    }

    net::io_context& getIoContextImpl()
    {
        return conn->get_io_context();
    }

    void startTimeout(std::shared_ptr<net::steady_timer> timer,
                      std::chrono::steady_clock::duration timeout)
    {
        timer->expires_after(timeout);
        std::weak_ptr<Derived> weak = derived().shared_from_this();
        auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
        logInfo("startTimeout: arming {}s timer for watcher={}", secs,
                static_cast<const void*>(this));
        timer->async_wait([timer, weak, secs](const error_code& ec) {
            if (!ec)
            {
                if (auto self = weak.lock())
                {
                    logError(
                        "Timeout occurred after {}s — watcher={} (firing cancelWatch)",
                        secs, static_cast<const void*>(self.get()));
                    self->cancelWatch();
                }
            }
            else
            {
                logDebug("startTimeout timer cancelled ec={} watcher={}",
                         ec.message(), weak.expired() ? "expired" : "alive");
            }
        });
    }

    struct Initiation
    {
        template <typename Handler>
        void operator()(Handler handler, DbusWatcher* self) const
        {
            self->propHandler = [h = std::move(handler)](
                                    error_code ec, PropType value) mutable {
                h(ec, std::move(value));
            };
        }
    };

    Task<std::tuple<error_code, PropType>> waitForChange()
    {
        using Token = const std::remove_cvref_t<decltype(useSenderNoThrow)>&;
        co_return co_await net::async_initiate<Token,
                                               void(error_code, PropType)>(
            Initiation{}, useSenderNoThrow, this);
    }

    Task<void> watch(auto callback,
                     std::chrono::steady_clock::duration timeout =
                         std::chrono::steady_clock::duration::max())
    {
        std::shared_ptr<net::steady_timer> timer;
        if (timeout != std::chrono::steady_clock::duration::max())
        {
            timer = std::make_shared<net::steady_timer>(getIoContext());
            startTimeout(timer, timeout);
        }

        error_code ec{};
        while (true)
        {
            PropType res{};
            std::tie(ec, res) = co_await waitForChange();
            if (!ec)
            {
                if constexpr (requires {
                                  {
                                      callback(ec,
                                               std::optional(std::move(res)))
                                  } -> std::same_as<Task<bool>>;
                              })
                {
                    if (!co_await callback(ec, std::optional(std::move(res))))
                    {
                        break;
                    }
                }
                else
                {
                    co_await callback(ec, std::optional(std::move(res)));
                }
                continue;
            }
            if (ec == net::error::operation_aborted)
            {
                logDebug("Watch timed out — stopping watch");
                co_await callback(net::error::timed_out, std::nullopt);
                break;
            }
            if (ec != net::error::no_such_device)
            {
                logDebug(
                    "Unsupported type or some other error occurred: {} stopping watch",
                    ec.message());
                propHandler = nullptr;
                co_await callback(ec, std::nullopt);
                break;
            }
        }
        if (timer)
        {
            timer->cancel();
        }
        co_return;
    }

    Task<std::optional<PropType>> watchOnce(
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(1))
    {
        auto timer = std::make_shared<net::steady_timer>(getIoContext());
        startTimeout(timer, timeout);
        auto [ec, res] = co_await waitForChange();
        timer->cancel();
        propHandler = nullptr;
        if (ec)
        {
            logError("Error in watching Dbus: {}", ec.message());
            co_return std::nullopt;
        }
        co_return std::optional(std::move(res));
    }

    void notifyChange(const error_code& ec, PropType value)
    {
        if (propHandler)
        {
            auto h = std::move(propHandler);
            h(ec, std::move(value));
        }
    }

    void cancelWatch()
    {
        if (propHandler)
        {
            auto h = std::move(propHandler);
            h(net::error::operation_aborted, PropType{});
        }
    }

    void removeMatch()
    {
        matchReg.reset();
    }
};

template <typename TYPE>
struct DbusPropertyWatcher : public DbusWatcher<DbusPropertyWatcher<TYPE>, TYPE>
{
    using BASE = DbusWatcher<DbusPropertyWatcher<TYPE>, TYPE>;
    using PropType = TYPE;
    std::string propMatchRule;
    std::string propName;

  private:
    struct PrivateTag
    {};

  public:
    static std::shared_ptr<DbusPropertyWatcher<TYPE>> create(
        std::shared_ptr<sdbusplus::asio::connection> conn,
        const std::string& path, const std::string& intf,
        const std::string& prop)
    {
        auto watcher = std::make_shared<DbusPropertyWatcher<TYPE>>(
            PrivateTag{}, std::move(conn), path, intf, prop);
        watcher->addMatch();
        return watcher;
    }

    DbusPropertyWatcher(PrivateTag,
                        std::shared_ptr<sdbusplus::asio::connection> conn,
                        const std::string& path, const std::string& intf,
                        const std::string& prop) :
        BASE(std::move(conn)), propName(prop)
    {
        propMatchRule =
            sdbusplus::bus::match::rules::propertiesChanged(path, intf);
    }

    void dispatchMessage(sdbusplus::message_t& msg)
    {
        handlePropertyChange(msg);
    }

  private:
    void addMatch()
    {
        BASE::matchReg.emplace(*BASE::conn, propMatchRule,
                               BASE::derived().shared_from_this());
    }

    void handlePropertyChange(sdbusplus::message_t& msg)
    {
        std::string interfaceName;
        PropertyMap changedProperties;
        std::vector<std::string> invalidatedProperties;

        try
        {
            msg.read(interfaceName, changedProperties, invalidatedProperties);
        }
        catch (const std::exception& e)
        {
            logError("Error decoding PropertiesChanged signal: {}", e.what());
            return;
        }

        logInfo("Properties changed on interface: {}", interfaceName);

        auto it = changedProperties.find(propName);
        if (it == changedProperties.end())
        {
            return;
        }

        if constexpr (std::is_same_v<PropType, DbusVariant>)
        {
            logDebug("Property {} changed", propName);
            BASE::notifyChange(error_code{}, it->second);
        }
        else
        {
            if (!std::holds_alternative<PropType>(it->second))
            {
                logError("Property {} type mismatch in PropertiesChanged",
                         propName);
                BASE::notifyChange(net::error::invalid_argument, PropType{});
                return;
            }
            auto val = std::get<PropType>(it->second);
            logDebug("Property {} changed", propName);
            BASE::notifyChange(error_code{}, std::move(val));
        }
    }
};

template <typename TYPE>
struct DbusSignalWatcher : public DbusWatcher<DbusSignalWatcher<TYPE>, TYPE>
{
    using BASE = DbusWatcher<DbusSignalWatcher<TYPE>, TYPE>;
    using PropType = TYPE;

    std::string signalMatchRule;

  private:
    struct PrivateTag
    {};

  public:
    static std::shared_ptr<DbusSignalWatcher<TYPE>> nameOwnerChanged(
        std::shared_ptr<sdbusplus::asio::connection> conn)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::nameOwnerChanged());
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesAdded(
        std::shared_ptr<sdbusplus::asio::connection> conn)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::interfacesAdded());
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesAdded(
        std::shared_ptr<sdbusplus::asio::connection> conn, std::string_view p)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::interfacesAdded(p));
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesAddedAtPath(
        std::shared_ptr<sdbusplus::asio::connection> conn, std::string_view p)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::interfacesAddedAtPath(p));
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesRemoved(
        std::shared_ptr<sdbusplus::asio::connection> conn)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::interfacesRemoved());
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesRemoved(
        std::shared_ptr<sdbusplus::asio::connection> conn, std::string_view p)
    {
        return makeWith(std::move(conn),
                        sdbusplus::bus::match::rules::interfacesRemoved(p));
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> interfacesRemovedAtPath(
        std::shared_ptr<sdbusplus::asio::connection> conn, std::string_view p)
    {
        return makeWith(
            std::move(conn),
            sdbusplus::bus::match::rules::interfacesRemovedAtPath(p));
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> create(
        std::shared_ptr<sdbusplus::asio::connection> conn,
        const std::string& intf, const std::string& signal)
    {
        return makeWith(std::move(conn),
                        std::format("type='signal',interface='{}',member='{}'",
                                    intf, signal));
    }

    static std::shared_ptr<DbusSignalWatcher<TYPE>> create(
        std::shared_ptr<sdbusplus::asio::connection> conn,
        const std::string& matchRule)
    {
        return makeWith(std::move(conn), matchRule);
    }

    DbusSignalWatcher(PrivateTag,
                      std::shared_ptr<sdbusplus::asio::connection> conn,
                      std::string rule) :
        BASE(std::move(conn)), signalMatchRule(std::move(rule))
    {}

    void dispatchMessage(sdbusplus::message_t& msg)
    {
        handleSignalChange(msg);
    }

  private:
    static std::shared_ptr<DbusSignalWatcher<TYPE>> makeWith(
        std::shared_ptr<sdbusplus::asio::connection> conn, std::string rule)
    {
        auto watcher = std::make_shared<DbusSignalWatcher<TYPE>>(
            PrivateTag{}, std::move(conn), std::move(rule));
        watcher->addMatch();
        return watcher;
    }

    void addMatch()
    {
        logDebug("Adding signal match rule: {}", signalMatchRule);
        BASE::matchReg.emplace(*BASE::conn, signalMatchRule,
                               BASE::derived().shared_from_this());
    }

    void handleSignalChange(sdbusplus::message_t& msg)
    {
        if constexpr (std::is_same_v<PropType, sdbusplus::message_t>)
        {
            BASE::notifyChange(error_code{}, std::move(msg));
        }
        else
        {
            try
            {
                PropType value;
                msg.read(value);
                logDebug("Received signal value");
                BASE::notifyChange(error_code{}, std::move(value));
            }
            catch (const std::exception& e)
            {
                logError("Failed to unpack signal message: {}", e.what());
                BASE::notifyChange(net::error::invalid_argument, PropType{});
            }
        }
    }
};

} // namespace sutra

#endif // SUTRA_HAVE_DBUS
