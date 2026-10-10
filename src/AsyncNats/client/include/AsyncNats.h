/**
 * @file AsyncNats.h
 * @brief Application entry point declaration for the AsyncNats runtime.
 *
 * Include this header to define the application's `co_main`. The full client
 * API is available through `<AsyncNats/client.h>`.
 */
#pragma once

#include <AsyncNats/client.h>

/**
 * @brief Register the process-wide error handler.
 *
 * Call this before the first `co_await` in `co_main`. Kind `closed` does not
 * run the handler. On `ErrorKind::signal`, clients have already been closed.
 */
namespace AsyncNats {
void onError(ErrorHandler handler);
}  // namespace AsyncNats

/**
 * @brief Application entry the library calls from `main`.
 *
 * Define this in the program that links `AsyncNats::AsyncNats`. The returned
 * integer is logged. The process exits 0 unless an exception escapes.
 * Register `AsyncNats::onError` before the first `co_await`.
 */
auto co_main(int argc, char** argv) -> AsyncNats::Main;
