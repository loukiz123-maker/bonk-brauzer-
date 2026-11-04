#include "bonk/core/result.hpp"

#include <sstream>

namespace bonk {

Error Error::make(std::string_view code, std::string_view message) {
  return Error(std::string(code), std::string(message));
}

Error Error::make(std::string_view code, std::string_view message, std::string_view source, int line, int column) {
  Error error{std::string(code), std::string(message)};
  error.located_at(source, line, column);
  return error;
}

Error& Error::located_at(std::string_view source_in, int line_in, int column_in) {
  source = std::string(source_in);
  line = line_in;
  column = column_in;
  return *this;
}

std::string Error::to_string() const {
  std::string out = code;
  out += ": ";
  out += message;
  if (!source.empty() || line > 0) {
    out += " (at ";
    out += source.empty() ? std::string_view("<input>") : std::string_view(source);
    if (line > 0) {
      out += ':';
      out += std::to_string(line);
      if (column > 0) {
        out += ':';
        out += std::to_string(column);
      }
    }
    out += ')';
  }
  return out;
}

}  // namespace bonk
