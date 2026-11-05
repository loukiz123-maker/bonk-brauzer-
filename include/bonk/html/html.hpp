// Bonk Brauzer — HTML parsing entry point (tokenizer + tree builder).
//
// The tree builder implements the parts of the WHATWG "in body" insertion mode
// that real pages need: implicit <html>/<head>/<body>, implied end tags for
// p/li/dd/dt/option/headings/table cells, void elements, and RCDATA/RAWTEXT
// elements. Deliberately *not* implemented yet (documented, and they are
// reported as diagnostics rather than silently mangled): foster parenting,
// the adoption agency algorithm for misnested formatting elements, and
// templates.
#pragma once

#include <string_view>

#include "bonk/core/diag.hpp"
#include "bonk/core/result.hpp"
#include "bonk/dom/dom.hpp"

namespace bonk::html {

struct ParseOptions {
  bool scripting_enabled = false;    // <noscript> content is parsed as markup when disabled
  bool keep_whitespace_in_head = false;
};

/// Parses `bytes` (UTF-8; the caller decodes charsets in the net layer) into `document`.
/// Never fails on malformed input: problems go to `diagnostics`.
Status parse_html(std::string_view bytes, dom::Document& document, Diagnostics* diagnostics = nullptr,
                  ParseOptions options = {});

/// Fragment parsing (`Element.innerHTML`, `Range.createContextualFragment`): parses
/// `bytes` as the children of `context_tag`. Returns the context element that was
/// created inside `document`; its children are the parsed result. The tokenizer
/// starts in RCDATA/RAWTEXT/PLAINTEXT where the context element demands it.
[[nodiscard]] dom::NodeId parse_fragment(std::string_view bytes, std::string_view context_tag,
                                         dom::Document& document, Diagnostics* diagnostics = nullptr,
                                         ParseOptions options = {});

/// Convenience: build a fresh document from a string (used by tests and the CLI).
[[nodiscard]] Result<dom::Document> parse_document(std::string_view bytes, std::string_view source_url = {},
                                                   Diagnostics* diagnostics = nullptr);

}  // namespace bonk::html
