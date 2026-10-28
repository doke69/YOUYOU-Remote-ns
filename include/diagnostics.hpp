#pragma once
#include <exception>
#include <string>
namespace uu {
void diagnostic(const char *event) noexcept;
// Waits until every line accepted before this call has been written or has
// definitively failed/been dropped. Returns false for failure/drop, or when the
// writer is unavailable; true is a write completion, not power-loss durability.
// Waits for the write attempts accepted before this call, including an in-flight
// batch. False reports a prior failure/drop; true does not guarantee power-loss
// durability. Storage waits have no hard deadline.
bool flushDiagnosticsLog() noexcept;
std::string exceptionSummary(std::exception_ptr error);
void diagnosticException(const char *event, std::exception_ptr error) noexcept;
[[noreturn]] void terminateWithDiagnostics() noexcept;
const char *diagnosticPath() noexcept;
std::string connectionFailure(const std::string &stage, const std::string &error);
} // namespace uu
