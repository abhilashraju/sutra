# sutra

Networking building blocks for C++23 on top of **std::execution**
([stdexec](https://github.com/NVIDIA/stdexec)) and **Boost.Asio / Beast**.
Every asynchronous operation is a sender; coroutines are `exec::task<T>`.
There is no custom scheduler: work runs on an Asio executor (`onExecutor()`
hops onto one) or on stdexec's own pools.

| Layer | Header(s) |
| --- | --- |
| Basics | `config.hpp`, `log.hpp`, `spawn.hpp`, `wait_queue.hpp`, `gather.hpp`, `command_line.hpp` |
| TCP / TLS | `connection.hpp`, `tcp_client.hpp`, `tcp_server.hpp`, `cert_generator.hpp` |
| HTTP | `http_target.hpp`, `http_router.hpp`, `http_server.hpp`, `http_client.hpp` |
| Pooling and retries | `connection_pool.hpp`, `web_client.hpp` |
| D-Bus (optional) | `dbus.hpp` |
| GraphQL | `graphql/{parser,schema,executor,data_source,dbus_source,redfish_source,endpoint}.hpp` |

## Requirements

* GCC 14+ (developed with 15.2), C++23
* meson >= 1.3, Boost >= 1.86 (headers), OpenSSL, nlohmann_json
* libsystemd (`log.hpp` writes to the journal)
* **stdexec with Asio support** (see below)
* optional: sdbusplus (D-Bus layer, GraphQL D-Bus source)
* optional: GoogleTest (tests)

### Installing stdexec with Asio support

sutra needs `<exec/asio/use_sender.hpp>`, which stdexec installs only when it
is configured for Asio:

```sh
git clone https://github.com/NVIDIA/stdexec && cd stdexec
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DSTDEXEC_ENABLE_ASIO=ON -DSTDEXEC_ASIO_IMPLEMENTATION=boost \
      -DSTDEXEC_BUILD_TESTS=OFF -DSTDEXEC_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX=$HOME/stdexec-install
cmake --build build && cmake --install build
```

### Building

```sh
export CMAKE_PREFIX_PATH=$HOME/stdexec-install   # where stdexec was installed
meson setup build .
meson compile -C build
meson test -C build --print-errorlogs
```

Options: `-Ddbus=enabled|disabled|auto`, `-Dtests=...`, `-Dexamples=...`.
`scripts/build.sh` wraps these steps with the toolchain paths of the
development host.

Notes:

* Do **not** define `BOOST_ASIO_DISABLE_THREADS`: stdexec's `use_sender`
  relies on `std::recursive_mutex`. `sdbusplus.pc` injects that macro, so
  `meson.build` links `libsdbusplus` directly instead of using the
  pkg-config dependency.
* Logging goes to the systemd journal by default (`sd_journal_send`, program
  name as `SYSLOG_IDENTIFIER`, stderr only if the journal is unreachable).
  `SUTRA_LOG_LEVEL` filters at compile time; `setLogSink()` replaces the
  sink. Debug lines are sent at INFO priority, as in coroserver, because
  OpenBMC's journald drops anything below INFO.
* `BOOST_ASIO_NO_DEPRECATED` is on (`steady_timer::cancel()` takes no
  arguments).
* Link `sutra_dep` for the core and `sutra_dbus_dep` for the D-Bus layer; the
  latter defines `SUTRA_HAVE_DBUS=1` when sdbusplus is available.

## API tour

All snippets run inside a `Task<>` (an `exec::task`). Run one with
`spawnTask(scope, task, "what")` on an `exec::async_scope`, then `ioc.run()`.

### Basics (`config.hpp`)

```cpp
using namespace sutra;
net::io_context ioc;
exec::async_scope scope;

net::steady_timer timer(ioc);
timer.expires_after(10ms);
auto [ec] = co_await timer.async_wait(useSenderNoThrow);  // error as value
```

Fallible sutra operations return `Expected<T>`, an alias of
`std::expected<T, boost::system::error_code>`. Errors raised by sutra itself
(bad URL, bad header, invalid JSON, ...) use `Errc`, which converts to an
`error_code`:

```cpp
auto url = parseUrl("ftp://host/");
if (!url && url.error() == Errc::unsupportedScheme) { /* ... */ }
```

`WaitQueue` is a condition-variable-like primitive:
`bool woken = co_await queue.wait(timeout);` with `notifyOne()` /
`notifyAll()`.

`gather()` runs a runtime-sized vector of tasks concurrently (`ex::when_all`
has a fixed arity), bounded by `maxConcurrent` (default 8):

```cpp
std::vector<Task<int>> tasks = ...;
std::vector<int> results = co_await gather(std::move(tasks), 4);
```

Results keep input order. `gather()` is generic and keeps the task's own
failure channel: if a task throws, its siblings still run to
completion and the first failure in input order is rethrown. A stop request is
forwarded to the children and the gather completes as stopped. Everything stays
on the calling io_context thread, so no locking is needed.

`CommandLine` is a small declarative argument parser for programs built on
sutra. It keeps coroserver's alias syntax and adds boolean flags,
`--name=value`, and errors for unknown options, missing values and bad numbers
(coroserver's `parseCommandline()` only handles `key value` pairs):

```cpp
CommandLine cmd;
cmd.addOption("--port,-p", "N", "port to listen on");
cmd.addFlag("--tls,-t", "serve HTTPS");
if (auto parsed = cmd.parse(argc, argv); !parsed)
{
    std::fprintf(stderr, "%s\n%s", parsed.error().c_str(),
                 cmd.usage(argv[0]).c_str());
    return 2;
}
auto port = cmd.number<uint16_t>("--port");  // expected<optional<T>, string>
bool tls = cmd.isSet("--tls");
```

### TCP and TLS

```cpp
TcpServer server(ioc.get_executor(), "127.0.0.1", 9000,
                 [](TcpConnection conn) -> Task<void> {
                     auto line = co_await conn.readUntil("\n");
                     if (line)
                     {
                         co_await conn.write(*line);
                     }
                 });
if (auto started = server.start(); !started)   // binds and listens;
{                                              // 0.0.0.0 / :: are refused
    log(started.error().message());
}

TcpClient client(ioc.get_executor());
if (auto connected = co_await client.connect("127.0.0.1", "9000"); !connected)
{
    /* connected.error(): resolve/connect failure, or timed_out */
}
```

`TcpServer` / `HttpServer` constructors do not fail for runtime input: opening
and binding the listener happens in `start()`, which returns `Expected<void>`.

`Connection<Stream>` owns the read buffer and a watchdog: every operation has
a timeout (`setTimeout`) and `run(initiate)` wraps any Beast operation.
`write`, `readSome`, `readUntil`, `readExactly` and `run` return
`Task<Expected<...>>`; a peer that closes the connection yields
`boost::asio::error::eof`.
`cert_generator.hpp` creates self-signed PEMs and TLS 1.2+ contexts:
`generateSelfSignedCertificate(host)`, `makeServerContext(cert, key)` and
`makeClientContext(trustedPem)` (peer verification is always on; with an empty
`trustedPem` the system store is used). All of them return `Expected<...>`; an
OpenSSL failure is an `error_code` in the OpenSSL error category, a bad
argument an `Errc`.

### HTTP server and client

```cpp
HttpRouter router;
router.get("/items/{id}", [](HttpRequest&, RequestContext& ctx) {
    return jsonResponse(http::status::ok,
                        "{\"id\":\"" + ctx.param("id") + "\"}");
});
router.post("/echo", [](HttpRequest& req, RequestContext&) -> Task<HttpResponse> {
    co_return makeResponse(http::status::ok, req.body());
});
router.sse("/events", [](HttpRequest&, RequestContext&, SseWriter& out) -> Task<void> {
    for (int i = 0; i < 3; ++i)
        if (!co_await out.write("tick " + std::to_string(i))) co_return;
});

HttpServer server(ioc.get_executor(), endpoint, router);            // HTTP
HttpServer tls(ioc.get_executor(), endpoint, router, *serverCtx);   // HTTPS
if (!server.start()) { /* Expected<void> */ }

HttpClient client(ioc.get_executor());
if (co_await client.connect("127.0.0.1", port))
{
    auto res = co_await client.send(request);   // Expected<HttpResponse>
    if (res) { use(res->result(), res->body()); }
}
```

`HttpServer` supports keep-alive, a body size limit and request timeouts
(`HttpServerOptions`). `HttpClient` / `HttpsClient` are one connection each:
`connect(host, port)` returns `Expected<void>`, `send(req)` returns
`Expected<HttpResponse>` and `subscribe(req, onEvent)` for SSE returns
`Expected<http::status>`. HTTPS sends SNI and verifies the certificate against
the host name; a failed verification is an error in the TLS category.

### Connection pool and `WebClient`

```cpp
auto pool = HttpConnectionPool::create(ioc.get_executor());  // 5/host, 30 s idle
WebClient web(pool);
auto res = co_await web.withUrl("http://127.0.0.1:8080/items?x=1")
                       .withMethod(http::verb::get)
                       .withRetries(3)
                       .execute();            // Expected<HttpResponse>
if (!res) { /* res.error(): Errc, timed_out, connect/TLS error, ... */ }
auto json = co_await web.withUrl(...).executeJson();   // Expected<json>
auto status = co_await web.withUrl(".../events").executeAsStream(
    [](std::string event) { return true; /* false stops the stream */ });
```

`parseUrl()` returns `Expected<ParsedUrl>` and `buildRequest()` returns
`Expected<HttpRequest>`. The fluent setters never fail: `withUrl()` remembers
a bad URL (or an `http` URL on a TLS client, `Errc::schemeMismatch`) and
`execute()` / `buildRequest()` report it.

* `ConnectionPoolConfig{maxConnectionsPerHost = 5, idleTimeout = 30 s,
  acquireTimeout = 10 s, ...}`; `acquire()` waits (through a `WaitQueue`)
  until a lease frees up and returns `timed_out` otherwise. Leases are RAII and
  return to the pool unless marked bad. Idle connections expire and
  connections closed by the peer are dropped before reuse.
* HTTPS: `SecureWebClient` / `HttpsConnectionPool::create(executor, &ctx)`
  with a `makeClientContext(...)`. Verification cannot be turned off.
* Retry policy: connect errors are always retried; transport errors only for
  idempotent verbs (or when a reused keep-alive connection was stale); 5xx
  responses only for idempotent verbs. Exponential backoff; TLS and protocol
  errors are never retried.

### D-Bus (`dbus.hpp`, needs `SUTRA_HAVE_DBUS`)

```cpp
auto conn = makeConnection(ioc, /*session=*/false); // works with older sdbusplus too
if (!conn) { /* conn.error(): the errno-style code sdbusplus reported */ }
auto& bus = **conn;
auto state = co_await getProperty<std::string>(
    bus, "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
    "xyz.openbmc_project.State.Host", "CurrentHostState");
if (!state) { /* state.error() is an error_code */ }
auto tree = co_await getSubTree(bus, "/xyz/openbmc_project/sensors", 0,
                                {"xyz.openbmc_project.Sensor.Value"});
```

Same shape as the coroserver helpers: `dbusMethodCall<Ret...>`,
`getProperty`, `setProperty`, `getAllProperties`, `callObjectMapperMethod`,
`getSubTree`, `getObjects`, `getSubTreePaths`, the `getAssociated*` family,
`getManagedObjects`, `introspect`. `makeConnection` returns
`Expected<std::shared_ptr<connection>>`. Each helper returns
`Task<DbusExpected<T>>`
(`std::expected<T, error_code>`); failures are returned, not thrown.
`getPropertyFromMap` / `getPropertiesFromMap` return `DbusExpected` too.

### GraphQL (`graphql/`)

* `Parser::parse/validate` return `Result<Operation>` / `Result<void>`
  (`Result<T>` is `std::expected<T, std::string>`; queries, variables with
  defaults, directives, named and inline fragments, depth limit 20).
  `expandFragments()` returns `Result<void>`.
* `Schema` is JSON driven (`objects[]`, `queries[]`; see
  `examples/graphql_server/schema.json`). Each root query names a `source`
  and an optional `sourceConfig`.
* `DataSource::fetch(FieldSpec, args, RequestCache&) -> Task<Result<json>>`;
  the executor creates one `RequestCache` per query execution and per
  subscription tick, so identical requests (e.g. the same Redfish URL) are
  fetched once per request, concurrent requests never share or invalidate each
  other's entries, and subscription ticks always see live data. Register
  sources in a `SourceRegistry`; `applySourceRegistry` drops (or rejects)
  fields whose source is missing and validates the rest at start-up.
  Built in: `MemorySource` and, with D-Bus, `DbusSource` (declarative
  `sourceConfig`: subtree, interfaces, path/property mappings, transforms such
  as `sensorId`, `sensorUnit`, `functionalToHealth`).
* `RedfishSource` (`"source": "redfish"`) reads a Redfish service through
  `WebClient`. `sourceConfig` is `{"path": "/redfish/v1/Chassis/{chassisId}/Sensors",
  "expandMembers": true}`: `{name}` placeholders are filled from the field's
  arguments (percent-encoded, so an argument cannot add path segments or a
  query; each must be a declared argument), `expandMembers` appends
  `?$expand=*($levels=1)` to list fields. A collection whose members are bare
  `@odata.id` links is followed (4 requests at a time; unreachable members are
  skipped with a warning). The schema's `responseKey`s are Redfish property
  names. `RedfishHttpBackend` / `RedfishHttpsBackend` (pooled, TLS verified
  against `makeClientContext`) send `X-Auth-Token` or HTTP Basic credentials
  (`RedfishCredentials`). They are created with
  `RedfishHttpBackend::create(pool, baseUrl, credentials)`, which returns
  `Expected<std::shared_ptr<...>>` (a bad base URL is an error, not an
  exception). Implement `RedfishBackend` to supply another transport.
* `Executor::execute(query, variables) -> Task<json>` returns `{"data":...}`
  or `{"errors":[{"message":...}]}`.
* `registerGraphqlEndpoint(router, executor, options)` adds
  `POST /graphql` (JSON `{query, variables}` or `application/graphql`) and
  `GET /graphql?query=&variables=`; query and variable sizes are limited
  (`EndpointOptions`).

```cpp
auto schema = graphql::Schema::fromFile("schema.json");
auto registry = std::make_shared<graphql::SourceRegistry>();
registry->add("memory", memorySource);
std::vector<std::string> warnings;
graphql::applySourceRegistry(*schema, *registry, /*strict=*/true, warnings);
auto executor = std::make_shared<graphql::Executor>(std::move(*schema), registry);
graphql::registerGraphqlEndpoint(router, executor);
```

## Example: GraphQL server

`examples/graphql_server` serves a Redfish-like schema over HTTP or HTTPS,
backed by a Redfish service and, optionally, D-Bus.

```sh
graphql_server --redfish https://bmc:443 --redfish-ca /etc/ssl/certs/authority \
               --redfish-user root               # Redfish service (default
                                                 # https://127.0.0.1:443, CA
                                                 # directory as shown)
graphql_server --redfish https://bmc:443 \
               --redfish-client-cert client.pem \
               --redfish-client-key client.key   # mutual TLS to Redfish
graphql_server --redfish http://127.0.0.1:9000   # plain-http Redfish
graphql_server --bus system                      # + live D-Bus sensors
graphql_server --cert c.pem --key k.pem          # HTTPS (cert and key required)
graphql_server --no-tls                          # plain HTTP
curl -s --cacert cert.pem https://localhost:8080/graphql \
     -H 'Content-Type: application/json' \
     -d '{"query":"{ sensors { Name Reading } }"}'
```

Redfish is the default source: the example schema's `serviceRoot`, `systems`,
`system`, `chassis`, `ethernetInterfaces`, `sensors` and `sensor` fields all
read from the service given by `--redfish`. D-Bus is an optional addition:
`--bus session|system` (default `none`) enables the `dbus*` fields; without it
they are dropped at start-up. When the Redfish service requires mutual TLS
(bmcweb with client-certificate authentication), pass
`--redfish-client-cert` and `--redfish-client-key` (PEM, given together); the
certificate is presented during the TLS handshake and the server is still
verified against `--redfish-ca`. The Redfish token or password come from the
environment (`SUTRA_REDFISH_TOKEN`, or `SUTRA_REDFISH_PASSWORD` together with
`--redfish-user`), never from the command line.

It binds to `127.0.0.1` by default and refuses wildcard addresses.
`scripts/demo_graphql.sh` (with `examples/graphql_server/demo_redfish.py` as a
fake Redfish service) and `scripts/demo_graphql_dbus.sh` (with
`dbus-run-session` and `examples/graphql_server/demo_bus.py` as a fake
ObjectMapper) reproduce the sessions used to test it.

## Error and cancellation model

* **Errors are values.** Every fallible public operation returns
  `Expected<T>` (`std::expected<T, boost::system::error_code>`); the
  GraphQL layer uses `Result<T>` (`std::expected<T, std::string>`). Nothing in
  the public API throws for runtime failures. Library-defined failures use
  `Errc` (`invalidUrl`, `unsupportedScheme`, `schemeMismatch`, `missingHost`,
  `invalidTarget`, `invalidHeader`, `invalidJson`, `opensslFailure`,
  `invalidCaPath`, `incompleteClientCredentials`, `unspecifiedAddress`);
  everything else is the Asio / OpenSSL / sd-bus code that occurred.
  Internally the operations use `useSenderNoThrow`, so an error is never
  turned into an exception. The only exceptions are programmer errors in
  constructors and setup calls (`std::invalid_argument`): a null pool, a TLS
  client or pool without an `ssl::context`, or a `CommandLine` option declared
  without a name. The GraphQL parser throws internally but catches at the
  `Parser::parse` boundary.
* **Timeouts** are enforced per operation by the connection watchdog and
  are returned as `timed_out`.
* **Cancellation is the stopped channel.** A stop request on an operation's
  environment cancels the pending Asio operation, which completes as
  *stopped* (not as an error). `spawnTask` logs errors and swallows stops, so
  stopping a server or an `exec::async_scope` unwinds all its sessions.
  `TcpServer::stop()` / `HttpServer::stop()` close the listener and request
  stop on all sessions.
* A pending **D-Bus call cannot be revoked**; a stop is noticed when the
  reply (or sdbusplus' own timeout) arrives.
* **HTTP layer:** protocol violations answer 400/413/etc. and close the
  connection; a route handler that throws becomes a `500` with a generic body
  (details go to the log only).
* **Tasks you write** may still throw: `gather()` and `spawnTask` keep the
  task's own exception channel, and `spawnTask` logs what it catches.
* **GraphQL:** invalid queries, validation failures and data-source failures
  are reported in `errors` with HTTP 200; malformed HTTP requests (bad JSON,
  oversized query, wrong types) get 400.

## Differences from coroserver and limitations

* Handlers and resolvers are `Task<>` (stdexec) instead of
  `boost::asio::awaitable`.
* GraphQL: no mutations, no introspection, no `@skip/@include` evaluation, no
  `__typename`. The per-request cache does not deduplicate identical GETs that
  are in flight at the same moment, only ones that already completed.
* Root fields, the objects of a D-Bus query and the interfaces of each object
  are resolved concurrently through `gather()` (at most 8 in flight per level;
  `Executor::setMaxConcurrentFields()` changes the root-field limit). Results
  keep query / mapper order.
* The Redfish source only reads (GET); there is no session login or token
  refresh, so supply an existing token or Basic credentials.
* TLS peer verification cannot be disabled by design.
* `Connection` cannot interrupt a D-Bus call; Beast `tcp_stream` timers are
  not used (they are unsafe with `use_sender`).
* The tests run on Linux only; they are developed against stdexec 0.11.0.

## License

MIT.
