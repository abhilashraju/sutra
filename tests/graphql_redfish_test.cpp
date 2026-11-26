#include "sutra/graphql/executor.hpp"
#include "sutra/graphql/redfish_source.hpp"
#include "sutra/http_server.hpp"
#include "sutra/spawn.hpp"

#include <algorithm>
#include <map>

#include <gtest/gtest.h>

using namespace sutra;
using namespace sutra::graphql;
using nlohmann::json;

namespace
{

/** Canned Redfish tree keyed by target; records the targets asked for. */
class FakeRedfish : public RedfishBackend
{
  public:
    Task<Result<json>> get(std::string target) override
    {
        calls.push_back(target);
        auto it = tree.find(target);
        if (it == tree.end())
        {
            co_return std::unexpected("not found: " + target);
        }
        co_return it->second;
    }

    std::map<std::string, json> tree;
    std::vector<std::string> calls;
};

std::shared_ptr<FakeRedfish> makeFake()
{
    auto fake = std::make_shared<FakeRedfish>();
    fake->tree["/redfish/v1"] = {{"Id", "RootService"},
                                 {"RedfishVersion", "1.17.0"}};
    fake->tree["/redfish/v1/Chassis"] = {
        {"Members",
         {{{"@odata.id", "/redfish/v1/Chassis/c1"}},
          {{"@odata.id", "/redfish/v1/Chassis/missing"}},
          {{"@odata.id", "/redfish/v1/Chassis/c2"}}}}};
    fake->tree["/redfish/v1/Chassis/c1"] = {
        {"Id", "c1"},
        {"Status", {{"Health", "OK"}}},
        {"Sensors", {{"@odata.id", "/redfish/v1/Chassis/c1/Sensors"}}}};
    fake->tree["/redfish/v1/Chassis/c2"] = {
        {"Id", "c2"}, {"Status", {{"Health", "Critical"}}}};
    fake->tree["/redfish/v1/Chassis/c1/Sensors?$expand=*($levels=1)"] = {
        {"Members",
         {{{"@odata.id", "/redfish/v1/Chassis/c1/Sensors/t"},
           {"Id", "t"},
           {"Reading", 41.5}},
          {{"@odata.id", "/redfish/v1/Chassis/c1/Sensors/f"},
           {"Id", "f"},
           {"Reading", 3200.0}}}}};
    return fake;
}

json schemaJson()
{
    return json::parse(R"({
  "objects": [
    {"name": "Status", "fields": [
      {"name": "Health", "responseKey": "Health", "returnType": "String",
       "scalar": true}]},
    {"name": "Root", "fields": [
      {"name": "Id", "responseKey": "Id", "returnType": "String",
       "scalar": true},
      {"name": "RedfishVersion", "responseKey": "RedfishVersion",
       "returnType": "String", "scalar": true}]},
    {"name": "Chassis", "fields": [
      {"name": "Id", "responseKey": "Id", "returnType": "String",
       "scalar": true},
      {"name": "Status", "responseKey": "Status", "returnType": "Status"},
      {"name": "sensors", "responseKey": "Sensors", "returnType": "Sensor",
       "isList": true, "collectionLink": true}]},
    {"name": "Sensor", "fields": [
      {"name": "Id", "responseKey": "Id", "returnType": "String",
       "scalar": true},
      {"name": "Reading", "responseKey": "Reading", "returnType": "Float",
       "scalar": true}]}
  ],
  "queries": [
    {"name": "serviceRoot", "returnType": "Root", "source": "redfish",
     "sourceConfig": {"path": "/redfish/v1"}},
    {"name": "chassisList", "returnType": "Chassis", "isList": true,
     "source": "redfish",
     "sourceConfig": {"path": "/redfish/v1/Chassis"}},
    {"name": "chassis", "returnType": "Chassis", "source": "redfish",
     "arguments": [{"name": "id", "typeName": "ID", "required": true}],
     "sourceConfig": {"path": "/redfish/v1/Chassis/{id}"}},
    {"name": "sensors", "returnType": "Sensor", "isList": true,
     "source": "redfish",
     "arguments": [{"name": "chassisId", "typeName": "ID", "default": "c1"}],
     "sourceConfig": {"path": "/redfish/v1/Chassis/{chassisId}/Sensors",
                      "expandMembers": true}}
  ]
})");
}

