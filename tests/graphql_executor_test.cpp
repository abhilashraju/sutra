#include "graphql_fixture.hpp"
#include "sutra/spawn.hpp"

#include <gtest/gtest.h>

using namespace sutra;
using namespace sutra::graphql;
using nlohmann::json;

namespace
{

/** Runs one query to completion on a fresh io_context. */
json run(Executor& executor, std::string query, json variables = json::object())
{
    net::io_context ioc;
    exec::async_scope scope;
    json out;
    auto body = [&]() -> Task<void> {
        out = co_await executor.execute(std::move(query), std::move(variables));
    };
    spawnTask(scope, body(), "graphql executor test");
    ioc.run();
    return out;
}

std::string firstError(const json& response)
{
    if (!response.contains("errors") || response["errors"].empty())
    {
        return {};
    }
    return response["errors"][0]["message"].get<std::string>();
}

} // namespace

TEST(GraphqlExecutor, ScalarRootField)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "{ version }");
    EXPECT_EQ(r["data"]["version"], "1.2.3");
}

TEST(GraphqlExecutor, ListWithProjectionAndAlias)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "{ all: sensors { name: Id Reading } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    const auto& list = r["data"]["all"];
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0]["name"], "temp_cpu");
    EXPECT_EQ(list[0]["Reading"], 41.5);
    EXPECT_FALSE(list[0].contains("Id"));
    EXPECT_FALSE(list[0].contains("Status"));
}

TEST(GraphqlExecutor, NestedObjects)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "{ sensors { Id Status { Health } } }");
    EXPECT_EQ(r["data"]["sensors"][1]["Status"]["Health"], "Warning");
    EXPECT_FALSE(r["data"]["sensors"][1]["Status"].contains("State"));
}

TEST(GraphqlExecutor, NestedListInsideObject)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "{ chassis { Name Sensors { Id } } }");
    EXPECT_EQ(r["data"]["chassis"]["Name"], "chassis0");
    EXPECT_EQ(r["data"]["chassis"]["Sensors"].size(), 2u);
}

TEST(GraphqlExecutor, ArgumentsSelectOneObject)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, R"({ sensor(id: "fan_1") { Reading } })");
    EXPECT_EQ(r["data"]["sensor"]["Reading"], 3200.0);
}

TEST(GraphqlExecutor, VariablesAndDefaults)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "query Q($id: String) { sensor(id: $id) { Id } }",
                 {{"id", "temp_cpu"}});
    EXPECT_EQ(r["data"]["sensor"]["Id"], "temp_cpu");

    r = run(*executor,
            R"(query Q($id: String = "fan_1") { sensor(id: $id) { Id } })");
    EXPECT_EQ(r["data"]["sensor"]["Id"], "fan_1");

    // A supplied variable overrides the declared default.
    r = run(*executor,
            R"(query Q($id: String = "fan_1") { sensor(id: $id) { Id } })",
            {{"id", "temp_cpu"}});
    EXPECT_EQ(r["data"]["sensor"]["Id"], "temp_cpu");
}

TEST(GraphqlExecutor, Fragments)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor,
                 "query { sensors { ...Basic ... on Sensor { Reading } } }"
                 " fragment Basic on Sensor { Id }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["sensors"][0]["Id"], "temp_cpu");
    EXPECT_EQ(r["data"]["sensors"][0]["Reading"], 41.5);
}

TEST(GraphqlExecutor, MultipleRootFields)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, "{ version chassis { Name } }");
    EXPECT_EQ(r["data"]["version"], "1.2.3");
    EXPECT_EQ(r["data"]["chassis"]["Name"], "chassis0");
}

TEST(GraphqlExecutor, ValidationErrors)
{
    auto executor = graphql_fixture::makeExecutor();
    EXPECT_NE(firstError(run(*executor, "{ nope }")).find("Unknown field"),
              std::string::npos);
    EXPECT_NE(firstError(run(*executor, "{ sensors { Bogus } }"))
                  .find("Unknown field 'Bogus'"),
              std::string::npos);
    EXPECT_NE(firstError(run(*executor, "{ sensor { Id } }"))
                  .find("Missing required argument"),
              std::string::npos);
    EXPECT_NE(firstError(run(*executor, R"({ sensor(id: "a", x: 1) { Id } })"))
                  .find("Unknown argument"),
              std::string::npos);
    EXPECT_NE(
        firstError(run(*executor, "{ version { x } }")).find("Scalar field"),
        std::string::npos);
    EXPECT_NE(
        firstError(run(*executor, "mutation { version }")).find("Only query"),
        std::string::npos);
}

TEST(GraphqlExecutor, SyntaxAndFragmentErrors)
{
    auto executor = graphql_fixture::makeExecutor();
    EXPECT_FALSE(firstError(run(*executor, "{ sensors { Id }")).empty());
    EXPECT_NE(firstError(run(*executor, "{ sensors { ...Nope } }"))
                  .find("Unknown fragment"),
              std::string::npos);
}

TEST(GraphqlExecutor, SourceFailureIsReported)
{
    auto executor = graphql_fixture::makeExecutor();
    json r = run(*executor, R"({ sensor(id: "missing") { Id } })");
    EXPECT_FALSE(r.contains("data"));
    EXPECT_NE(firstError(r).find("not found"), std::string::npos);
}

