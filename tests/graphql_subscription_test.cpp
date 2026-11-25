#include "graphql_fixture.hpp"
#include "sutra/graphql/executor.hpp"
#include "sutra/graphql/ql_dbus_property_watcher.hpp"
#include "sutra/spawn.hpp"

#include <gtest/gtest.h>

using namespace sutra;
using namespace sutra::graphql;
using nlohmann::json;

TEST(GraphqlSubscription, DbusWatcherIndexResolution)
{
    std::vector<graphql::DbusWatcher> watchers = {
        {"/xyz/openbmc_project/state/host0",
         "xyz.openbmc_project.State.Host",
         "CurrentHostState",
         {"systemStatus"}},
        {"/xyz/openbmc_project/state/chassis0",
         "xyz.openbmc_project.State.Chassis",
         "CurrentPowerState",
         {"chassisStatus", "chassisUpdates"}},
        {"",
         "xyz.openbmc_project.Network.EthernetInterface",
         "",
         {"ethernetInterfaceUpdates"}},
        {"",
         "xyz.openbmc_project.Sensor.Value",
         "Value",
         {"sensorUpdates", "dbusSensorUpdates"}},
        {"",
         "xyz.openbmc_project.Sensor.Threshold.Warning",
         "",
         {"sensorUpdates"}},
    };

    DbusWatcherIndex index = buildDbusWatcherIndex(watchers);

    // Exact match
    auto fields1 =
        resolveFields(index, "/xyz/openbmc_project/state/host0",
                      "xyz.openbmc_project.State.Host", "CurrentHostState");
    ASSERT_EQ(fields1.size(), 1u);
    EXPECT_EQ(fields1[0], "systemStatus");

    // Multiple fields on exact match
    auto fields2 =
        resolveFields(index, "/xyz/openbmc_project/state/chassis0",
                      "xyz.openbmc_project.State.Chassis", "CurrentPowerState");
    ASSERT_EQ(fields2.size(), 2u);
    EXPECT_EQ(fields2[0], "chassisStatus");
    EXPECT_EQ(fields2[1], "chassisUpdates");

    // Wildcard object + specific property
    auto fields3 = resolveFields(index, "/xyz/openbmc_project/sensors/temp_cpu",
                                 "xyz.openbmc_project.Sensor.Value", "Value");
    ASSERT_EQ(fields3.size(), 2u);
    EXPECT_EQ(fields3[0], "sensorUpdates");
    EXPECT_EQ(fields3[1], "dbusSensorUpdates");

    // Wildcard object + wildcard property
    auto fields4 = resolveFields(
        index, "/xyz/openbmc_project/network/eth0",
        "xyz.openbmc_project.Network.EthernetInterface", "LinkUp");
    ASSERT_EQ(fields4.size(), 1u);
    EXPECT_EQ(fields4[0], "ethernetInterfaceUpdates");

    // Unrelated interface resolves to empty
    auto fields5 = resolveFields(index, "/xyz/openbmc_project/unknown",
                                 "xyz.openbmc_project.Unknown", "Prop");
    EXPECT_TRUE(fields5.empty());
}

TEST(GraphqlSubscription, TimerTrigger)
{
    net::io_context ioc;
    exec::async_scope scope;

    TimerTrigger trigger(ioc.get_executor(), std::chrono::milliseconds(20));
    EXPECT_EQ(trigger.triggerType(), "timer");

    bool waited = false;
    auto testWait = [&]() -> Task<void> { waited = co_await trigger.wait(); };

    spawnTask(scope, testWait(), "timer trigger test");
    ioc.run();
    EXPECT_TRUE(waited);
}

TEST(GraphqlSubscription, EventTriggerFireAndCancel)
{
    net::io_context ioc;
    exec::async_scope scope;

    EventTrigger trigger(ioc.get_executor(), std::chrono::milliseconds(10));
    EXPECT_EQ(trigger.triggerType(), "event");
    EXPECT_EQ(trigger.triggerKey(), "event");

    int wakes = 0;
    auto loop = [&]() -> Task<void> {
        while (co_await trigger.wait())
        {
            wakes++;
            if (wakes >= 2)
            {
                trigger.cancel();
            }
        }
    };

    auto fires = [&]() -> Task<void> {
        net::steady_timer t(ioc.get_executor());
        t.expires_after(std::chrono::milliseconds(5));
        auto [ec1] = co_await t.async_wait(useSenderNoThrow);
        trigger.fire();

        t.expires_after(std::chrono::milliseconds(25));
        auto [ec2] = co_await t.async_wait(useSenderNoThrow);
        trigger.fire();
    };

    spawnTask(scope, loop(), "event trigger loop");
    spawnTask(scope, fires(), "event trigger fires");
    ioc.run();

    EXPECT_EQ(wakes, 2);
}