Executor makeExecutor(std::shared_ptr<RedfishBackend> backend)
{
    auto registry = std::make_shared<SourceRegistry>();
    registry->add(redfishSourceName,
                  std::make_shared<RedfishSource>(std::move(backend)));
    auto schema = Schema::fromJson(schemaJson());
    EXPECT_TRUE(schema.has_value());
    std::vector<std::string> warnings;
    auto applied = applySourceRegistry(*schema, *registry, true, warnings);
    EXPECT_TRUE(applied.has_value());
    return Executor(std::move(*schema), registry);
}

json run(Executor& executor, std::string query)
{
    net::io_context ioc;
    exec::async_scope scope;
    json out;
    auto body = [&]() -> Task<void> {
        out = co_await executor.execute(std::move(query));
    };
    spawnTask(scope, body(), "graphql redfish test");
    ioc.run();
    return out;
}

Result<void> validateConfig(json config, json arguments = json::array())
{
    json field = {{"name", "f"},
                  {"returnType", "Root"},
                  {"source", "redfish"},
                  {"arguments", std::move(arguments)}};
    if (!config.is_null())
    {
        field["sourceConfig"] = std::move(config);
    }
    json doc = {{"objects", json::array()}, {"queries", {field}}};
    auto schema = Schema::fromJson(doc);
    EXPECT_TRUE(schema.has_value());
    RedfishSource source(nullptr);
    return source.validate(*schema->getRootQueryField("f"));
}

} // namespace

TEST(GraphqlRedfish, ObjectField)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r = run(executor, "{ serviceRoot { Id RedfishVersion } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["serviceRoot"]["Id"], "RootService");
    EXPECT_EQ(r["data"]["serviceRoot"]["RedfishVersion"], "1.17.0");
}

TEST(GraphqlRedfish, ArgumentFillsPathAndNestedObjectIsProjected)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r = run(executor, R"({ chassis(id: "c2") { Id Status { Health } } })");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["chassis"]["Id"], "c2");
    EXPECT_EQ(r["data"]["chassis"]["Status"]["Health"], "Critical");
    ASSERT_EQ(fake->calls.size(), 1u);
    EXPECT_EQ(fake->calls[0], "/redfish/v1/Chassis/c2");
}

TEST(GraphqlRedfish, CollectionLinkTraversedAndProjected)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r =
        run(executor, R"({ chassis(id: "c1") { Id sensors { Id Reading } } })");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["chassis"]["Id"], "c1");
    const auto& sensors = r["data"]["chassis"]["sensors"];
    ASSERT_EQ(sensors.size(), 2u);
    EXPECT_EQ(sensors[0]["Id"], "t");
    EXPECT_EQ(sensors[1]["Id"], "f");
}

TEST(GraphqlRedfish, ArgumentCannotEscapeThePath)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    run(executor, R"({ chassis(id: "../../Systems?x=1 y") { Id } })");
    ASSERT_EQ(fake->calls.size(), 1u);
    EXPECT_EQ(fake->calls[0],
              "/redfish/v1/Chassis/..%2F..%2FSystems%3Fx%3D1%20y");
}

TEST(GraphqlRedfish, LinkMembersAreFetchedAndFailuresSkipped)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r = run(executor, "{ chassisList { Id Status { Health } } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    const auto& list = r["data"]["chassisList"];
    // "missing" cannot be fetched and is dropped; order is preserved.
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0]["Id"], "c1");
    EXPECT_EQ(list[1]["Id"], "c2");
    EXPECT_EQ(list[1]["Status"]["Health"], "Critical");
    EXPECT_EQ(fake->calls.size(), 4u);
}

