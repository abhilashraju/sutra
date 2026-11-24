#include "sutra/cert_generator.hpp"
#include "sutra/http_server.hpp"
#include "sutra/web_client.hpp"

#include <set>

#include <gtest/gtest.h>

using namespace sutra;

namespace
{

/** Shared state the route handlers use to observe the clients. */
struct Observed
{
    std::set<unsigned short> remotePorts;
    int inFlight = 0;
    int maxInFlight = 0;
    int flakyCalls = 0;
    int flakyPostCalls = 0;
};

HttpRouter makeRouter(net::io_context& ioc, Observed& seen)
{
    HttpRouter router;
    router.get("/hello", [&seen](HttpRequest&, RequestContext& ctx) {
        seen.remotePorts.insert(ctx.remote.port());
        return makeResponse(http::status::ok, "world");
    });
    router.get("/echo", [](HttpRequest&, RequestContext& ctx) {
        return makeResponse(http::status::ok,
                            ctx.queryParam("a") + "|" + ctx.queryParam("b"));
    });
    router.post("/json", [](HttpRequest& req, RequestContext&) {
        return makeResponse(http::status::ok,
                            std::string(req[http::field::content_type]) + "|" +
                                req.body());
    });
    router.get("/close", [&seen](HttpRequest&, RequestContext& ctx) {
        seen.remotePorts.insert(ctx.remote.port());
        auto res = makeResponse(http::status::ok, "bye");
        res.keep_alive(false);
        return res;
    });
    router.get(
        "/slow",
        [&ioc, &seen](HttpRequest&, RequestContext& ctx) -> Task<HttpResponse> {
            seen.remotePorts.insert(ctx.remote.port());
            seen.maxInFlight = std::max(seen.maxInFlight, ++seen.inFlight);
            net::steady_timer timer(ioc);
            timer.expires_after(100ms);
            co_await timer.async_wait(useSenderNoThrow);
            --seen.inFlight;
            co_return makeResponse(http::status::ok, "slow");
        });
    router.get("/flaky", [&seen](HttpRequest&, RequestContext&) {
        if (++seen.flakyCalls < 3)
        {
            return makeResponse(http::status::service_unavailable, "busy");
        }
        return makeResponse(http::status::ok, "recovered");
    });
    router.post("/flaky", [&seen](HttpRequest&, RequestContext&) {
        ++seen.flakyPostCalls;
        return makeResponse(http::status::service_unavailable, "busy");
    });
    router.sse("/events",
               [](HttpRequest&, RequestContext&, SseWriter& out) -> Task<void> {
                   for (int i = 0; i < 3; ++i)
                   {
                       if (!co_await out.write("tick " + std::to_string(i)))
                       {
                           co_return;
                       }
                   }
               });
    return router;
}

/** Runs `body` as a task, then stops `server` so ioc.run() returns. */
template <class Server, class Body>
void runClient(net::io_context& ioc, Server& server, Body& body)
{
    exec::async_scope scope;
    bool failed = false;
    auto wrapper = [&]() -> Task<void> {
        try
        {
            co_await body();
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "client failed: " << e.what();
            failed = true;
        }
        server.stop();
    };
    spawnTask(scope, wrapper(), "web client test");
    ioc.run();
    EXPECT_FALSE(failed);
}

tcp::endpoint loopback()
{
    return tcp::endpoint(net::ip::make_address("127.0.0.1"), 0);
}

std::string baseUrl(const HttpServer& server, const char* scheme = "http")
{
    return std::string(scheme) +
           "://127.0.0.1:" + std::to_string(server.localEndpoint().port());
}

} // namespace

TEST(ParseUrl, SplitsParts)
{
    auto u = parseUrl("http://example.com/a/b?x=1#frag");
    EXPECT_FALSE(u.tls);
    EXPECT_EQ(u.host, "example.com");
    EXPECT_EQ(u.port, "80");
    EXPECT_EQ(u.target, "/a/b?x=1");

    u = parseUrl("https://host:8443");
    EXPECT_TRUE(u.tls);
    EXPECT_EQ(u.port, "8443");
    EXPECT_EQ(u.target, "/");

    u = parseUrl("http://[::1]:9000/p");
    EXPECT_EQ(u.host, "::1");
    EXPECT_EQ(u.port, "9000");

    u = parseUrl("http://h?q=1");
    EXPECT_EQ(u.target, "/?q=1");
}

