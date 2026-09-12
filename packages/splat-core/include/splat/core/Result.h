#pragma once

#include <cassert>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace splat {

// Error codes are stable across platforms: Android and iOS map them one to one
// to the `LoadState.error` codes seen from JavaScript.
enum class ErrorCode {
  unsupportedFormat,
  corrupt,
  gpuUnavailable,
  unreadable,  // a file that cannot be opened, is empty, or cannot be mapped
};

struct Error {
  ErrorCode code;
  std::string message;
};

// For operations that succeed with nothing to return.
struct Ok {};

// A value or an error. Exceptions do not cross JNI or Swift boundaries well,
// so the core never throws on bad input.
template <typename T>
class Result {
  static_assert(!std::is_same_v<T, Error>, "Result<Error> is meaningless");
  static_assert(!std::is_same_v<T, bool>,
                "use Result<Ok> or an explicit type: Result<bool> reads as success");

 public:
  Result(T value) : storage_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const { return ok(); }

  // Callers check ok() first; these fail fast in debug rather than throw in release.
  T& value() {
    assert(ok());
    return std::get<T>(storage_);
  }
  const T& value() const {
    assert(ok());
    return std::get<T>(storage_);
  }
  const Error& error() const {
    assert(!ok());
    return std::get<Error>(storage_);
  }

 private:
  std::variant<T, Error> storage_;
};

}  // namespace splat