TEST(GraphqlRedfish, ExpandMembersUsesOneRequestAndSchemaDefault)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r = run(executor, "{ sensors { Id Reading } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    const auto& list = r["data"]["sensors"];
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0]["Id"], "t");
    EXPECT_EQ(list[1]["Reading"], 3200.0);
    ASSERT_EQ(fake->calls.size(), 1u);
    EXPECT_EQ(fake->calls[0],
              "/redfish/v1/Chassis/c1/Sensors?$expand=*($levels=1)");
}

TEST(GraphqlRedfish, BackendErrorIsReported)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);
    json r = run(executor, R"({ chassis(id: "nope") { Id } })");
    EXPECT_FALSE(r.contains("data"));
    EXPECT_NE(r["errors"][0]["message"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST(GraphqlRedfish, ListWithoutMembersIsAnError)
{
    auto fake = makeFake();
    fake->tree["/redfish/v1/Chassis"] = {{"Name", "oops"}};
    Executor executor = makeExecutor(fake);
    json r = run(executor, "{ chassisList { Id } }");
    EXPECT_FALSE(r.contains("data"));
    EXPECT_NE(r["errors"][0]["message"].get<std::string>().find("Members"),
              std::string::npos);
}

TEST(GraphqlRedfish, ValidatesConfiguration)
{
    EXPECT_TRUE(validateConfig({{"path", "/redfish/v1"}}));
    EXPECT_FALSE(validateConfig(nullptr));
    EXPECT_FALSE(validateConfig({{"path", ""}}));
    EXPECT_FALSE(validateConfig({{"path", "redfish/v1"}}));
    EXPECT_FALSE(validateConfig({{"path", "/a/{x}"}}));
    EXPECT_FALSE(validateConfig({{"path", "/a/{x"}}));
    EXPECT_FALSE(validateConfig({{"path", "/a/x}"}}));
    EXPECT_FALSE(validateConfig({{"path", "/a/{}"}}));
    EXPECT_FALSE(validateConfig({{"path", "/a"}, {"expandMembers", "yes"}}));
    EXPECT_TRUE(
        validateConfig({{"path", "/a/{x}"}}, json::array({{{"name", "x"}}})));
}

namespace
{

struct Seen
{
    std::string auth;
    std::string token;
    std::string accept;
};

HttpRouter makeRouter(Seen& seen)
{
    HttpRouter router;
    router.get("/redfish/v1", [&seen](HttpRequest& req, RequestContext&) {
        seen.auth = std::string(req[http::field::authorization]);
        seen.token = std::string(req["X-Auth-Token"]);
        seen.accept = std::string(req[http::field::accept]);
        return jsonResponse(http::status::ok, R"({"Id":"RootService"})");
    });
    router.get("/redfish/v1/bad", [](HttpRequest&, RequestContext&) {
        return jsonResponse(http::status::ok, "not json");
    });
    router.get("/redfish/v1/denied", [](HttpRequest&, RequestContext&) {
        return makeResponse(http::status::unauthorized, "no");
    });
    return router;
}

template <class Body>
void runWithServer(Body body)
{
    net::io_context ioc;
    Seen seen;
    auto router = makeRouter(seen);
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.localEndpoint().port());

    exec::async_scope scope;
    auto wrapper = [&]() -> Task<void> {
        try
        {
            co_await body(pool, url, seen);
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "client failed: " << e.what();
        }
        server.stop();
    };
    spawnTask(scope, wrapper(), "redfish web backend test");
    ioc.run();
}

} // namespace

TEST(RedfishWebBackend, GetsJsonWithBasicAuth)
{
    runWithServer([](auto pool, std::string url, Seen& seen) -> Task<void> {
        RedfishHttpBackend backend(pool, url, {"", "root", "0penBmc"});
        auto r = co_await backend.get("/redfish/v1");
        EXPECT_TRUE(r.has_value());
        if (r)
        {
            EXPECT_EQ((*r)["Id"], "RootService");
        }
        EXPECT_EQ(seen.auth, "Basic cm9vdDowcGVuQm1j");
        EXPECT_EQ(seen.accept, "application/json");
    });
}

TEST(RedfishWebBackend, TokenWinsOverPassword)
{
    runWithServer([](auto pool, std::string url, Seen& seen) -> Task<void> {
        RedfishHttpBackend backend(pool, url, {"tok", "root", "pw"});
        auto r = co_await backend.get("/redfish/v1");
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(seen.token, "tok");
        EXPECT_TRUE(seen.auth.empty());
    });
}

TEST(RedfishWebBackend, ReportsHttpAndJsonErrors)
{
    runWithServer([](auto pool, std::string url, Seen&) -> Task<void> {
        RedfishHttpBackend backend(pool, url);
        auto denied = co_await backend.get("/redfish/v1/denied");
        EXPECT_FALSE(denied.has_value());
        EXPECT_NE(denied.error().find("HTTP 401"), std::string::npos);
        auto bad = co_await backend.get("/redfish/v1/bad");
        EXPECT_FALSE(bad.has_value());
        EXPECT_NE(bad.error().find("invalid JSON"), std::string::npos);
        auto missing = co_await backend.get("/redfish/v1/none");
        EXPECT_FALSE(missing.has_value());
        EXPECT_NE(missing.error().find("HTTP 404"), std::string::npos);
    });
}

TEST(RedfishWebBackend, RejectsBadBaseUrl)
{
    net::io_context ioc;
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    EXPECT_THROW(RedfishHttpBackend(pool, "https://host"),
                 std::invalid_argument);
    EXPECT_THROW(RedfishHttpBackend(pool, "http://host/redfish/v1"),
                 std::invalid_argument);
}

namespace
{

std::size_t countCalls(const FakeRedfish& fake, const std::string& target)
{
    return static_cast<std::size_t>(
        std::count(fake.calls.begin(), fake.calls.end(), target));
}

} // namespace

TEST(RequestCache, SameUrlFetchedOncePerQuery)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);

    json r = run(executor, R"({
        a: chassis(id: "c1") { Id }
        b: chassis(id: "c1") { Id }
    })");
    EXPECT_EQ(r["data"]["a"]["Id"], "c1");
    EXPECT_EQ(r["data"]["b"]["Id"], "c1");
    EXPECT_EQ(countCalls(*fake, "/redfish/v1/Chassis/c1"), 1u);
}

TEST(RequestCache, NotSharedBetweenQueries)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);

    run(executor, R"({ chassis(id: "c1") { Id } })");
    run(executor, R"({ chassis(id: "c1") { Id } })");
    // Each query has its own cache, so the second one fetches again.
    EXPECT_EQ(countCalls(*fake, "/redfish/v1/Chassis/c1"), 2u);
}

TEST(RequestCache, ConcurrentQueriesDoNotShareEntries)
{
    auto fake = makeFake();
    Executor executor = makeExecutor(fake);

    net::io_context ioc;
    exec::async_scope scope;
    json first, second;
    auto one = [&]() -> Task<void> {
        first = co_await executor.execute(R"({ chassis(id: "c1") { Id } })");
    };
    auto two = [&]() -> Task<void> {
        second = co_await executor.execute(R"({ chassis(id: "c1") { Id } })");
    };
    spawnTask(scope, one(), "request cache query 1");
    spawnTask(scope, two(), "request cache query 2");
    ioc.run();

    EXPECT_EQ(first["data"]["chassis"]["Id"], "c1");
    EXPECT_EQ(second["data"]["chassis"]["Id"], "c1");
    EXPECT_EQ(countCalls(*fake, "/redfish/v1/Chassis/c1"), 2u);
}
