// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_STATUS_H_
#define DESIGNPP_CORE_STATUS_H_

#include <optional>
#include <string>
#include <utility>

namespace designpp::core {

enum class ErrorCode {
  kOk,
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kPermissionDenied,
  kConflict,
  kUnsupportedSchema,
  kCorruptData,
  kIoError,
  kCancelled,
  kExternalModification,
  kInvalidEncoding,
  kFileTooLarge,
};

struct Status {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  unsigned long native_error = 0;

  [[nodiscard]] bool Ok() const noexcept { return code == ErrorCode::kOk; }
  [[nodiscard]] static Status Success() { return {}; }
};

template <typename T>
class Result final {
 public:
  // Implicit construction keeps error propagation concise at API boundaries.
  Result(T value) : value_(std::move(value)) {}
  Result(Status status) : status_(std::move(status)) {}

  [[nodiscard]] bool Ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] const Status& GetStatus() const noexcept { return status_; }
  [[nodiscard]] const T& Value() const& { return *value_; }
  [[nodiscard]] T&& Value() && { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Status status_{ErrorCode::kIoError, "No value", 0};
};

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_STATUS_H_
