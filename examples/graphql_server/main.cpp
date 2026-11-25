/**
 * @file main.cpp
 * @brief GraphQL server over HTTP(S) with a Redfish-like schema.
 *
 * Root fields come from a Redfish service ("redfish", by default the one on
 * https://127.0.0.1:443). With --bus session|system, and when built with
 * D-Bus support, the live bus ("dbus") is added as an optional second source.
 *
 *   graphql_server [--address 127.0.0.1] [--port 8080] [--schema FILE]
 *                  [--redfish URL] [--redfish-user NAME] [--redfish-ca DIR]
 *                  [--redfish-server-name NAME]
 *                  [--bus session|system|none] [--no-tls]
 *                  [--cert FILE --key FILE]   (--help lists the short forms)
 *
 * Redfish secrets are read from the environment, never from the command
 * line: SUTRA_REDFISH_TOKEN (X-Auth-Token) or SUTRA_REDFISH_PASSWORD (with
 * --redfish-user, HTTP Basic).
 *
 * The server speaks HTTPS unless --no-tls is given; HTTPS requires --cert and
 * --key (no certificate is generated). An https Redfish
 * service is verified against the CA directory --redfish-ca (default
 * /etc/ssl/certs/authority), and its certificate must carry the host in the
 * URL, or --redfish-server-name when the certificate names a different host
 * than the address used. The server refuses wildcard addresses.
 */
#include "sutra/cert_generator.hpp"
#include "sutra/command_line.hpp"
#include "sutra/graphql/endpoint.hpp"
#include "sutra/graphql/redfish_source.hpp"
#include "sutra/http_server.hpp"

#ifdef SUTRA_HAVE_DBUS
#include "sutra/graphql/dbus_source.hpp"
#include "sutra/graphql/ql_dbus_property_watcher.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef SUTRA_EXAMPLE_SCHEMA
#define SUTRA_EXAMPLE_SCHEMA "schema.json"
#endif

using namespace sutra;

