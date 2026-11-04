// Bonk Brauzer — error model.
//
// Design notes (why not exceptions):
//   Parsers (HTML, CSS, URL) and the network stack report *recoverable*
//   problems thousands of times per page; they belong in a diagnostic sink,
//   not in an exception. Genuinely fatal, "cannot continue" conditions are
//   rare and are modelled with Result<T>/Status as well, so that every call
//   site is forced to look at the failure. Exceptions stay enabled only
//   because the standard library uses them (bad_alloc, std::string growth);
//   browser code must not throw.
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bonk {

/// An error value. `code` is machine readable and namespaced by module
/// ("net.dns_failure", "html.unexpected_eof", "url.invalid_scheme").
struct Error {
  std::string code;
  std::string message;
  std::string source;  // url / file name, empty when unknown
  int line = 0;        // 1-based, 0 when unknown
  int column = 0;      // 1-based, 0 when unknown

  Error() = default;
  Error(std::string code_in, std::string message_in)
      : code(std::move(code_in)), message(std::move(message_in)) {}

  [[nodiscard]] static Error make(std::string_view code, std::string_view message);
  [[nodiscard]] static Error make(std::string_view code, std::string_view message,
                                  std::string_view source, int line, int column);

  /// Attach a location; returns *this so it can be used inline.
  Error& located_at(std::string_view source, int line, int column);

  /// "net.dns_failure: no A record for example.com (at https://x/:12:3)"
  [[nodiscard]] std::string to_string() const;
};

/// Result of an operation that can only fail (no payload).
class Status {
 public:
  Status() = default;  // success
  explicit Status(Error error) : error_(std::move(error)) {}

  [[nodiscard]] static Status success() { return Status(); }
  [[nodiscard]] static Status failure(Error error) { return Status(std::move(error)); }

  [[nodiscard]] bool is_ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] bool is_error() const noexcept { return error_.has_value(); }
  explicit operator bool() const noexcept { return is_ok(); }

  /// Precondition: is_error().
  [[nodiscard]] const Error& error() const noexcept {
    assert(error_.has_value());
    return *error_;
  }
  /// Consumes the error: afterwards the status is empty (is_ok() is true).
  [[nodiscard]] std::optional<Error> take_error() {
    std::optional<Error> taken = std::move(error_);
    error_.reset();
    return taken;
  }

 private:
  std::optional<Error> error_;
};

/// Result of an operation that yields a value or an error.
template <class T>
class Result {
 public:
  Result(const Result&) = default;
  Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

  [[nodiscard]] static Result success(T value) { return Result(std::in_place_index<0>, std::move(value)); }
  [[nodiscard]] static Result failure(Error error) { return Result(std::in_place_index<1>, std::move(error)); }

  [[nodiscard]] bool is_ok() const noexcept { return value_.index() == 0; }
  [[nodiscard]] bool is_error() const noexcept { return value_.index() == 1; }
  explicit operator bool() const noexcept { return is_ok(); }

  /// Precondition: is_ok().
  [[nodiscard]] T& value() & {
    assert(is_ok());
    return std::get<0>(value_);
  }
  [[nodiscard]] const T& value() const& {
    assert(is_ok());
    return std::get<0>(value_);
  }
  [[nodiscard]] T&& value() && {
    assert(is_ok());
    return std::get<0>(std::move(value_));
  }

  /// Precondition: is_error().
  [[nodiscard]] const Error& error() const& {
    assert(is_error());
    return std::get<1>(value_);
  }

  [[nodiscard]] T value_or(T fallback) const& {
    return is_ok() ? std::get<0>(value_) : std::move(fallback);
  }

  /// Drop the payload; used when a caller only needs success/failure.
  [[nodiscard]] Status status() const& {
    if (is_ok()) return Status::success();
    return Status::failure(std::get<1>(value_));
  }

 private:
  explicit Result(std::in_place_index_t<0> tag, T value) : value_(tag, std::move(value)) {}
  explicit Result(std::in_place_index_t<1> tag, Error error) : value_(tag, std::move(error)) {}

  std::variant<T, Error> value_;
};

}  // namespace bonk