TEST(ParseUrl, RejectsBadInput)
{
    EXPECT_THROW(parseUrl("ftp://host/"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http:///path"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://host:0/"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://host:99999/"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://host:12ab/"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://user@host/"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://host/a b"), std::invalid_argument);
    EXPECT_THROW(parseUrl("http://host/a\r\nX: y"), std::invalid_argument);
}

TEST(WebClient, BuildsRequests)
{
    net::io_context ioc;
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    WebClient client(pool);
    client.withUrl("http://example.com:8080/base")
        .withParams({{"a b", "c&d"}, {"z", "1"}})
        .withHeader("X-Test", "1")
        .withJsonBody({{"k", "v"}})
        .withMethod(http::verb::post);
    auto req = client.buildRequest();
    EXPECT_EQ(req.target(), "/base?a%20b=c%26d&z=1");
    EXPECT_EQ(req[http::field::host], "example.com:8080");
    EXPECT_EQ(req[http::field::content_type], "application/json");
    EXPECT_EQ(req["X-Test"], "1");
    EXPECT_EQ(req.body(), R"({"k":"v"})");

    EXPECT_THROW(client.withHeader("Bad", "a\r\nb").buildRequest(),
                 std::invalid_argument);
    EXPECT_THROW(client.withUrl("https://example.com/"), std::invalid_argument);
}

TEST(WebClient, ReusesKeepAliveConnection)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    auto body = [&]() -> Task<void> {
        for (int i = 0; i < 5; ++i)
        {
            WebClient client(pool);
            auto res = co_await client.withUrl(url + "/hello").execute();
            EXPECT_EQ(res.result(), http::status::ok);
            EXPECT_EQ(res.body(), "world");
        }
        EndpointKey key{"127.0.0.1",
                        std::to_string(server.localEndpoint().port()), false};
        EXPECT_EQ(pool->idleCount(key), 1u);
        EXPECT_EQ(pool->activeCount(key), 0u);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.remotePorts.size(), 1u);
}

TEST(WebClient, ConnectionCloseIsNotReused)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    auto body = [&]() -> Task<void> {
        for (int i = 0; i < 3; ++i)
        {
            WebClient client(pool);
            auto res = co_await client.withUrl(url + "/close").execute();
            EXPECT_EQ(res.body(), "bye");
        }
        EndpointKey key{"127.0.0.1",
                        std::to_string(server.localEndpoint().port()), false};
        EXPECT_EQ(pool->idleCount(key), 0u);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.remotePorts.size(), 3u);
}

TEST(WebClient, IdleConnectionsExpire)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    ConnectionPoolConfig config;
    config.idleTimeout = 50ms;
    auto pool = HttpConnectionPool::create(ioc.get_executor(), nullptr, config);
    const auto url = baseUrl(server);

    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        client.withUrl(url + "/hello");
        co_await client.execute();
        net::steady_timer timer(ioc);
        timer.expires_after(120ms);
        co_await timer.async_wait(useSenderNoThrow);
        co_await client.execute();
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.remotePorts.size(), 2u);
}

TEST(WebClient, PeerClosedIdleConnectionIsDropped)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServerOptions options;
    // The server drops idle keep-alive connections quickly.
    options.requestTimeout = 50ms;
    HttpServer server(ioc.get_executor(), loopback(), router, options);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        client.withUrl(url + "/hello");
        auto first = co_await client.execute();
        EXPECT_EQ(first.body(), "world");
        net::steady_timer timer(ioc);
        timer.expires_after(200ms);
        co_await timer.async_wait(useSenderNoThrow);
        auto second = co_await client.execute();
        EXPECT_EQ(second.body(), "world");
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.remotePorts.size(), 2u);
}

