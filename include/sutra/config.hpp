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
 * Asio operations are initiated with `useSender`, so an `error_code` turns
 * into the sender error channel as a `boost::system::system_error`, and
 * `operation_aborted` turns into the stopped channel. Inside a sutra `Task`
 * this means: failures throw, cancellation unwinds the task.
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
#include <string>

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

/** @brief Completion token that turns an Asio operation into a sender. */
inline constexpr auto useSender = exec::asio::use_sender;

/**
 * @brief Completion token that keeps the error_code as a value.
 *
 * `auto [ec] = co_await timer.async_wait(useSenderNoThrow);` Use it where a
 * cancelled operation is an expected outcome (for example a timer that is
 * cancelled on purpose) and must not unwind the calling task. Note that with
 * plain `useSender` operation_aborted completes the task as *stopped*.
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