namespace
{

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options
{
    std::string address = "127.0.0.1";
    uint16_t port = 18080;
    std::string schemaPath = SUTRA_EXAMPLE_SCHEMA;
    std::string redfishUrl = "https://127.0.0.1:443";
    std::string bus = "system";
    bool tls = true;
    std::string certPath = "/etc/ssl/certs/https/server_cert.pem";
    std::string keyPath = "/etc/ssl/private/server_pkey.pem";
    std::string redfishUser;
    std::string redfishCa = "/etc/ssl/certs/authority";
    std::string redfishServerName;
};

CommandLine makeCommandLine()
{
    CommandLine cmd;
    cmd.addOption("--address,-a", "ADDR",
                  "listen address (default 127.0.0.1, wildcards refused)");
    cmd.addOption("--port,-p", "N", "listen port (default 8080)");
    cmd.addOption("--schema,-s", "FILE",
                  "schema JSON (default: the example schema.json)");
    cmd.addOption("--redfish", "URL",
                  "Redfish service to read from (default "
                  "https://127.0.0.1:443)");
    cmd.addOption("--redfish-user", "NAME",
                  "Redfish user (password: SUTRA_REDFISH_PASSWORD)");
    cmd.addOption("--redfish-ca", "DIR",
                  "CA directory trusted for an https Redfish service "
                  "(default /etc/ssl/certs/authority)");
    cmd.addOption("--redfish-server-name", "NAME",
                  "name the Redfish certificate must carry (SNI and "
                  "verification), when it differs from the --redfish host");
    cmd.addOption("--bus,-b", "session|system|none",
                  "also read D-Bus fields from this bus (default none)");
    cmd.addFlag("--no-tls", "serve plain HTTP instead of HTTPS");
    cmd.addOption("--cert,-c", "FILE",
                  "TLS certificate (PEM), required for HTTPS");
    cmd.addOption("--key,-k", "FILE",
                  "TLS private key (PEM), required for HTTPS");
    cmd.addFlag("--help,-h", "show this help");
    return cmd;
}

/** @return an error message when the combination of options is invalid. */
std::optional<std::string> validateOptions(const Options& options)
{
    if (options.bus != "session" && options.bus != "system" &&
        options.bus != "none")
    {
        return "option '--bus' must be session, system or none, got '" +
               options.bus + "'";
    }
    if (options.certPath.empty() != options.keyPath.empty())
    {
        return "--cert and --key must be given together";
    }
    if (options.tls && options.certPath.empty())
    {
        return "HTTPS needs --cert and --key (or use --no-tls)";
    }
    if (!options.tls && !options.certPath.empty())
    {
        return "--cert/--key cannot be used with --no-tls";
    }
    return std::nullopt;
}

/** @return the options, or an error message to print before the usage. */
std::expected<Options, std::string> readOptions(const CommandLine& cmd)
{
    Options options;
    options.address = cmd.valueOr("--address", options.address);
    options.schemaPath = cmd.valueOr("--schema", options.schemaPath);
    options.bus = cmd.valueOr("--bus", options.bus);
    options.tls = !cmd.isSet("--no-tls");
    options.certPath = cmd.valueOr("--cert", "");
    options.keyPath = cmd.valueOr("--key", "");
    options.redfishUrl = cmd.valueOr("--redfish", options.redfishUrl);
    options.redfishUser = cmd.valueOr("--redfish-user", "");
    options.redfishCa = cmd.valueOr("--redfish-ca", options.redfishCa);
    options.redfishServerName = cmd.valueOr("--redfish-server-name", "");

    auto port = cmd.number<uint16_t>("--port");
    if (!port)
    {
        return std::unexpected(port.error());
    }
    options.port = port->value_or(options.port);

    if (auto problem = validateOptions(options))
    {
        return std::unexpected(*problem);
    }
    return options;
}

/**
 * @brief Parses argv into `options`.
 * @return the process exit code when the program should end now (--help or a
 *         usage error), nullopt to carry on.
 */
std::optional<int> parseCommandLine(int argc, char** argv, Options& options)
{
    CommandLine cmd = makeCommandLine();
    if (auto parsed = cmd.parse(argc, argv); !parsed)
    {
        std::fprintf(stderr, "%s\n%s", parsed.error().c_str(),
                     cmd.usage(argv[0]).c_str());
        return 2;
    }
    if (cmd.isSet("--help"))
    {
        std::fputs(cmd.usage(argv[0]).c_str(), stdout);
        return 0;
    }
    auto parsedOptions = readOptions(cmd);
    if (!parsedOptions)
    {
        std::fprintf(stderr, "%s\n%s", parsedOptions.error().c_str(),
                     cmd.usage(argv[0]).c_str());
        return 2;
    }
    options = *parsedOptions;
    return std::nullopt;
}

std::optional<std::string> readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// ---------------------------------------------------------------------------
// Data sources
// ---------------------------------------------------------------------------

/** @brief Redfish credentials: user from the options, secrets from the env. */
graphql::RedfishCredentials readRedfishCredentials(const Options& options)
{
    graphql::RedfishCredentials credentials;
    if (const char* token = std::getenv("SUTRA_REDFISH_TOKEN"))
    {
        credentials.token = token;
    }
    if (const char* password = std::getenv("SUTRA_REDFISH_PASSWORD"))
    {
        credentials.password = password;
    }
    credentials.username = options.redfishUser;
    return credentials;
}

/** @brief Backend for an https Redfish service, verified against the CA dir. */
std::shared_ptr<graphql::RedfishBackend> makeHttpsRedfishBackend(
    net::io_context& ioc, const Options& options,
    const graphql::RedfishCredentials& credentials)
{
    auto context = std::make_shared<ssl::context>(
        makeClientContextWithCaPath(options.redfishCa));
    ConnectionPoolConfig poolConfig;
    poolConfig.tlsServerName = options.redfishServerName;
    auto pool = HttpsConnectionPool::create(ioc.get_executor(), context.get(),
                                            poolConfig, context);
    return std::make_shared<graphql::RedfishHttpsBackend>(
        pool, options.redfishUrl, credentials);
}

/** @brief Backend for a plain-http Redfish service (warns about secrets). */
std::shared_ptr<graphql::RedfishBackend> makeHttpRedfishBackend(
    net::io_context& ioc, const Options& options,
    const graphql::RedfishCredentials& credentials)
{
    if (!credentials.token.empty() || !credentials.password.empty())
    {
        logWarning("sending Redfish credentials over plain http");
    }
    auto pool = HttpConnectionPool::create(ioc.get_executor());
    return std::make_shared<graphql::RedfishHttpBackend>(
        pool, options.redfishUrl, credentials);
}

/** @brief Registers the Redfish source for `options.redfishUrl`. */
std::expected<void, std::string> addRedfishSource(
    net::io_context& ioc, const Options& options,
    graphql::SourceRegistry& registry)
{
    const auto credentials = readRedfishCredentials(options);
    try
    {
        auto backend = parseUrl(options.redfishUrl).tls
                           ? makeHttpsRedfishBackend(ioc, options, credentials)
                           : makeHttpRedfishBackend(ioc, options, credentials);
        registry.add(graphql::redfishSourceName,
                     std::make_shared<graphql::RedfishSource>(backend));
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string("--redfish: ") + e.what());
    }
    return {};
}