TEST(GraphqlExecutor, UnavailableSourceFieldsAreDropped)
{
    auto schema = Schema::fromJson(graphql_fixture::schemaJson());
    ASSERT_TRUE(schema);
    SourceRegistry registry;
    registry.add("memory", graphql_fixture::makeMemory());

    // Re-point one field at a source nobody registered.
    schema->mutableRootQueries()["version"].source = "ghost";
    std::vector<std::string> warnings;
    EXPECT_FALSE(applySourceRegistry(*schema, registry, true, warnings));
    EXPECT_TRUE(applySourceRegistry(*schema, registry, false, warnings));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(schema->getRootQueryField("version"), nullptr);
    EXPECT_NE(schema->getRootQueryField("sensors"), nullptr);
}

TEST(GraphqlExecutor, BadSchemaIsRejected)
{
    EXPECT_FALSE(Schema::fromJson(json::object()));
    EXPECT_FALSE(Schema::fromJson(json::parse(R"({"objects":[{"x":1}]})")));
    EXPECT_FALSE(Schema::fromFile("/nonexistent/schema.json"));
}

TEST(GraphqlExecutor, ThrowingSourceBecomesError)
{
    struct Thrower : DataSource
    {
        Result<void> validate(const FieldSpec&) const override
        {
            return {};
        }

        Task<Result<json>> fetch(const FieldSpec&, const json&) override
        {
            throw std::runtime_error("boom");
            co_return json();
        }
    };
    auto schema = Schema::fromJson(graphql_fixture::schemaJson());
    auto registry = std::make_shared<SourceRegistry>();
    registry->add("memory", std::make_shared<Thrower>());
    Executor executor(std::move(*schema), registry);
    json r = run(executor, "{ version }");
    EXPECT_NE(firstError(r).find("boom"), std::string::npos);
}

namespace
{

/** Sleeps in every fetch and records how many fetches overlap. */
class SlowSource : public DataSource
{
  public:
    SlowSource(net::io_context& context, std::chrono::milliseconds firstDelay,
               std::chrono::milliseconds step) :
        ioc(context), first(firstDelay), decrement(step)
    {}

    Result<void> validate(const FieldSpec&) const override
    {
        return {};
    }

    Task<Result<json>> fetch(const FieldSpec&, const json&) override
    {
        const int index = started++;
        ++running;
        peak = std::max(peak, running);
        net::steady_timer timer(ioc, first - index * decrement);
        co_await timer.async_wait(useSender);
        --running;
        co_return json("v" + std::to_string(index));
    }

    int started = 0;
    int running = 0;
    int peak = 0;

  private:
    net::io_context& ioc;
    std::chrono::milliseconds first;
    std::chrono::milliseconds decrement;
};

} // namespace

TEST(GraphqlExecutor, RootFieldsResolveConcurrentlyInQueryOrder)
{
    net::io_context ioc;
    // The first field is the slowest, the last the fastest.
    auto slow = std::make_shared<SlowSource>(ioc, 100ms, 20ms);
    auto schema = Schema::fromJson(graphql_fixture::schemaJson());
    ASSERT_TRUE(schema);
    auto registry = std::make_shared<SourceRegistry>();
    registry->add("memory", slow);
    std::vector<std::string> warnings;
    ASSERT_TRUE(applySourceRegistry(*schema, *registry, true, warnings));
    Executor executor(std::move(*schema), registry);

    json out;
    exec::async_scope scope;
    auto body = [&]() -> Task<void> {
        out = co_await executor.execute("{ a: version b: version c: version "
                                        "d: version e: version }");
    };
    spawnTask(scope, body(), "concurrent root fields");
    auto begin = std::chrono::steady_clock::now();
    ioc.run();
    auto took = std::chrono::steady_clock::now() - begin;

    ASSERT_TRUE(out.contains("data")) << out.dump();
    // Values follow the selection order, not the completion order.
    EXPECT_EQ(out["data"]["a"], "v0");
    EXPECT_EQ(out["data"]["b"], "v1");
    EXPECT_EQ(out["data"]["e"], "v4");
    EXPECT_EQ(slow->peak, 5);
    // Sequentially: 100+80+60+40+20 = 300 ms.
    EXPECT_LT(took, 200ms);
}

TEST(GraphqlExecutor, RootFieldConcurrencyIsBounded)
{
    net::io_context ioc;
    auto slow = std::make_shared<SlowSource>(ioc, 10ms, 0ms);
    auto schema = Schema::fromJson(graphql_fixture::schemaJson());
    ASSERT_TRUE(schema);
    auto registry = std::make_shared<SourceRegistry>();
    registry->add("memory", slow);
    std::vector<std::string> warnings;
    ASSERT_TRUE(applySourceRegistry(*schema, *registry, true, warnings));
    Executor executor(std::move(*schema), registry);
    executor.setMaxConcurrentFields(2);

    json out;
    exec::async_scope scope;
    auto body = [&]() -> Task<void> {
        out = co_await executor.execute("{ a: version b: version c: version "
                                        "d: version e: version }");
    };
    spawnTask(scope, body(), "bounded root fields");
    ioc.run();
    ASSERT_TRUE(out.contains("data")) << out.dump();
    EXPECT_EQ(slow->started, 5);
    EXPECT_EQ(slow->peak, 2);
}
