/**
 * @file config.hpp
 * @brief Common includes and namespace aliases for sutra.
 *
 * sutra builds asynchronous network clients and servers from std::execution
 * (stdexec) senders on top of Boost.Asio / Boost.Beast. This header pulls in
 * the pieces every other header needs and defines short aliases.
 *
 * Requirements: stdexec installed with -DSTDEXEC_ENABLE_ASIO=ON
 * -DSTDEXEC_ASIO_IMPLEMENTATION=boost (that is what generates
 * <exec/asio/asio_config.hpp>).
 *
 * Error model
 * -----------
 * Failures are values: every fallible sutra function returns `Expected<T>`
 * (`std::expected<T, error_code>`), and a Task that can fail is a
 * `Task<Expected<T>>`. Nothing is thrown for a failed operation. Asio
 * operations are therefore initiated with `useSenderNoThrow`, which keeps the
 * `error_code` as a value.
 *
 * Cancellation is not an error: a stop request completes the task as
 * *stopped* and unwinds it, as before.
 *
 * Codes that have no OS equivalent (a malformed URL, a bad header, ...) come
 * from the `sutra::Errc` category.
 *
 * Constructors cannot return a value. A precondition violation by the
 * programmer (for example a null pool) throws `std::invalid_argument`;
 * anything that depends on runtime input has a factory returning `Expected`.
 *
 * Scheduling
 * ----------
 * No custom scheduler is needed. Asio completions run on the io_context thread
 * and a task continues inline, so it stays on the io_context. To hop onto an
 * executor explicitly use `onExecutor()`.
 */
#pragma once

// clang-format off
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>
#include <exec/asio/use_sender.hpp>
#include <exec/start_detached.hpp>
#include <exec/task.hpp>
// clang-format on

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <exception>
#include <expected>
#include <string>
#include <type_traits>

namespace sutra
{

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ex = stdexec;

using tcp = net::ip::tcp;
using error_code = boost::system::error_code;
using system_error = boost::system::system_error;

using namespace std::chrono_literals;

/** @brief Value of type `T`, or the `error_code` that prevented it. */
template <class T>
using Expected = std::expected<T, error_code>;

/** @brief Errors that have no operating system code. */
enum class Errc
{
    /** Malformed URL (control characters, user info, bad port, ...). */
    invalidUrl = 1,
    /** URL scheme other than http or https. */
    unsupportedScheme,
    /** URL scheme does not match the client (https needs the TLS client). */
    schemeMismatch,
    /** A request without a host. */
    missingHost,
    /** Request target is empty, does not start with '/', or has bad bytes. */
    invalidTarget,
    /** Header name or value that could corrupt the request. */
    invalidHeader,
    /** Body that is not valid JSON. */
    invalidJson,
    /** An OpenSSL call failed without leaving an error. */
    opensslFailure,
    /** CA path is not a directory. */
    invalidCaPath,
    /** Only one of client certificate and key was given. */
    incompleteClientCredentials,
    /** The server was asked to listen on an unspecified address. */
    unspecifiedAddress,
};

class ErrcCategory : public boost::system::error_category
{
  public:
    const char* name() const noexcept override
    {
        return "sutra";
    }

    std::string message(int value) const override
    {
        switch (static_cast<Errc>(value))
        {
            case Errc::invalidUrl:
                return "malformed url";
            case Errc::unsupportedScheme:
                return "only http and https urls are supported";
            case Errc::schemeMismatch:
                return "url scheme does not match the client";
            case Errc::missingHost:
                return "no host set";
            case Errc::invalidTarget:
                return "bad request target";
            case Errc::invalidHeader:
                return "bad header";
            case Errc::invalidJson:
                return "body is not valid json";
            case Errc::opensslFailure:
                return "openssl failure";
            case Errc::invalidCaPath:
                return "ca path is not a directory";
            case Errc::incompleteClientCredentials:
                return "client certificate and key must be given together";
            case Errc::unspecifiedAddress:
                return "refusing to listen on an unspecified address";
        }
        return "unknown sutra error";
    }
};

inline const boost::system::error_category& errcCategory()
{
    static const ErrcCategory category;
    return category;
}

inline error_code make_error_code(Errc value)
{
    return error_code(static_cast<int>(value), errcCategory());
}

} // namespace sutra

template <>
struct boost::system::is_error_code_enum<sutra::Errc> : std::true_type
{};

namespace sutra
{

/** @brief Completion token that turns an Asio operation into a sender. */
inline constexpr auto useSender = exec::asio::use_sender;

/**
 * @brief Completion token that keeps the error_code as a value.
 *
 * `auto [ec] = co_await timer.async_wait(useSenderNoThrow);` This is the token
 * for every fallible operation. Note that with plain `useSender`
 * operation_aborted completes the task as *stopped*, and any other failure is
 * thrown.
 */
inline const auto useSenderNoThrow = net::as_tuple(exec::asio::use_sender);

/** @brief Coroutine type used throughout sutra. */
template <class T = void>
using Task = exec::task<T>;

using HttpRequest = http::request<http::string_body>;
using HttpResponse = http::response<http::string_body>;

/**
 * @brief Sender that completes on the given Asio executor.
 *
 * `co_await onExecutor(ex)` moves a task onto that executor; it is the
 * io_context equivalent of `schedule(scheduler)`.
 */
template <class Executor>
inline auto onExecutor(const Executor& executor)
{
    return net::post(executor, useSender);
}

/** @brief Extracts the error_code carried by an exception. */
inline error_code errorCodeOf(const std::exception_ptr& eptr) noexcept
{
    try
    {
        std::rethrow_exception(eptr);
    }
    catch (const system_error& e)
    {
        return e.code();
    }
    catch (...)
    {
        return net::error::fault;
    }
}

} // namespace sutra