/**
 * @brief Registers the optional D-Bus source for `--bus session|system`.
 *
 * A no-op without D-Bus support or with `--bus none`. The source owns the bus
 * connection, so nothing needs to be kept by the caller.
 */
#ifdef SUTRA_HAVE_DBUS
std::shared_ptr<sdbusplus::asio::connection> addDbusSource(
    net::io_context& ioc, const Options& options,
    graphql::SourceRegistry& registry)
{
    if (options.bus == "none")
    {
        return nullptr;
    }
    try
    {
        auto bus = std::make_shared<sdbusplus::asio::connection>(
            ioc, options.bus == "session" ? sdbusplus::bus::new_user()
                                          : sdbusplus::bus::new_system());
        registry.add(graphql::dbusSourceName,
                     std::make_shared<graphql::DbusSource>(
                         std::make_shared<graphql::SutraDbusBackend>(bus)));
        return bus;
    }
    catch (const std::exception& e)
    {
        logWarning("D-Bus unavailable ({}); dbus fields are disabled",
                   e.what());
    }
    return nullptr;
}
#else
std::nullptr_t addDbusSource([[maybe_unused]] net::io_context& ioc,
                             [[maybe_unused]] const Options& options,
                             [[maybe_unused]] graphql::SourceRegistry& registry)
{
    return nullptr;
}
#endif

// ---------------------------------------------------------------------------
// GraphQL executor and HTTP routes
// ---------------------------------------------------------------------------

/**
 * @brief Binds the schema's fields to the registered sources.
 *
 * Fields whose source is not registered (no D-Bus support or no --bus) are
 * dropped with a warning; a misconfigured source stays a hard error.
 */
std::expected<std::shared_ptr<graphql::Executor>, std::string> makeExecutor(
    graphql::Schema schema, std::shared_ptr<graphql::SourceRegistry> registry,
    net::any_io_executor ioExec)
{
    std::vector<std::string> warnings;
    if (auto applied =
            graphql::applySourceRegistry(schema, *registry, false, warnings);
        !applied)
    {
        return std::unexpected("schema: " + applied.error());
    }
    for (const auto& warning : warnings)
    {
        logWarning("{}", warning);
    }
    return std::make_shared<graphql::Executor>(
        std::move(schema), std::move(registry), std::move(ioExec));
}