TEST(GraphqlSubscription, ExecuteSubscriptionEventTrigger)
{
    net::io_context ioc;
    exec::async_scope scope;

    auto executor = graphql_fixture::makeExecutor(ioc.get_executor());
    auto trigger = std::make_shared<EventTrigger>(
        ioc.get_executor(), std::chrono::milliseconds(10));

    std::vector<json> receivedEvents;
    auto subTask = [&]() -> Task<void> {
        co_await executor->executeSubscription(
            "subscription { sensorUpdates { Id Reading } }", json::object(),
            trigger, [&](json event) -> Task<bool> {
                receivedEvents.push_back(std::move(event));
                co_return true;
            });
    };

    auto notifyTask = [&]() -> Task<void> {
        net::steady_timer timer(ioc.get_executor());
        // Initial tick runs immediately on start.
        timer.expires_after(std::chrono::milliseconds(15));
        auto [ec1] = co_await timer.async_wait(useSenderNoThrow);

        // Fire notification
        executor->notifyFieldsChanged({"sensorUpdates"});

        timer.expires_after(std::chrono::milliseconds(30));
        auto [ec2] = co_await timer.async_wait(useSenderNoThrow);

        // Cancel trigger to finish subscription
        trigger->cancel();
    };

    spawnTask(scope, subTask(), "subscription task");
    spawnTask(scope, notifyTask(), "notify task");
    ioc.run();

    ASSERT_GE(receivedEvents.size(), 2u);
    for (const auto& ev : receivedEvents)
    {
        ASSERT_TRUE(ev.contains("data"));
        EXPECT_EQ(ev["data"]["sensorUpdates"].size(), 2u);
    }
}

TEST(GraphqlSubscription, SubscriptionMultiplexingAndStats)
{
    net::io_context ioc;
    exec::async_scope scope;

    auto executor = graphql_fixture::makeExecutor(ioc.get_executor());
    auto trigger = std::make_shared<EventTrigger>(
        ioc.get_executor(), std::chrono::milliseconds(10));

    std::vector<json> sub1Events;
    std::vector<json> sub2Events;

    auto sub1Task = [&]() -> Task<void> {
        co_await executor->executeSubscription(
            "subscription { sensorUpdates { Id } }", json::object(), trigger,
            [&](json event) -> Task<bool> {
                sub1Events.push_back(std::move(event));
                co_return true;
            });
    };

    auto sub2Task = [&]() -> Task<void> {
        co_await executor->executeSubscription(
            "subscription { sensorUpdates { Id } }", json::object(), trigger,
            [&](json event) -> Task<bool> {
                sub2Events.push_back(std::move(event));
                co_return true;
            });
    };

    auto checkStatsAndStop = [&]() -> Task<void> {
        net::steady_timer timer(ioc.get_executor());
        timer.expires_after(std::chrono::milliseconds(15));
        auto [ec] = co_await timer.async_wait(useSenderNoThrow);

        json stats = executor->getSubscriptionStats();
        EXPECT_EQ(stats["active_shared_loops"], 1u);
        EXPECT_EQ(stats["total_client_subscribers"], 2u);
        EXPECT_EQ(stats["multiplexed_subscribers"], 1u);

        trigger->cancel();
    };

    spawnTask(scope, sub1Task(), "sub1 task");
    spawnTask(scope, sub2Task(), "sub2 task");
    spawnTask(scope, checkStatsAndStop(), "stats check task");
    ioc.run();

    EXPECT_FALSE(sub1Events.empty());
    EXPECT_FALSE(sub2Events.empty());
}

TEST(GraphqlSubscription, ExecuteShorthandSubscriptionQuery)
{
    net::io_context ioc;
    exec::async_scope scope;

    auto executor = graphql_fixture::makeExecutor(ioc.get_executor());
    auto trigger = std::make_shared<EventTrigger>(
        ioc.get_executor(), std::chrono::milliseconds(10));

    std::vector<json> receivedEvents;
    auto subTask = [&]() -> Task<void> {
        // Shorthand query omitting 'subscription' keyword
        co_await executor->executeSubscription(
            "{ sensorUpdates { Id Reading } }", json::object(), trigger,
            [&](json event) -> Task<bool> {
                receivedEvents.push_back(std::move(event));
                co_return true;
            });
    };

    auto stopTask = [&]() -> Task<void> {
        net::steady_timer timer(ioc.get_executor());
        timer.expires_after(std::chrono::milliseconds(15));
        auto [ec] = co_await timer.async_wait(useSenderNoThrow);
        trigger->cancel();
    };

    spawnTask(scope, subTask(), "subscription task");
    spawnTask(scope, stopTask(), "stop task");
    ioc.run();

    ASSERT_FALSE(receivedEvents.empty());
    ASSERT_TRUE(receivedEvents[0].contains("data"));
    EXPECT_EQ(receivedEvents[0]["data"]["sensorUpdates"].size(), 2u);
}