TEST(WebClient, LimitsConnectionsPerHostAndWaits)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    ConnectionPoolConfig config;
    config.maxConnectionsPerHost = 2;
    auto pool = HttpConnectionPool::create(ioc.get_executor(), nullptr, config);
    const auto url = baseUrl(server);
    EndpointKey key{"127.0.0.1", std::to_string(server.localEndpoint().port()),
                    false};

    int completed = 0;
    size_t maxWaiting = 0;
    auto worker = [&](int) -> Task<void> {
        WebClient client(pool);
        auto res = co_await client.withUrl(url + "/slow").execute();
        EXPECT_EQ(res.body(), "slow");
        ++completed;
    };
    auto body = [&]() -> Task<void> {
        exec::async_scope scope;
        for (int i = 0; i < 5; ++i)
        {
            spawnTask(scope, worker(i), "worker");
        }
        net::steady_timer timer(ioc);
        timer.expires_after(30ms);
        co_await timer.async_wait(useSenderNoThrow);
        maxWaiting = pool->waiterCount(key);
        co_await scope.on_empty();
    };
    runClient(ioc, server, body);
    EXPECT_EQ(completed, 5);
    EXPECT_EQ(seen.maxInFlight, 2);
    EXPECT_LE(seen.remotePorts.size(), 2u);
    EXPECT_EQ(maxWaiting, 3u);
}

TEST(WebClient, AcquireTimesOutWhenPoolIsExhausted)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    ConnectionPoolConfig config;
    config.maxConnectionsPerHost = 1;
    config.acquireTimeout = 30ms;
    auto pool = HttpConnectionPool::create(ioc.get_executor(), nullptr, config);
    const auto url = baseUrl(server);

    bool timedOut = false;
    auto slow = [&]() -> Task<void> {
        WebClient client(pool);
        co_await client.withUrl(url + "/slow").execute();
    };
    auto body = [&]() -> Task<void> {
        exec::async_scope scope;
        spawnTask(scope, slow(), "slow request");
        net::steady_timer timer(ioc);
        timer.expires_after(10ms);
        co_await timer.async_wait(useSenderNoThrow);
        try
        {
            WebClient client(pool);
            co_await client.withUrl(url + "/hello").withRetries(1).execute();
        }
        catch (const system_error& e)
        {
            timedOut = e.code() ==
                       make_error_code(boost::system::errc::timed_out);
        }
        co_await scope.on_empty();
    };
    runClient(ioc, server, body);
    EXPECT_TRUE(timedOut);
}

TEST(WebClient, RetriesServerErrorsOfIdempotentRequests)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);
    WebClient::RetryPolicy policy;
    policy.maxTries = 3;
    policy.initialBackoff = 1ms;

    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        auto res = co_await client.withUrl(url + "/flaky")
                       .withRetryPolicy(policy)
                       .execute();
        EXPECT_EQ(res.result(), http::status::ok);
        EXPECT_EQ(res.body(), "recovered");

        // POST is not idempotent: the 503 is returned after one attempt.
        WebClient post(pool);
        auto res2 = co_await post.withUrl(url + "/flaky")
                        .withMethod(http::verb::post)
                        .withRetryPolicy(policy)
                        .execute();
        EXPECT_EQ(res2.result(), http::status::service_unavailable);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.flakyCalls, 3);
    EXPECT_EQ(seen.flakyPostCalls, 1);
}

TEST(WebClient, ReturnsLastServerErrorWhenRetriesRunOut)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);
    WebClient::RetryPolicy policy;
    policy.maxTries = 2;
    policy.initialBackoff = 1ms;

    auto body = [&]() -> Task<void> {
        // "/flaky" fails twice before it recovers; two attempts are not
        // enough, so the last 503 comes back.
        WebClient client(pool);
        auto res = co_await client.withUrl(url + "/flaky")
                       .withRetryPolicy(policy)
                       .execute();
        EXPECT_EQ(res.result(), http::status::service_unavailable);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.flakyCalls, 2);
}

TEST(WebClient, ConnectFailureIsRetriedThenThrown)
{
    net::io_context ioc;
    // Find a port nothing listens on.
    unsigned short freePort = 0;
    {
        tcp::acceptor probe(ioc, loopback());
        freePort = probe.local_endpoint().port();
    }
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    WebClient::RetryPolicy policy;
    policy.maxTries = 3;
    policy.initialBackoff = 1ms;

    bool refused = false;
    exec::async_scope scope;
    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        try
        {
            co_await client
                .withUrl("http://127.0.0.1:" + std::to_string(freePort))
                .withRetryPolicy(policy)
                .execute();
        }
        catch (const system_error& e)
        {
            refused = e.code() == net::error::connection_refused;
        }
        EndpointKey key{"127.0.0.1", std::to_string(freePort), false};
        EXPECT_EQ(pool->activeCount(key), 0u);
    };
    spawnTask(scope, body(), "connect failure");
    ioc.run();
    EXPECT_TRUE(refused);
}