void addRoutes(HttpRouter& router,
               const std::shared_ptr<graphql::Executor>& executor)
{
    graphql::registerGraphqlEndpoint(router, executor);
    router.get("/health", [](HttpRequest&, RequestContext&) {
        return jsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    router.get("/schema", [](HttpRequest&, RequestContext&) {
        nlohmann::json schemaDoc = {
            {"queries",
             {{"serviceRoot", "Get Redfish service root"},
              {"systems", "List Redfish systems"},
              {"system", "Get a Redfish system by id"},
              {"chassis", "List Redfish chassis"},
              {"sensors", "List sensors"},
              {"sensor", "Get a sensor by id"}}},
            {"subscriptions",
             {{"systemStatus",
               "Stream live updates for a ComputerSystem (arg: id)"},
              {"chassisStatus", "Stream live updates for a Chassis (arg: id)"},
              {"sensorUpdates",
               "Stream live updates for sensors (arg: chassisId)"}}},
            {"events",
             {{"POST /graphql/events",
               "Inject an event to fire matching subscriptions immediately. "
               "Body: {\"fields\":[\"fieldName\",...]}"}}},
            {"triggerModes",
             {{"event",
               "DBus-driven or externally injected - wakes on signal (default)"},
              {"timer",
               "Poll on fixed interval (use &interval=N, default 5s)"}}}};
        return jsonResponse(http::status::ok, schemaDoc.dump());
    });
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

/** @brief Reads --cert/--key. */
std::expected<PemPair, std::string> readPemPair(const Options& options)
{
    auto cert = readFile(options.certPath);
    auto key = readFile(options.keyPath);
    if (!cert || !key)
    {
        return std::unexpected("cannot read the certificate or key file");
    }
    return PemPair{std::move(*cert), std::move(*key)};
}

/** @return the server TLS context; nullopt when serving plain HTTP. */
std::expected<std::optional<ssl::context>, std::string> loadTlsContext(
    const Options& options)
{
    if (!options.tls)
    {
        return std::optional<ssl::context>();
    }
    auto pair = readPemPair(options);
    if (!pair)
    {
        return std::unexpected(pair.error());
    }
    return std::optional<ssl::context>(
        makeServerContext(pair->certificate, pair->privateKey));
}

// ---------------------------------------------------------------------------
// Serving
// ---------------------------------------------------------------------------

/**
 * @brief Stops the server and the event loop on SIGINT/SIGTERM.
 *
 * Stopping the server alone is not enough for run() to return: the D-Bus
 * connection keeps a wait on the bus socket armed forever. So sessions get a
 * moment to finish, then the loop is stopped explicitly.
 */
class ShutdownHandler
{
  public:
    ShutdownHandler(net::io_context& context, HttpServer& httpServer) :
        ioc(context), server(httpServer), signals(context, SIGINT, SIGTERM),
        grace(context)
    {
        signals.async_wait([this](const error_code& ec, int) {
            if (!ec)
            {
                shutDown();
            }
        });
    }

  private:
    void shutDown()
    {
        logInfo("shutting down");
        server.stop();
        grace.expires_after(std::chrono::seconds(1));
        grace.async_wait([this](const error_code&) { ioc.stop(); });
    }

    net::io_context& ioc;
    HttpServer& server;
    net::signal_set signals;
    net::steady_timer grace;
};

std::unique_ptr<HttpServer> makeServer(
    net::io_context& ioc, const Options& options, HttpRouter& router,
    std::optional<ssl::context>& tlsContext)
{
    tcp::endpoint endpoint(net::ip::make_address(options.address),
                           options.port);
    if (tlsContext)
    {
        return std::make_unique<HttpServer>(ioc.get_executor(), endpoint,
                                            router, *tlsContext);
    }
    return std::make_unique<HttpServer>(ioc.get_executor(), endpoint, router);
}

/** @brief Runs the server until a signal arrives. @return the exit code. */
int serve(net::io_context& ioc, const Options& options, HttpRouter& router,
          std::optional<ssl::context>& tlsContext)
{
    try
    {
        auto server = makeServer(ioc, options, router, tlsContext);
        server->start();
        ShutdownHandler shutdown(ioc, *server);
        logInfo("listening on {}://{}:{}/graphql",
                options.tls ? "https" : "http", options.address,
                server->localEndpoint().port());
        ioc.run();
    }
    catch (const std::exception& e)
    {
        logError("server failed: {}", e.what());
        return 1;
    }
    return 0;
}

/** @brief Builds everything from `options` and serves. @return exit code. */
int run(const Options& options)
{
    auto schema = graphql::Schema::fromFile(options.schemaPath);
    if (!schema)
    {
        logError("{}", schema.error());
        return 1;
    }

    net::io_context ioc;
    auto registry = std::make_shared<graphql::SourceRegistry>();
    if (auto added = addRedfishSource(ioc, options, *registry); !added)
    {
        logError("{}", added.error());
        return 1;
    }
    auto dbusConn = addDbusSource(ioc, options, *registry);

    auto executor =
        makeExecutor(std::move(*schema), registry, ioc.get_executor());
    if (!executor)
    {
        logError("{}", executor.error());
        return 1;
    }

#ifdef SUTRA_HAVE_DBUS
    std::shared_ptr<graphql::QLDbusPropertyWatcher> watcher;
    if (dbusConn)
    {
        watcher = std::make_shared<graphql::QLDbusPropertyWatcher>(
            dbusConn, (*executor)->getDbusWatchers(),
            [exec = *executor](const std::unordered_set<std::string>& fields) {
                exec->notifyFieldsChanged(fields);
            });
    }
#endif

    HttpRouter router;
    addRoutes(router, *executor);

    auto tlsContext = loadTlsContext(options);
    if (!tlsContext)
    {
        logError("{}", tlsContext.error());
        return 1;
    }
    return serve(ioc, options, router, *tlsContext);
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (auto exitCode = parseCommandLine(argc, argv, options))
    {
        return *exitCode;
    }
    return run(options);
}
