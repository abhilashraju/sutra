#pragma once

#include "sutra/config.hpp"
#include "sutra/gather.hpp"
#include "sutra/graphql/data_source.hpp"
#include "sutra/graphql/error.hpp"
#include "sutra/graphql/parser.hpp"
#include "sutra/graphql/schema.hpp"
#include "sutra/graphql/util.hpp"
#include "sutra/log.hpp"
#include "sutra/spawn.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sutra::graphql
{

// Abstract trigger: suspends the subscription loop until the next event.
struct SubscriptionTrigger
{
    virtual ~SubscriptionTrigger() = default;
    // Suspend until the next trigger fires or cancel() is called.
    // Returns true to continue the loop, false to stop.
    virtual Task<bool> wait() = 0;
    // Cancel a pending wait (called by cleanup guard or event monitor).
    virtual void cancel() = 0;
    // Wake the loop immediately (no-op for non-event triggers).
    virtual void fire() {}
    // Key suffix used to form the canonical session key.
    virtual std::string triggerKey() const = 0;
    // Human-readable trigger type for stats.
    virtual std::string triggerType() const = 0;
    // Optional executor the trigger runs on.
    virtual std::optional<net::any_io_executor> getExecutor() const
    {
        return std::nullopt;
    }
};

// Timer-based trigger: wakes the loop on a fixed interval.
class TimerTrigger : public SubscriptionTrigger
{
  public:
    TimerTrigger(net::any_io_executor exec,
                 std::chrono::steady_clock::duration interval) :
        exec(std::move(exec)), timer(this->exec), interval(interval)
    {}

    explicit TimerTrigger(net::io_context& ioc,
                          std::chrono::steady_clock::duration interval) :
        TimerTrigger(ioc.get_executor(), interval)
    {}

    Task<bool> wait() override
    {
        timer.expires_after(interval);
        auto [ec] = co_await timer.async_wait(useSenderNoThrow);
        co_return !ec;
    }

    void cancel() override
    {
        timer.cancel();
    }

    std::string triggerKey() const override
    {
        return std::to_string(interval.count());
    }

    std::string triggerType() const override
    {
        return "timer";
    }

    std::optional<net::any_io_executor> getExecutor() const override
    {
        return exec;
    }

  private:
    net::any_io_executor exec;
    net::steady_timer timer;
    std::chrono::steady_clock::duration interval;
};

// EventTrigger wakes the subscription loop on D-Bus signals with throttling.
//
// The first fire() in a burst starts a fixed collection window (windowMs).
// All subsequent fire() calls within that window are silently absorbed.
// When the window expires the loop does exactly one fetch, no matter how many
// signals arrived. This guarantees a fetch rate no faster than 1/windowMs
// even under a continuous stream of D-Bus events.
class EventTrigger : public SubscriptionTrigger
{
  public:
    explicit EventTrigger(
        net::any_io_executor exec,
        std::chrono::milliseconds windowMs = std::chrono::milliseconds{2000}) :
        exec(std::move(exec)), timer(this->exec), windowMs(windowMs)
    {
        timer.expires_at(net::steady_timer::time_point::max());
    }

    explicit EventTrigger(
        net::io_context& ioc,
        std::chrono::milliseconds windowMs = std::chrono::milliseconds{2000}) :
        EventTrigger(ioc.get_executor(), windowMs)
    {}

    Task<bool> wait() override
    {
        // Phase 1: sleep until the first fire() of the next burst (or cancel).
        if (!pending.load())
        {
            timer.expires_at(net::steady_timer::time_point::max());
            auto [ec] = co_await timer.async_wait(useSenderNoThrow);
            if (cancelled.load())
            {
                co_return false;
            }
        }

        // Phase 2: collection window. Start a fixed timer, clear pending so
        // any fire() calls that arrive during the window are simply absorbed.
        pending.store(false);
        net::steady_timer window(exec);
        window.expires_after(windowMs);
        auto [ec] = co_await window.async_wait(useSenderNoThrow);
        co_return !cancelled.load();
    }

    void fire() override
    {
        pending.store(true);
        timer.cancel(); // wakes phase-1 sleep
    }

    void cancel() override
    {
        cancelled.store(true);
        timer.cancel();
    }

    std::string triggerKey() const override
    {
        return "event";
    }

    std::string triggerType() const override
    {
        return "event";
    }

    std::optional<net::any_io_executor> getExecutor() const override
    {
        return exec;
    }

  private:
    net::any_io_executor exec;
    net::steady_timer timer;
    std::chrono::milliseconds windowMs;
    std::atomic<bool> pending{false};
    std::atomic<bool> cancelled{false};
};

/**
 * @brief Runs GraphQL queries and subscriptions against a Schema and a
 * SourceRegistry.
 *
 * Supports queries and subscriptions with aliases, arguments, variables (with
 * defaults), named and inline fragments and nested objects/lists. `execute()`
 * never throws for query errors: failures are reported as
 * `{"errors":[{"message":...}]}`. Cancellation (a stop request) propagates
 * as a stopped completion.
 */
class Executor : public std::enable_shared_from_this<Executor>
{
  public:
    Executor(Schema schemaIn, std::shared_ptr<const SourceRegistry> registry,
             std::optional<net::any_io_executor> ioExec = std::nullopt) :
        schema(std::move(schemaIn)), sources(std::move(registry)),
        ioExecutor(std::move(ioExec))
    {}

    ~Executor()
    {
        for (auto& [key, session] : activeSubscriptions)
        {
            session->active = false;
            if (session->trigger)
            {
                session->trigger->cancel();
            }
            for (auto& sub : session->subscribers)
            {
                if (sub.disconnectTimer)
                {
                    sub.disconnectTimer->cancel();
                }
            }
        }
    }

    /**
     * @brief Executes `query`. `variables` must be a JSON object (or null).
     */
    Task<nlohmann::json> execute(
        std::string query, nlohmann::json variables = nlohmann::json::object())
    {
        Result<Operation> parsed = Parser::parse(query);
        if (!parsed)
        {
            co_return errorResponse(parsed.error());
        }
        Operation operation = std::move(*parsed);
        if (auto expanded = expandFragments(operation); !expanded)
        {
            co_return errorResponse(expanded.error());
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

        // Query root fields are independent, so resolve them concurrently
        // (bounded). Results keep the query order; the first failing field in
        // that order decides the error, exactly as in sequential execution.
        // The cache lives for this request only, so concurrent requests never
        // see each other's entries.
        RequestCache cache;
        std::vector<Task<Result<nlohmann::json>>> pending;
        pending.reserve(operation.selections.size());
        for (const FieldSelection& selection : operation.selections)
        {
            pending.push_back(resolveRootField(selection, merged, cache));
        }
        auto fields = co_await gather(std::move(pending), maxConcurrentFields);

        nlohmann::json data = nlohmann::json::object();
        for (size_t i = 0; i < fields.size(); ++i)
        {
            if (!fields[i])
            {
                co_return errorResponse(fields[i].error());
            }
            data[outputName(operation.selections[i])] = std::move(*fields[i]);
        }
        co_return nlohmann::json{{"data", std::move(data)}};
    }

    const Schema& getSchema() const
    {
        return schema;
    }

    void setSourceRegistry(std::shared_ptr<const SourceRegistry> registry)
    {
        sources = std::move(registry);
    }

    void setExecutor(net::any_io_executor exec)
    {
        ioExecutor = std::move(exec);
    }

    /** @brief The configured I/O executor, or an error when there is none. */
    Result<net::any_io_executor> getAnyExecutor() const
    {
        if (ioExecutor)
        {
            return *ioExecutor;
        }
        return std::unexpected(
            std::string("No I/O executor configured on Executor"));
    }

    /** @brief Root fields resolved at the same time (default 8). */
    void setMaxConcurrentFields(size_t value)
    {
        maxConcurrentFields = value;
    }

    const std::vector<DbusWatcher>& getDbusWatchers() const
    {
        return schema.getDbusWatchers();
    }

    // Called by D-Bus property watcher or external event injection with the
    // full set of GraphQL fields affected by signals.
    void notifyFieldsChanged(const std::unordered_set<std::string>& fieldNames)
    {
        for (auto& [key, session] : activeSubscriptions)
        {
            if (!session->trigger || session->trigger->triggerType() != "event")
            {
                continue;
            }
            if (operationReferencesAnyField(session->operation, fieldNames))
            {
                session->trigger->fire();
            }
        }
    }

    nlohmann::json getSubscriptionStats() const
    {
        size_t activeSharedLoops = activeSubscriptions.size();
        size_t totalClientSubscribers = 0;
        nlohmann::json details = nlohmann::json::array();

        for (const auto& [key, session] : activeSubscriptions)
        {
            size_t subsCount = session->subscribers.size();
            totalClientSubscribers += subsCount;

            nlohmann::json sessionDetails = {
                {"key", key},
                {"subscriber_count", subsCount},
                {"trigger_type",
                 session->trigger ? session->trigger->triggerType()
                                  : "unknown"}};
            details.push_back(std::move(sessionDetails));
        }

        size_t multiplexedSubscribers = 0;
        if (totalClientSubscribers > activeSharedLoops)
        {
            multiplexedSubscribers = totalClientSubscribers - activeSharedLoops;
        }

        return {{"active_shared_loops", activeSharedLoops},
                {"total_client_subscribers", totalClientSubscribers},
                {"multiplexed_subscribers", multiplexedSubscribers},
                {"subscriptions", std::move(details)}};
    }

    struct Subscriber
    {
        uint64_t id;
        std::function<Task<bool>(nlohmann::json)> callback;
        std::shared_ptr<net::steady_timer> disconnectTimer;
    };

    struct SubscriptionSession
    {
        std::string key;
        std::shared_ptr<SubscriptionTrigger> trigger;
        std::vector<Subscriber> subscribers;
        uint64_t nextSubscriberId{1};
        std::optional<nlohmann::json> lastResult;
        bool active{true};
        Operation operation; // stored for notifyFieldsChanged field matching
    };

    struct SubscriberCleanupGuard
    {
        Executor& executor;
        std::weak_ptr<SubscriptionSession> sessionWeak;
        uint64_t subId;

        ~SubscriberCleanupGuard()
        {
            if (auto session = sessionWeak.lock())
            {
                executor.removeSubscriber(session, subId);
                if (session->subscribers.empty())
                {
                    if (session->trigger)
                    {
                        session->trigger->cancel();
                    }
                }
            }
        }
    };

    void removeSubscriber(std::shared_ptr<SubscriptionSession> session,
                          uint64_t id)
    {
        auto it = std::find_if(
            session->subscribers.begin(), session->subscribers.end(),
            [id](const Subscriber& s) { return s.id == id; });
        if (it != session->subscribers.end())
        {
            if (it->disconnectTimer)
            {
                it->disconnectTimer->cancel();
            }
            session->subscribers.erase(it);
        }
    }

    Task<void> runSubscriptionLoop(std::shared_ptr<SubscriptionSession> session,
                                   nlohmann::json mergedVariables)
    {
        auto self = this->shared_from_this(); // keep executor alive

        while (session->active && !session->subscribers.empty())
        {
            // A fresh cache per tick: every tick sees live data, and it is
            // never shared with other requests.
            RequestCache cache;
            Result<nlohmann::json> tickResult =
                co_await executeSubscriptionSelections(
                    session->operation.selections, mergedVariables, cache);

            nlohmann::json event;
            if (!tickResult)
            {
                event["errors"] =
                    nlohmann::json::array({{{"message", tickResult.error()}}});
            }
            else
            {
                event["data"] = std::move(*tickResult);
            }

            session->lastResult = event;

            // Broadcast to all active subscribers
            std::vector<uint64_t> failedSubIds;
            for (auto& sub : session->subscribers)
            {
                bool ok = false;
                try
                {
                    ok = co_await sub.callback(event);
                }
                catch (...)
                {
                    ok = false;
                }
                if (!ok)
                {
                    failedSubIds.push_back(sub.id);
                }
            }

            // Clean up disconnected subscribers
            for (uint64_t id : failedSubIds)
            {
                removeSubscriber(session, id);
            }

            if (session->subscribers.empty())
            {
                break;
            }

            bool cont = co_await session->trigger->wait();
            if (!cont)
            {
                break;
            }
        }

        activeSubscriptions.erase(session->key);
    }

    // Execute a subscription: identical query/variables/trigger requests
    // are coalesced into a single background polling loop.
    template <typename AsyncEventFn>
    Task<void> executeSubscription(std::string query, nlohmann::json variables,
                                   std::shared_ptr<SubscriptionTrigger> trigger,
                                   AsyncEventFn onEvent)
    {
        Result<Operation> parseResult = Parser::parse(query);
        if (!parseResult)
        {
            nlohmann::json err = errorResponse(parseResult.error());
            co_await onEvent(std::move(err));
            co_return;
        }

        Operation operation = std::move(*parseResult);
        if (operation.type == Operation::Type::Query)
        {
            operation.type = Operation::Type::Subscription;
        }

        if (auto expanded = expandFragments(operation); !expanded)
        {
            nlohmann::json err = errorResponse(expanded.error());
            co_await onEvent(std::move(err));
            co_return;
        }

        if (auto r = schema.validateOperation(operation); !r)
        {
            nlohmann::json err = errorResponse(r.error());
            co_await onEvent(std::move(err));
            co_return;
        }

        nlohmann::json mergedVariables = initialVariables(operation);
        if (variables.is_object())
        {
            for (auto it = variables.begin(); it != variables.end(); ++it)
            {
                mergedVariables[it.key()] = it.value();
            }
        }

        std::string key =
            query + "|" + mergedVariables.dump() + "|" + trigger->triggerKey();

        auto it = activeSubscriptions.find(key);
        std::shared_ptr<SubscriptionSession> session;
        bool isNewSession = false;

        if (it == activeSubscriptions.end())
        {
            session = std::make_shared<SubscriptionSession>();
            session->key = key;
            session->trigger = std::move(trigger);
            session->operation = std::move(operation);
            activeSubscriptions[key] = session;
            isNewSession = true;
        }
        else
        {
            session = it->second;
        }

        auto exec = getExecutorForTimer(session->trigger.get());
        if (!exec)
        {
            // Nothing can run the session without an executor; undo the
            // registration made above.
            if (isNewSession)
            {
                activeSubscriptions.erase(key);
            }
            nlohmann::json err = errorResponse(exec.error());
            co_await onEvent(std::move(err));
            co_return;
        }
        auto disconnectTimer = std::make_shared<net::steady_timer>(*exec);
        disconnectTimer->expires_at(net::steady_timer::time_point::max());

        uint64_t subId = session->nextSubscriberId++;
        Subscriber subscriber{
            subId,
            [onEvent = std::move(onEvent)](nlohmann::json event) -> Task<bool> {
                co_return co_await onEvent(std::move(event));
            },
            disconnectTimer};

        session->subscribers.push_back(std::move(subscriber));

        SubscriberCleanupGuard guard{*this, session, subId};

        if (isNewSession)
        {
            spawnTask(subscriptionScope,
                      runSubscriptionLoop(session, mergedVariables),
                      "subscription loop");
        }
        else if (session->lastResult)
        {
            auto pushLast = [callback = session->subscribers.back().callback,
                             lastResult = *session->lastResult,
                             disconnectTimer]() -> Task<void> {
                bool ok = false;
                try
                {
                    ok = co_await callback(lastResult);
                }
                catch (...)
                {
                    ok = false;
                }
                if (!ok)
                {
                    disconnectTimer->cancel();
                }
            };
            spawnTask(subscriptionScope, pushLast(),
                      "subscription initial push");
        }

        auto [ec] = co_await disconnectTimer->async_wait(useSenderNoThrow);
        co_return;
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
        const FieldSelection& selection, const nlohmann::json& variables,
        RequestCache& cache)
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
            payload = co_await source->fetch(fieldSpec, args, cache);
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
            logWarning("graphql: source '{}' returned an error for '{}': {}",
                       fieldSpec.source, fieldSpec.name, payload.error());
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
            co_return co_await projectList((*payload)["Members"],
                                           fieldSpec.returnType,
                                           selection.selections, cache);
        }
        if (fieldSpec.scalar)
        {
            co_return std::move(*payload);
        }
        co_return co_await projectObject(*payload, fieldSpec.returnType,
                                         selection.selections, cache);
    }

    Task<Result<nlohmann::json>> resolveSubscriptionField(
        const FieldSelection& selection, const nlohmann::json& variables,
        RequestCache& cache)
    {
        const FieldSpec* fieldSpec =
            schema.getRootSubscriptionField(selection.name);
        if (!fieldSpec)
        {
            co_return std::unexpected(
                "Unknown subscription field: " + selection.name);
        }
        std::shared_ptr<DataSource> source =
            sources ? sources->get(fieldSpec->source) : nullptr;
        if (!source)
        {
            co_return std::unexpected(
                "Data source '" + fieldSpec->source + "' required by field '" +
                fieldSpec->name + "' is not available");
        }

        const nlohmann::json args = applyArgumentDefaults(
            argumentsToJson(selection.arguments, variables), *fieldSpec);

        Result<nlohmann::json> payload =
            std::unexpected(std::string("source did not run"));
        try
        {
            payload = co_await source->fetch(*fieldSpec, args, cache);
        }
        catch (const std::exception& e)
        {
            logWarning("graphql: source '{}' failed for subscription '{}': {}",
                       fieldSpec->source, fieldSpec->name, e.what());
            co_return std::unexpected("Failed to resolve subscription field '" +
                                      fieldSpec->name + "': " + e.what());
        }
        if (!payload)
        {
            logWarning(
                "graphql: source '{}' returned an error for subscription '{}': {}",
                fieldSpec->source, fieldSpec->name, payload.error());
            co_return std::unexpected(payload.error());
        }

        if (fieldSpec->isList)
        {
            if (!payload->is_object() || !payload->contains("Members") ||
                !(*payload)["Members"].is_array())
            {
                co_return std::unexpected(
                    "Expected collection Members array for '" +
                    fieldSpec->name + "'");
            }
            co_return co_await projectList((*payload)["Members"],
                                           fieldSpec->returnType,
                                           selection.selections, cache);
        }
        if (fieldSpec->scalar)
        {
            co_return std::move(*payload);
        }
        co_return co_await projectObject(*payload, fieldSpec->returnType,
                                         selection.selections, cache);
    }

    Task<Result<nlohmann::json>> executeSubscriptionSelections(
        const std::vector<FieldSelection>& selections,
        const nlohmann::json& variables, RequestCache& cache)
    {
        std::vector<Task<Result<nlohmann::json>>> pending;
        pending.reserve(selections.size());
        for (const FieldSelection& selection : selections)
        {
            pending.push_back(
                resolveSubscriptionField(selection, variables, cache));
        }
        auto fields = co_await gather(std::move(pending), maxConcurrentFields);

        nlohmann::json data = nlohmann::json::object();
        for (size_t i = 0; i < fields.size(); ++i)
        {
            if (!fields[i])
            {
                co_return std::unexpected(fields[i].error());
            }
            data[outputName(selections[i])] = std::move(*fields[i]);
        }
        co_return data;
    }

    static bool operationReferencesAnyField(
        const Operation& op, const std::unordered_set<std::string>& fieldNames)
    {
        for (const auto& sel : op.selections)
        {
            if (fieldNames.contains(sel.name))
            {
                return true;
            }
        }
        return false;
    }

    Result<net::any_io_executor> getExecutorForTimer(
        SubscriptionTrigger* trigger) const
    {
        if (trigger)
        {
            if (auto ex = trigger->getExecutor())
            {
                return *ex;
            }
        }
        if (ioExecutor)
        {
            return *ioExecutor;
        }
        return std::unexpected(
            std::string("No I/O executor available for subscription timer"));
    }

    Task<Result<nlohmann::json>> projectList(
        const nlohmann::json& items, const std::string& typeName,
        const std::vector<FieldSelection>& selections,
        RequestCache& cache) const
    {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& item : items)
        {
            Result<nlohmann::json> projected =
                co_await projectObject(item, typeName, selections, cache);
            if (!projected)
            {
                co_return projected;
            }
            result.push_back(std::move(*projected));
        }
        co_return result;
    }

    Task<Result<nlohmann::json>> projectField(
        const FieldSelection& selection, const FieldSpec& fieldSpec,
        const nlohmann::json& source, RequestCache& cache) const
    {
        auto it = source.find(fieldSpec.responseKey);
        if (it == source.end())
        {
            co_return nlohmann::json(nullptr);
        }
        const nlohmann::json& value = *it;
        if (fieldSpec.scalar)
        {
            co_return value;
        }
        if (value.is_null())
        {
            co_return value;
        }
        if (fieldSpec.isList)
        {
            if (fieldSpec.collectionLink)
            {
                if (!value.is_object() || !value.contains("@odata.id") ||
                    !value["@odata.id"].is_string())
                {
                    co_return std::unexpected(
                        "Expected collection link object for field '" +
                        selection.name + "'");
                }
                const std::string collUrl =
                    value["@odata.id"].get<std::string>() +
                    "?$expand=*($levels=1)";

                std::shared_ptr<DataSource> sourcePtr =
                    sources ? sources->get(fieldSpec.source) : nullptr;
                if (!sourcePtr)
                {
                    sourcePtr = sources ? sources->get(redfishSourceName)
                                        : nullptr;
                }
                if (!sourcePtr)
                {
                    co_return std::unexpected(
                        "Data source required for collection link on field '" +
                        selection.name + "' is not available");
                }

                Result<nlohmann::json> collResult =
                    co_await sourcePtr->fetchLink(collUrl, cache);
                if (!collResult)
                {
                    co_return std::unexpected(collResult.error());
                }
                if (!collResult->is_object() ||
                    !collResult->contains("Members") ||
                    !(*collResult)["Members"].is_array())
                {
                    co_return std::unexpected(
                        "Expected Members array in collection link for '" +
                        selection.name + "'");
                }

                nlohmann::json result = nlohmann::json::array();
                for (const auto& member : (*collResult)["Members"])
                {
                    Result<nlohmann::json> itemResult =
                        co_await projectObject(member, fieldSpec.returnType,
                                               selection.selections, cache);
                    if (itemResult)
                    {
                        result.push_back(std::move(*itemResult));
                    }
                    else
                    {
                        logWarning(
                            "Skipping collection link member (projection failed): {}",
                            itemResult.error());
                    }
                }
                co_return result;
            }

            if (!value.is_array())
            {
                co_return std::unexpected(
                    "Expected array for field '" + selection.name + "'");
            }
            co_return co_await projectList(value, fieldSpec.returnType,
                                           selection.selections, cache);
        }
        co_return co_await projectObject(value, fieldSpec.returnType,
                                         selection.selections, cache);
    }

    Task<Result<nlohmann::json>> projectObject(
        const nlohmann::json& source, const std::string& typeName,
        const std::vector<FieldSelection>& selections,
        RequestCache& cache) const
    {
        const ObjectSpec* objectSpec = schema.getObject(typeName);
        if (objectSpec == nullptr)
        {
            co_return std::unexpected("Unknown object type: " + typeName);
        }
        if (!source.is_object())
        {
            co_return std::unexpected(
                "Expected object for type '" + typeName + "'");
        }
        if (selections.empty())
        {
            co_return source;
        }
        nlohmann::json result = nlohmann::json::object();
        for (const FieldSelection& selection : selections)
        {
            auto fieldIt = objectSpec->fields.find(selection.name);
            if (fieldIt == objectSpec->fields.end())
            {
                co_return std::unexpected("Unknown field '" + selection.name +
                                          "' on type '" + typeName + "'");
            }
            Result<nlohmann::json> field = co_await projectField(
                selection, fieldIt->second, source, cache);
            if (!field)
            {
                co_return field;
            }
            result[outputName(selection)] = std::move(*field);
        }
        co_return result;
    }

    Schema schema;
    std::shared_ptr<const SourceRegistry> sources;
    std::optional<net::any_io_executor> ioExecutor;
    size_t maxConcurrentFields = 8;
    std::unordered_map<std::string, std::shared_ptr<SubscriptionSession>>
        activeSubscriptions;
    exec::async_scope subscriptionScope;
};

} // namespace sutra::graphql
