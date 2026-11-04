// Shared internals between the JS parser and the interpreter.
#pragma once

#include <string_view>

#include "bonk/js/js.hpp"
#include "ast.hpp"

namespace bonk::js {

/// Parses a script into a program node. Errors: "js.syntax".
[[nodiscard]] Result<ast::StmtPtr> parse_script(std::string_view source);

}  // namespace bonk::js