TEST(WebClient, PostsJsonAndEncodesParams)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        auto res = co_await client.withUrl(url + "/json")
                       .withMethod(http::verb::post)
                       .withJsonBody({{"n", 1}})
                       .execute();
        EXPECT_EQ(res.body(), R"(application/json|{"n":1})");

        WebClient query(pool);
        auto echoed = co_await query.withUrl(url + "/echo")
                          .withParams({{"a", "x y"}, {"b", "&="}})
                          .execute();
        EXPECT_EQ(echoed.body(), "x y|&=");
    };
    runClient(ioc, server, body);
}

TEST(WebClient, StreamsServerSentEvents)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    std::vector<std::string> events;
    http::status status = http::status::unknown;
    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        status = co_await client.withUrl(url + "/events")
                     .executeAsStream([&](std::string data) {
                         events.push_back(std::move(data));
                         return true;
                     });
        EndpointKey key{"127.0.0.1",
                        std::to_string(server.localEndpoint().port()), false};
        // A streaming connection is never returned to the pool.
        EXPECT_EQ(pool->idleCount(key), 0u);
        EXPECT_EQ(pool->activeCount(key), 0u);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(status, http::status::ok);
    EXPECT_EQ(events, (std::vector<std::string>{"tick 0", "tick 1", "tick 2"}));
}

TEST(WebClient, StreamStopsWhenHandlerReturnsFalse)
{
    net::io_context ioc;
    Observed seen;
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    const auto url = baseUrl(server);

    std::vector<std::string> events;
    auto body = [&]() -> Task<void> {
        WebClient client(pool);
        co_await client.withUrl(url + "/events")
            .executeAsStream([&](std::string data) {
                events.push_back(std::move(data));
                return false;
            });
    };
    runClient(ioc, server, body);
    EXPECT_EQ(events, (std::vector<std::string>{"tick 0"}));
}

TEST(SecureWebClient, VerifiedRoundTripAndReuse)
{
    net::io_context ioc;
    Observed seen;
    auto pem = generateSelfSignedCertificate("localhost");
    auto serverContext = makeServerContext(pem.certificate, pem.privateKey);
    auto clientContext = makeClientContext(pem.certificate);
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router, serverContext);
    server.start();
    auto pool = HttpsConnectionPool::create(ioc.get_executor(), &clientContext);
    const auto port = std::to_string(server.localEndpoint().port());

    auto body = [&]() -> Task<void> {
        for (int i = 0; i < 3; ++i)
        {
            SecureWebClient client(pool);
            auto res =
                co_await client.withUrl("https://localhost:" + port + "/hello")
                    .execute();
            EXPECT_EQ(res.body(), "world");
        }
        EndpointKey key{"localhost", port, true};
        EXPECT_EQ(pool->idleCount(key), 1u);
    };
    runClient(ioc, server, body);
    EXPECT_EQ(seen.remotePorts.size(), 1u);
}

TEST(SecureWebClient, UntrustedServerIsRejectedWithoutRetry)
{
    net::io_context ioc;
    Observed seen;
    auto pem = generateSelfSignedCertificate("localhost");
    auto serverContext = makeServerContext(pem.certificate, pem.privateKey);
    auto other = generateSelfSignedCertificate("localhost");
    auto clientContext = makeClientContext(other.certificate);
    auto router = makeRouter(ioc, seen);
    HttpServer server(ioc.get_executor(), loopback(), router, serverContext);
    server.start();
    auto pool = HttpsConnectionPool::create(ioc.get_executor(), &clientContext);
    const auto port = std::to_string(server.localEndpoint().port());

    int attempts = 0;
    bool rejected = false;
    exec::async_scope scope;
    auto body = [&]() -> Task<void> {
        SecureWebClient client(pool);
        try
        {
            co_await client.withUrl("https://localhost:" + port + "/hello")
                .withRetries(3)
                .execute();
        }
        catch (const system_error& e)
        {
            rejected = true;
            attempts = e.code().category() == net::error::get_ssl_category();
        }
        server.stop();
    };
    spawnTask(scope, body(), "untrusted");
    ioc.run();
    EXPECT_TRUE(rejected);
    // The failure is a TLS verification error, which is not retried.
    EXPECT_EQ(attempts, 1);
}
