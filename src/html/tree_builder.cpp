#include "bonk/html/html.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "bonk/core/strings.hpp"
#include "bonk/core/utf8.hpp"
#include "bonk/html/tokenizer.hpp"

namespace bonk::html {
namespace {

const char* const kVoid[] = {"area", "base", "basefont", "bgsound", "br",   "col",   "embed",
                             "hr",   "img",  "input",    "keygen",  "link", "meta",  "param",
                             "source", "track", "wbr",    "frame"};

// Start tags that close an open <p> (WHATWG "in body", the "close a p element"
// clause). Getting this list right is most of what makes real pages lay out the
// way their authors expect.
const char* const kCloseParagraph[] = {"address", "article", "aside",  "blockquote", "center", "details",
                                       "dialog",  "dir",     "div",    "dl",         "fieldset", "figcaption",
                                       "figure",  "footer",  "form",   "h1",         "h2",       "h3",
                                       "h4",      "h5",      "h6",     "header",     "hgroup",   "hr",
                                       "listing", "main",    "menu",   "nav",        "ol",       "p",
                                       "plaintext", "pre",   "search",  "section", "summary",   "table",    "ul",
                                       "xmp",      "li",     "dd",      "dt"};

// Start tags that leave the "frameset-ok" flag alone. The spec clears the flag for
// characters and for "any other start tag", which is roughly "inline" content: a
// document that only has blocks so far may still be replaced by a <frameset>.
/// Start tags that set the frameset-ok flag to "not ok" ("in body"): the ones whose
/// own rule says so. Ordinary content (`<div>`, `<span>`, `<b>`, …) does *not* touch
/// the flag, which is why `<div><frameset>` still builds a frameset document while
/// `<br><frameset>` does not.
const char* const kClearsFramesetOk[] = {"pre",   "listing", "form",     "li",     "dd",     "dt",
                                         "plaintext", "button", "applet", "marquee", "object",
                                         "table", "area",     "br",       "embed",  "img",    "keygen",
                                         "wbr",   "input",    "hr",       "textarea", "xmp",  "iframe",
                                         "select"};

// Start tags whose "in body" rule does *not* reconstruct the active formatting
// elements before inserting (the spec says so for the block-ish ones, the void
// ones, and everything handled in "in head"). Every other start tag — `span`, an
// unknown element, `img`, `input`, `select`, … — goes through "any other start
// tag", which reconstructs first. That single rule is what makes
// `<div><i></div><option>` grow an `<i>` clone around the option.
const char* const kNoReconstruct[] = {
    "html",     "head",     "body",     "frameset", "frame",     "base",      "basefont",
    "bgsound",  "link",     "meta",     "noframes", "script",    "style",     "template",
    "title",    "noscript", "address",  "article",  "aside",     "blockquote", "center",
    "details",  "dialog",   "dir",      "div",      "dl",        "fieldset",  "figcaption",
    "figure",   "footer",   "header",   "hgroup",   "main",      "menu",      "nav",
    "ol",       "p",        "section",  "search",   "summary",   "ul",        "h1",
    "h2",       "h3",       "h4",       "h5",       "h6",        "pre",       "listing",
    "form",     "li",       "dd",       "dt",       "plaintext", "table",     "hr",
    "param",    "source",   "track",    "textarea", "xmp",       "iframe",    "noembed",
    "rb",       "rtc",      "rp",       "rt"};

// End tags with their own "in body" rule: generate implied end tags, then pop.
// Everything else goes through "any other end tag", which stops at a special
// element instead of climbing out of it.
const char* const kBlockEndTags[] = {"address",  "article", "aside",  "blockquote", "button",   "center",
                                     "details",  "dialog",  "dir",    "div",        "dl",       "fieldset",
                                     "figcaption", "figure", "footer", "header",    "hgroup",   "listing",
                                     "main",     "menu",    "nav",    "ol",         "pre",      "section",
                                     "search",   "summary", "ul"};

// "Formatting" elements: the ones the adoption agency algorithm works on.
const char* const kFormatting[] = {"a",  "b",   "big", "code", "em",    "font", "i",
                                   "nobr", "s", "small", "strike", "strong", "tt", "u"};

// The "special" category, used to find the furthest block in the adoption agency.
const char* const kSpecial[] = {"address", "applet",  "area",   "article", "aside",  "base",     "basefont",
                                "bgsound", "blockquote", "body", "br",    "button", "caption",  "center",
                                "col",     "colgroup", "dd",    "details", "dir",    "div",      "dl",
                                "dt",      "embed",    "fieldset", "figcaption", "figure", "footer", "form",
                                "frame",   "frameset", "h1",    "h2",      "h3",     "h4",       "h5",
                                "h6",      "head",     "header", "hgroup",  "hr",     "html",     "iframe",
                                "img",     "input",    "keygen", "li",     "link",   "listing",  "main",
                                "marquee", "menu",     "meta",   "nav",     "noembed", "noframes", "noscript",
                                "object",  "ol",       "p",      "param",   "plaintext", "pre",   "script",
                                "search",  "section",  "select", "source",  "style",  "summary",  "table",
                                "tbody",   "td",       "template", "textarea", "tfoot", "th",      "thead",
                                "title",   "tr",       "track",  "ul",      "wbr",    "xmp"};

const char* const kHeadElements[] = {"base", "basefont", "bgsound", "link",     "meta",    "noframes",
                                     "title", "style",  "script",  "noscript", "template"};

const char* const kRawText[] = {"script", "style", "xmp", "iframe", "noembed", "noframes"};
const char* const kRcData[] = {"title", "textarea"};

// SVG element names that are not all lowercase: the tree builder adjusts the tag
// name of a start tag before inserting it into an SVG subtree (HTML §13.2.6.5).
struct NamePair { const char* from; const char* to; };
const NamePair kSvgTagNames[] = {
    {"altglyph", "altGlyph"}, {"altglyphdef", "altGlyphDef"}, {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"}, {"animatemotion", "animateMotion"},
    {"animatetransform", "animateTransform"}, {"clippath", "clipPath"}, {"feblend", "feBlend"},
    {"fecolormatrix", "feColorMatrix"}, {"fecomponenttransfer", "feComponentTransfer"},
    {"fecomposite", "feComposite"}, {"feconvolvematrix", "feConvolveMatrix"},
    {"fediffuselighting", "feDiffuseLighting"}, {"fedisplacementmap", "feDisplacementMap"},
    {"fedistantlight", "feDistantLight"}, {"fedropshadow", "feDropShadow"}, {"feflood", "feFlood"},
    {"fefunca", "feFuncA"}, {"fefuncb", "feFuncB"}, {"fefuncg", "feFuncG"}, {"fefuncr", "feFuncR"},
    {"fegaussianblur", "feGaussianBlur"}, {"feimage", "feImage"}, {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"}, {"femorphology", "feMorphology"}, {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"}, {"fespecularlighting", "feSpecularLighting"},
    {"fespotlight", "feSpotLight"}, {"fetile", "feTile"}, {"feturbulence", "feTurbulence"},
    {"foreignobject", "foreignObject"}, {"glyphref", "glyphRef"}, {"lineargradient", "linearGradient"},
    {"radialgradient", "radialGradient"}, {"textpath", "textPath"},  // no "solidcolor": removed from the spec
};

// SVG attribute names that are not all lowercase (same section, "adjust SVG
// attributes"). MathML has exactly one: definitionurl -> definitionURL.
const NamePair kSvgAttributeNames[] = {
    {"attributename", "attributeName"}, {"attributetype", "attributeType"},
    {"basefrequency", "baseFrequency"}, {"baseprofile", "baseProfile"}, {"calcmode", "calcMode"},
    {"clippathunits", "clipPathUnits"}, {"diffuseconstant", "diffuseConstant"},
    {"edgemode", "edgeMode"}, {"filterunits", "filterUnits"}, {"glyphref", "glyphRef"},
    {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"},
    {"keypoints", "keyPoints"}, {"keysplines", "keySplines"}, {"keytimes", "keyTimes"},
    {"lengthadjust", "lengthAdjust"}, {"limitingconeangle", "limitingConeAngle"},
    {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"}, {"markerwidth", "markerWidth"},
    {"maskcontentunits", "maskContentUnits"}, {"maskunits", "maskUnits"}, {"numoctaves", "numOctaves"},
    {"pathlength", "pathLength"}, {"patterncontentunits", "patternContentUnits"},
    {"patterntransform", "patternTransform"}, {"patternunits", "patternUnits"},
    {"pointsatx", "pointsAtX"}, {"pointsaty", "pointsAtY"}, {"pointsatz", "pointsAtZ"},
    {"preservealpha", "preserveAlpha"}, {"preserveaspectratio", "preserveAspectRatio"},
    {"primitiveunits", "primitiveUnits"}, {"refx", "refX"}, {"refy", "refY"},
    {"repeatcount", "repeatCount"}, {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"}, {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"}, {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"}, {"startoffset", "startOffset"}, {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"}, {"surfacescale", "surfaceScale"},
    {"systemlanguage", "systemLanguage"}, {"tablevalues", "tableValues"}, {"targetx", "targetX"},
    {"targety", "targetY"}, {"textlength", "textLength"}, {"viewbox", "viewBox"},
    {"viewtarget", "viewTarget"}, {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"}, {"zoomandpan", "zoomAndPan"},
};

// Namespaced attributes: `xlink:href`, `xml:lang`, `xmlns` and friends. The prefix
// goes into its own slot, the rest becomes the local name — the html5lib outlines
// print them as `xlink href=…`.
struct ForeignAttribute { const char* name; const char* prefix; const char* local; };
const ForeignAttribute kForeignAttributes[] = {
    {"xlink:actuate", "xlink", "actuate"}, {"xlink:arcrole", "xlink", "arcrole"},
    {"xlink:href", "xlink", "href"},       {"xlink:role", "xlink", "role"},
    {"xlink:show", "xlink", "show"},       {"xlink:title", "xlink", "title"},
    {"xlink:type", "xlink", "type"},       {"xml:lang", "xml", "lang"},
    {"xml:space", "xml", "space"},         {"xmlns", "xmlns", "xmlns"},
    {"xmlns:xlink", "xmlns", "xlink"},
};

// Start tags that end foreign content: they are HTML elements by definition, so the
// parser pops back to HTML and reprocesses them there (`<svg><b>` must not nest a
// `b` inside the `svg`). `<font>` is special: only with color/face/size (SVG's own
// `<font>` element is legitimate).
const char* const kForeignBreakoutTags[] = {
    "b",    "big", "blockquote", "body", "br",   "center", "code",   "dd",   "div",   "dl",
    "dt",   "em",  "embed",      "h1",   "h2",   "h3",     "h4",     "h5",   "h6",    "head",
    "hr",   "i",   "img",        "li",   "listing", "menu", "meta",   "nobr", "ol",   "p",
    "pre",  "ruby", "s",         "small", "span", "strong", "strike", "sub",  "sup",  "table",
    "tt",   "u",   "ul",         "var"};

[[nodiscard]] bool is_ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

[[nodiscard]] bool equals_ascii_ci(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
    if (x != y) return false;
  }
  return true;
}

/// Doctype-based quirks detection (HTML §13.2.6.4.1). The lists are the spec's; the
/// two "limited quirks" prefixes only matter for layout, so they are kept apart
/// rather than folded into `Quirks`.
enum class DoctypeMode { Standards, LimitedQuirks, Quirks };

const char* const kQuirkyPublicPrefixes[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//", "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//", "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//", "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//", "-//IETF//DTD HTML 2.0 Strict//", "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//", "-//IETF//DTD HTML 3.0//", "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//", "-//IETF//DTD HTML 3//", "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//", "-//IETF//DTD HTML Level 2//", "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//", "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//", "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//", "-//IETF//DTD HTML//", "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//", "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//", "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//", "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//", "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//", "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//", "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//", "-//W3C//DTD HTML 3.2 Final//", "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//", "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//", "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//", "-//W3C//DTD W3 HTML//", "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//", "-//WebTechs//DTD Mozilla HTML//",
};

const char* const kLimitedQuirksPublicPrefixes[] = {
    "-//W3C//DTD XHTML 1.0 Frameset//", "-//W3C//DTD XHTML 1.0 Transitional//",
};

[[nodiscard]] bool starts_with_ascii_ci(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  return equals_ascii_ci(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool matches_any_prefix(std::string_view text, const char* const* list, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (starts_with_ascii_ci(text, list[i])) return true;
  }
  return false;
}

template <std::size_t N>
[[nodiscard]] bool matches_any_prefix(std::string_view text, const char* const (&list)[N]) {
  return matches_any_prefix(text, list, N);
}

[[nodiscard]] DoctypeMode doctype_mode(const Token& token) {
  if (token.force_quirks) return DoctypeMode::Quirks;
  if (!equals_ascii_ci(token.data, "html")) return DoctypeMode::Quirks;

  const std::string_view public_id = token.public_identifier;
  const std::string_view system_id = token.system_identifier;
  const bool has_public = token.has_public_identifier;
  const bool system_missing_or_empty = !token.has_system_identifier || system_id.empty();

  if (has_public && (equals_ascii_ci(public_id, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
                     equals_ascii_ci(public_id, "-/W3C/DTD HTML 4.0 Transitional/EN") ||
                     equals_ascii_ci(public_id, "HTML"))) {
    return DoctypeMode::Quirks;
  }
  if (!system_missing_or_empty && equals_ascii_ci(system_id, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")) {
    return DoctypeMode::Quirks;
  }
  if (has_public && matches_any_prefix(public_id, kQuirkyPublicPrefixes)) return DoctypeMode::Quirks;
  // HTML 4.01 Frameset/Transitional without a system identifier is *fully* quirky.
  if (has_public && system_missing_or_empty &&
      (starts_with_ascii_ci(public_id, "-//W3C//DTD HTML 4.01 Frameset//") ||
       starts_with_ascii_ci(public_id, "-//W3C//DTD HTML 4.01 Transitional//"))) {
    return DoctypeMode::Quirks;
  }
  if (has_public && matches_any_prefix(public_id, kLimitedQuirksPublicPrefixes)) return DoctypeMode::LimitedQuirks;
  if (has_public && !system_missing_or_empty &&
      (starts_with_ascii_ci(public_id, "-//W3C//DTD HTML 4.01 Frameset//") ||
       starts_with_ascii_ci(public_id, "-//W3C//DTD HTML 4.01 Transitional//"))) {
    return DoctypeMode::LimitedQuirks;
  }
  return DoctypeMode::Standards;
}

/// `<input type=hidden>` never clears the frameset-ok flag and is allowed inside a
/// table; the value is matched case-insensitively ("hidDEN" counts).
[[nodiscard]] bool is_hidden_input(const Token& token) {
  return equals_ascii_ci(std::string(token.attribute("type")), "hidden");
}

template <std::size_t N>
bool contains(const char* const (&list)[N], std::string_view value) noexcept {
  for (const char* item : list) {
    if (value == item) return true;
  }
  return false;
}

class TreeBuilder {
 public:
  TreeBuilder(dom::Document& document, Diagnostics* diagnostics, ParseOptions options)
      : document_(document), diagnostics_(diagnostics), options_(options) {}

  /// Fragment parsing: the context element whose children were built.
  [[nodiscard]] dom::NodeId fragment_context_element() const { return context_; }
  [[nodiscard]] dom::NodeId html_element() const { return html_; }
  /// `context` is either a bare tag name (HTML) or `svg tag` / `math tag`, the way
  /// the html5lib corpus spells a foreign fragment context.
  void set_fragment_context(std::string_view context) {
    if (context.size() > 4 && context.substr(0, 4) == "svg ") {
      fragment_namespace_ = dom::Namespace::Svg;
      context.remove_prefix(4);
    } else if (context.size() > 5 && context.substr(0, 5) == "math ") {
      fragment_namespace_ = dom::Namespace::Mathml;
      context.remove_prefix(5);
    }
    fragment_tag_ = context;
  }

  void run(std::string_view input) {
    tokenizer_ = std::make_unique<Tokenizer>(input, diagnostics_);
    if (!fragment_tag_.empty()) begin_fragment(fragment_tag_);
    for (;;) {
      // The tokenizer asks about the adjusted current node for CDATA sections.
      tokenizer_->set_foreign_content(current_namespace() != dom::Namespace::Html);
      Token token = tokenizer_->next();
      if (token.type == TokenType::EndOfFile) break;
      process(token);
      // Only an HTML element switches the tokenizer to a text state: an SVG
      // <title>, <script> or <style> is a plain foreign element whose content stays
      // markup.
      if (token.type == TokenType::StartTag && current_namespace() == dom::Namespace::Html) {
        if (contains(kRawText, token.name)) tokenizer_->enter_text_element(token.name, false);
        if (contains(kRcData, token.name)) tokenizer_->enter_text_element(token.name, true);
        if (token.name == "noscript" && options_.scripting_enabled) {
          tokenizer_->enter_text_element(token.name, false);  // "<noscript> with scripting" is RAWTEXT
        }
      }
    }
    finish();
  }

 private:
  /// Insertion modes. Not a closed set of parser states: the mode is derived from
  /// the stack of open elements (see `mode()`), which keeps nested tables honest.
  enum class Mode { Body, Table, TableBody, Row, Cell, ColumnGroup, Template };

  // --- helpers -----------------------------------------------------------
  [[nodiscard]] dom::NodeId current() const {
    // Fragment parsing: with the stack down to the fake <html> root, new children
    // belong to the context element (which is deliberately not on the stack).
    if (fragment_mode_ && stack_.size() <= 1) return context_;
    if (stack_.empty()) return document_.root();
    return stack_.back();
  }

  /// `<template>` keeps its children in a separate DocumentFragment ("template
  /// contents"), so the "appropriate place for inserting a node" is that fragment
  /// rather than the element itself.
  [[nodiscard]] dom::NodeId template_content(dom::NodeId element) const {
    for (const dom::NodeId child : document_.children(element)) {
      if (document_.type(child) == dom::NodeType::Fragment) return child;
    }
    return dom::NodeId{};
  }

  [[nodiscard]] dom::NodeId insertion_parent() const {
    const dom::NodeId top = current();
    if (tag_of(top) == "template") {
      const dom::NodeId content = template_content(top);
      if (content.valid()) return content;
    }
    return top;
  }

  /// An HTML element named `tag`. Scope walks and "close the nearest <p>" style
  /// searches must not match a foreign element that merely shares a name: `<svg td>`
  /// is not a table cell and `<svg template>` is not a template, so `</td>` and
  /// `</template>` walk straight past them.
  [[nodiscard]] bool is_html_named(dom::NodeId node, std::string_view tag) const {
    return document_.type(node) == dom::NodeType::Element &&
           document_.element_namespace(node) == dom::Namespace::Html && document_.tag_name(node) == tag;
  }

  [[nodiscard]] bool has_open(std::string_view tag) const {
    for (const dom::NodeId node : stack_) {
      if (is_html_named(node, tag)) return true;
    }
    return false;
  }

  [[nodiscard]] bool current_is(std::string_view tag) const {
    return !stack_.empty() && is_html_named(stack_.back(), tag);
  }

  /// True when the "in select" insertion mode applies: an open <select> with no
  /// table/td/th/caption/template boundary above it (a simplified select scope).
  [[nodiscard]] bool select_context() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "select") return true;
      if (tag == "table" || tag == "td" || tag == "th" || tag == "caption" || tag == "template") return false;
    }
    return false;
  }

  /// The *strict* "has a select in select scope" test: only optgroup/option are
  /// transparent, so any other element above the select (a <button>, a <div>)
  /// means an end tag for the select is a parse error and gets dropped.
  [[nodiscard]] bool select_in_select_scope() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "select") return true;
      if (tag != "optgroup" && tag != "option") return false;
    }
    return false;
  }

  [[nodiscard]] bool body_created() const { return fragment_mode_ || body_.valid(); }
  [[nodiscard]] bool head_created() const { return fragment_mode_ || head_.valid(); }

  /// The fragment algorithm (HTML §13.4), specialised for this builder: the fake
  /// root is an <html> element that holds nothing, the context element lives
  /// outside the stack, and `current()` hands it back while the stack is down to
  /// that root. That is why no rule can ever pop the context element — matching
  /// the spec, where the context element is not on the stack of open elements at
  /// all and the result is "the children of the root".
  void begin_fragment(std::string_view context_tag) {
    fragment_mode_ = true;
    ensure_html();
    const dom::Namespace context_namespace =
        fragment_namespace_ != dom::Namespace::Html
            ? fragment_namespace_
            : (context_tag == "svg" ? dom::Namespace::Svg
               : context_tag == "math" ? dom::Namespace::Mathml
                                       : dom::Namespace::Html);
    context_ = document_.create_element(context_tag, context_namespace);
    document_.append_child(document_.root(), context_);
    fragment_initial_mode_ = initial_mode_for(context_tag);
    // RCDATA/RAWTEXT/PLAINTEXT, but with no appropriate end tag: the context tag
    // was never a start tag we emitted, so `</script>` in the fragment is text.
    // Only HTML contexts get a text state — an SVG `<title>` is an ordinary foreign
    // element, and markup inside it stays markup.
    if (context_namespace != dom::Namespace::Html) return;
    if (context_tag == "title" || context_tag == "textarea") {
      tokenizer_->enter_context_text_state(true);
    } else if (context_tag == "style" || context_tag == "xmp" || context_tag == "iframe" ||
               context_tag == "noembed" || context_tag == "noframes" || context_tag == "script" ||
               (context_tag == "noscript" && options_.scripting_enabled)) {
      tokenizer_->enter_context_text_state(false);
    } else if (context_tag == "plaintext") {
      tokenizer_->enter_plain_text();
    }
  }

  /// "Reset the insertion mode appropriately", fragment case: the context element
  /// is the last node in the (one element) stack, so every rule that requires
  /// `last` to be false is skipped. That is what makes a `td` context behave like
  /// "in body" (no `<tr>`/`<td>` repairing) while `tr`/`tbody`/`colgroup`/`table`
  /// keep their own modes.
  [[nodiscard]] static Mode initial_mode_for(std::string_view tag) {
    if (tag == "tr") return Mode::Row;
    if (tag == "tbody" || tag == "thead" || tag == "tfoot") return Mode::TableBody;
    if (tag == "colgroup") return Mode::ColumnGroup;
    if (tag == "table") return Mode::Table;
    return Mode::Body;  // td/th/caption/select/head/body/… and everything else
  }


  void ensure_html() {
    if (html_.valid()) return;
    html_ = document_.create_element("html");
    document_.append_child(document_.root(), html_);
    stack_.push_back(html_);
  }

  void ensure_head() {
    ensure_html();
    if (head_.valid()) return;
    head_ = document_.create_element("head");
    document_.append_child(html_, head_);
    stack_.push_back(head_);
  }

  void ensure_body() {
    ensure_html();
    if (body_.valid()) return;
    ensure_head();  // every document has a <head>, even when the author omitted it
    close_head();
    body_ = document_.create_element("body");
    document_.append_child(html_, body_);
    stack_.clear();
    stack_.push_back(html_);
    stack_.push_back(body_);
  }

  void close_head() {
    while (!stack_.empty() && stack_.back() != html_) {
      if (stack_.back() == head_) {
        stack_.pop_back();
        return;
      }
      stack_.pop_back();
    }
  }

  /// Pops the stack down to (and including) the nearest `tag`; no-op when absent.
  bool close_open(std::string_view tag) {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (is_html_named(stack_[i - 1], tag)) {
        stack_.resize(i - 1);
        return true;
      }
    }
    return false;
  }

  /// Indices are stack positions; "in scope" means "above the nearest scope
  /// boundary", which is what stops `<button><p><div>` from closing the outer <p>.
  [[nodiscard]] bool p_in_scope() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = document_.tag_name(stack_[i - 1]);
      if (is_html_named(stack_[i - 1], "p")) return true;
      if (tag == "button" || tag == "table" || tag == "td" || tag == "th" || tag == "caption" ||
          tag == "marquee" || tag == "object" || tag == "applet" || tag == "template" || tag == "html" ||
          is_scope_boundary(stack_[i - 1])) {
        return false;
      }
    }
    return false;
  }

  /// "Generate implied end tags, optionally ignoring `except`" (HTML §13.2.6.4.8):
  /// pops the phrasing elements that never need an explicit end tag. `rb`/`rt`/
  /// `rtc`/`rp` are in the list, which is what makes `<ruby>a<rb>b<rb>c` produce
  /// sibling annotations instead of a nesting ladder.
  void generate_implied_end_tags(std::string_view except = {}) {
    while (!stack_.empty()) {
      if (document_.element_namespace(stack_.back()) != dom::Namespace::Html) break;  // never a foreign element
      const std::string_view top = tag_of(stack_.back());
      if (top == except) break;
      if (top != "dd" && top != "dt" && top != "li" && top != "optgroup" && top != "option" && top != "p" &&
          top != "rb" && top != "rp" && top != "rt" && top != "rtc") {
        break;
      }
      stack_.pop_back();
    }
  }

  [[nodiscard]] static bool is_heading(std::string_view tag) {
    return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
  }

  /// "Close a p element" / heading / button scope juggling for start tags.
  void implied_end_tags(std::string_view next) {
    // A block start tag closes the nearest open <p>, even when other phrasing
    // elements are open in between (`<p><b>x<div>`), but never across a scope
    // boundary such as <button>.
    // `<table>` is the one block in the list that does *not* close a <p> in quirks
    // mode (`<p><table></table>` keeps the table inside the paragraph).
    const bool closes_p = contains(kCloseParagraph, next) && !(next == "table" && quirks_);
    if (closes_p && p_in_scope()) close_open("p");
    if (next == "button" && tag_in_scope("button")) {
      generate_implied_end_tags();
      pop_through("button");
      report("html.nested_button", "<button> inside <button> closes the outer one");
    }

    if (next == "li") {
      // Only inside the same list: `<ul><li>a<ul><li>b` must nest, not flatten.
      if (tag_in_scope("li", true)) close_open("li");
    } else if (next == "dd" || next == "dt") {
      if (tag_in_scope("dd", true)) close_open("dd");
      if (tag_in_scope("dt", true)) close_open("dt");
    } else if (next == "option") {
      // Only an option that is the current node is closed: `<option><span><option>`
      // nests the second option inside the span.
      if (current_is("option")) stack_.pop_back();
    } else if (next == "optgroup") {
      close_open("option");
      close_open("optgroup");
    } else if (next == "tr") {
      close_open("td");
      close_open("th");
      close_open("tr");
    } else if (next == "td" || next == "th") {
      close_open("td");
      close_open("th");
    } else if (next == "rb" || next == "rtc") {
      generate_implied_end_tags();
    } else if (next == "rt" || next == "rp") {
      generate_implied_end_tags("rtc");  // an <rtc> stays open around its <rt>s
    } else if (is_heading(next)) {
      // `<h1>` closes only a heading that is the current node, not one buried
      // under a <div>: the spec pops exactly one.
      if (is_heading(tag_of(current()))) stack_.pop_back();
    }
  }

  [[nodiscard]] dom::NodeId nearest_table() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (document_.tag_name(stack_[i - 1]) == "table") return stack_[i - 1];
    }
    return dom::NodeId{};
  }

  /// "In select in table": a select opened while a table is in table scope. The
  /// select itself is fostered out of the table, so the table sits *below* it on the
  /// stack — that is the condition, not a table part above it.
  [[nodiscard]] bool select_in_table() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (tag_of(stack_[i - 1]) == "select") return in_table_scope("table");
      if (is_html_named(stack_[i - 1], "template")) return false;
    }
    return false;
  }

  /// "In head noscript" (scripting disabled): a <noscript> that is the current node
  /// inside <head> keeps only a few tags, everything else is a parse error.
  [[nodiscard]] bool in_head_noscript() const {
    return !fragment_mode_ && !options_.scripting_enabled && !body_created() && head_.valid() &&
           current_is("noscript") && document_.parent(current()) == head_;
  }

  [[nodiscard]] bool in_frameset() const {
    return has_open("frameset") || (fragment_mode_ && tag_of(context_) == "frameset");
  }

  /// True when the document's content root is a <frameset> instead of a <body>.
  [[nodiscard]] bool frameset_document() const { return body_.valid() && tag_of(body_) == "frameset"; }

  /// Replaces the (still empty) <body> with a <frameset>, per the "in body" rule
  /// for a frameset start tag.
  void enter_frameset(const Token& token) {
    const dom::NodeId frameset = make_element(token);
    if (body_.valid() && body_ != head_) document_.unlink(body_);
    document_.append_child(html_, frameset);
    stack_.clear();
    stack_.push_back(html_);
    stack_.push_back(frameset);
    body_ = frameset;
  }

  /// True when a `<template>` sits above the innermost table in the stack: the
  /// "in template" rules then own the content, so the implicit tbody/tr/colgroup
  /// fix must not run (a `<td>` inside `<template>` belongs to the template).
  [[nodiscard]] bool template_above_table() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "template") return true;
      if (tag == "table") return false;
    }
    return false;
  }

  /// True when text or a stray element here must be foster-parented (i.e. moved
  /// before the enclosing <table>), per the "in table" insertion mode.
  [[nodiscard]] bool table_context() const {
    if (stack_.empty()) return false;
    const std::string_view top = document_.tag_name(stack_.back());
    return top == "table" || top == "tbody" || top == "thead" || top == "tfoot" || top == "tr";
  }

  /// Foster parenting only relocates content whose parent would otherwise be a
  /// table structure element — `<table><div>` fosters the div, but the div's own
  /// children then land inside the div, not before the table.
  [[nodiscard]] bool foster_target() const {
    const std::string_view top = tag_of(current());
    return top == "table" || top == "tbody" || top == "tfoot" || top == "thead" || top == "tr";
  }

  /// "Appropriate place for inserting a node", with foster parenting for tables.
  void insert_node(dom::NodeId node) {
    if (foster_target()) {
      foster_insert(node);
      return;
    }
    document_.append_child(insertion_parent(), node);
  }

  /// Inserts `node` into `parent`, but fosters it out when `parent` is a table part
  /// (the adoption agency needs this: the common ancestor may be a <table>).
  void insert_at(dom::NodeId parent, dom::NodeId node) {
    const std::string_view tag = tag_of(parent);
    if (tag == "table" || tag == "tbody" || tag == "tfoot" || tag == "thead" || tag == "tr") {
      foster_insert(node);
      return;
    }
    if (tag == "template") {  // the children of a template live in its contents
      const dom::NodeId content = template_content(parent);
      if (content.valid()) {
        document_.append_child(content, node);
        return;
      }
    }
    document_.append_child(parent, node);
  }

  void insert_text(std::string_view text) {
    if (foster_target()) {
      foster_text(text);
      return;
    }
    document_.append_text(insertion_parent(), text);
  }

  /// Inserts a node just before the innermost open table.
  void foster_insert(dom::NodeId child) {
    const dom::NodeId table = nearest_table();
    if (!table.valid()) {
      // A table mode can be reached through a <template> only (`<template><tr><div>`
      // or `<template><tbody><select>`); there is no table to foster relative to,
      // so the node simply joins the template's contents after what is already
      // there — html5lib's corpus has no table-less foster parent at all.
      for (std::size_t i = stack_.size(); i > 0; --i) {
        if (is_html_named(stack_[i - 1], "template")) {
          const dom::NodeId content = template_content(stack_[i - 1]);
          document_.append_child(content.valid() ? content : stack_[i - 1], child);
          return;
        }
      }
      document_.append_child(current(), child);
      return;
    }
    const dom::NodeId parent = document_.parent(table);
    if (!parent.valid()) {
      document_.append_child(current(), child);
      return;
    }
    document_.insert_before(parent, child, table);
  }

  /// Foster-parented text joins the Text node that already sits right before the
  /// table, so `<table>A<td>B</td>C</table>` ends up as a single "AC" node.
  void foster_text(std::string_view text) {
    const dom::NodeId table = nearest_table();
    const dom::NodeId parent = table.valid() ? document_.parent(table) : current();
    if (!parent.valid()) {
      document_.append_text(current(), text);
      return;
    }
    const dom::NodeId previous = previous_sibling_of(table);
    if (previous.valid() && document_.type(previous) == dom::NodeType::Text) {
      document_.extend_text(previous, text);
      return;
    }
    document_.insert_before(parent, document_.create_text(text), table);
  }

  [[nodiscard]] dom::NodeId previous_sibling_of(dom::NodeId node) const {
    if (!node.valid()) return dom::NodeId{};
    const dom::NodeId parent = document_.parent(node);
    if (!parent.valid()) return dom::NodeId{};
    dom::NodeId previous;
    for (const dom::NodeId child : document_.children(parent)) {
      if (child == node) return previous;
      previous = child;
    }
    return dom::NodeId{};
  }

  // --- active formatting elements ----------------------------------------
  //
  // Without this list a document like `<p><b>one<p>two` cannot be built the way
  // the spec says: <b> stays "active" after its paragraph closes and is cloned
  // into the next one. The same list is what the adoption agency algorithm
  // rewrites when an end tag arrives for an element that is not the current node.
  [[nodiscard]] static bool is_formatting(std::string_view tag) { return contains(kFormatting, tag); }
  [[nodiscard]] static bool is_special(std::string_view tag) { return contains(kSpecial, tag); }

  /// "Special" elements are the boundaries of the adoption agency algorithm. Only a
  /// few foreign elements qualify — `<svg tr>` is *not* one, which is what keeps
  /// `<a><svg><tr></a>` from being rebuilt around a table row.
  [[nodiscard]] bool is_special_node(dom::NodeId node) const {
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return false;
    const std::string_view tag = tag_of(node);
    switch (document_.element_namespace(node)) {
      case dom::Namespace::Html:
        return is_special(tag);
      case dom::Namespace::Mathml:
        return tag == "mi" || tag == "mo" || tag == "mn" || tag == "ms" || tag == "mtext" ||
               tag == "annotation-xml";
      case dom::Namespace::Svg:
        return tag == "foreignObject" || tag == "desc" || tag == "title";
    }
    return false;
  }

  [[nodiscard]] int index_in_stack(dom::NodeId node) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (stack_[i - 1] == node) return static_cast<int>(i - 1);
    }
    return -1;
  }

  [[nodiscard]] int index_in_afe(dom::NodeId node) const {
    for (std::size_t i = afe_.size(); i > 0; --i) {
      if (afe_[i - 1] == node) return static_cast<int>(i - 1);
    }
    return -1;
  }

  /// Attribute-list equality for the "Noah's Ark" clause. `attributes()` hands
  /// out a span over one shared staging buffer, so the first list is copied out
  /// before the second one is queried (otherwise both spans alias the buffer and
  /// every comparison trivially succeeds).
  [[nodiscard]] bool same_attributes(dom::NodeId a, dom::NodeId b) const {
    const std::span<const dom::Attribute> span = document_.attributes(a);
    const std::vector<dom::Attribute> left(span.begin(), span.end());
    const std::span<const dom::Attribute> right = document_.attributes(b);
    if (left.size() != right.size()) return false;
    for (const dom::Attribute& attribute : left) {
      if (document_.attribute(b, attribute.name) != attribute.value) return false;
    }
    return true;
  }

  void push_formatting(dom::NodeId node) {
    // "Noah's Ark clause": at most three entries with the same name and attributes
    // after the last marker; the earliest of them is dropped.
    std::size_t same = 0;
    int earliest = -1;
    for (std::size_t i = afe_.size(); i > 0; --i) {
      const dom::NodeId entry = afe_[i - 1];
      if (!entry.valid()) break;  // marker
      if (document_.tag_name(entry) != document_.tag_name(node)) continue;
      if (!same_attributes(entry, node)) continue;
      ++same;
      earliest = static_cast<int>(i - 1);
    }
    if (same >= 3 && earliest >= 0) afe_.erase(afe_.begin() + earliest);
    afe_.push_back(node);
  }

  void push_marker() { afe_.push_back(dom::NodeId{}); }

  void clear_to_last_marker() {
    while (!afe_.empty()) {
      if (!afe_.back().valid()) {
        afe_.pop_back();
        return;
      }
      afe_.pop_back();
    }
  }

  /// Clones an element (same name, same attributes) without its children.
  [[nodiscard]] dom::NodeId clone_element(dom::NodeId node) const {
    const dom::NodeId copy = document_.create_element(document_.tag_name(node));
    for (const dom::Attribute& attribute : document_.attributes(node)) {
      document_.set_attribute(copy, attribute.name, attribute.value);
    }
    return copy;
  }

  /// Re-opens the formatting elements that were closed by an implicit end tag.
  void reconstruct_formatting() {
    if (afe_.empty()) return;
    if (!afe_.back().valid()) return;                 // marker on top
    if (index_in_stack(afe_.back()) >= 0) return;      // already open
    std::size_t index = afe_.size();
    while (index > 0) {
      const dom::NodeId entry = afe_[index - 1];
      if (!entry.valid() || index_in_stack(entry) >= 0) break;
      --index;
    }
    for (; index < afe_.size(); ++index) {
      const dom::NodeId clone = clone_element(afe_[index]);
      insert_node(clone);
      stack_.push_back(clone);
      afe_[index] = clone;
    }
  }

  /// Moves-ready detach: unlink from the parent but keep the node alive.
  void detach(dom::NodeId node) { document_.unlink(node); }

  void remove_from_stack(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < stack_.size()) {
      stack_.erase(stack_.begin() + index);
    }
  }

  void remove_from_afe(dom::NodeId node) {
    const int index = index_in_afe(node);
    if (index >= 0) afe_.erase(afe_.begin() + index);
  }

  /// Node name, or "" when the id is dead (detached nodes linger in the
  /// active-formatting list until the algorithm cleans them up).
  [[nodiscard]] std::string_view tag_of(dom::NodeId node) const {
    return node.valid() && document_.is_alive(node) ? document_.tag_name(node) : std::string_view{};
  }

  /// WHATWG "adoption agency algorithm" (HTML §13.2.6.4.7).
  ///
  /// The whole point of the algorithm: an end tag for a formatting element that
  /// is *not* the current node must not simply be ignored or pop everything above
  /// it. `1<b>2<i>3</b>4</i>` builds `<b>2<i>3</i></b><i>4</i>`-shaped trees by
  /// cloning the still-active formatting elements into a new home. This is the
  /// single most visible tree-construction rule and the reason a naive builder
  /// produces the "<b> swallows the rest of the page" bug.
  void adoption_agency(std::string_view subject) {
    // 1. A stray </b> when <b> is the current node and not active: just pop it.
    if (!stack_.empty() && tag_of(stack_.back()) == subject && index_in_afe(stack_.back()) < 0) {
      stack_.pop_back();
      return;
    }
    for (int outer = 0; outer < 8; ++outer) {
      // 2. The formatting element: the last matching entry after the last marker.
      int formatting_index = -1;
      for (std::size_t i = afe_.size(); i > 0; --i) {
        const dom::NodeId entry = afe_[i - 1];
        if (!entry.valid()) break;  // marker
        if (tag_of(entry) == subject) {
          formatting_index = static_cast<int>(i - 1);
          break;
        }
      }
      if (formatting_index < 0) {  // 3. no such element: "any other end tag"
        generic_end_tag(subject);
        return;
      }
      const dom::NodeId formatting = afe_[static_cast<std::size_t>(formatting_index)];
      const int formatting_stack_index = index_in_stack(formatting);
      if (formatting_stack_index < 0) {  // 4. not open: forget it
        afe_.erase(afe_.begin() + formatting_index);
        return;
      }
      if (!in_scope(formatting)) return;  // 5. not in scope: ignore the tag

      // 6. The furthest block: the lowest "special" element above the formatting
      //    element (the boundary the misnested content has to be moved into).
      int furthest_index = -1;
      for (std::size_t i = static_cast<std::size_t>(formatting_stack_index) + 1; i < stack_.size(); ++i) {
        if (is_special_node(stack_[i])) {
          furthest_index = static_cast<int>(i);
          break;
        }
      }
      if (furthest_index < 0) {  // 7. no furthest block: just pop through it
        stack_.resize(static_cast<std::size_t>(formatting_stack_index));
        afe_.erase(afe_.begin() + formatting_index);
        return;
      }
      const dom::NodeId furthest_block = stack_[static_cast<std::size_t>(furthest_index)];
      // The node below the formatting element. At the bottom of the stack that is
      // the fake <html> root — or, for a fragment, the context element the built
      // nodes really end up in.
      dom::NodeId common_ancestor = document_.root();
      if (formatting_stack_index > 1) {
        common_ancestor = stack_[static_cast<std::size_t>(formatting_stack_index) - 1];
      } else if (fragment_mode_) {
        common_ancestor = context_;
      } else if (formatting_stack_index > 0) {
        common_ancestor = stack_[0];
      }

      // 8.-15. Walk down from the furthest block towards the formatting element,
      //        cloning every active formatting element on the way and re-parenting
      //        the nodes we already handled under those clones.
      int bookmark = formatting_index;
      dom::NodeId node = furthest_block;
      dom::NodeId last_node = furthest_block;
      int node_index = furthest_index;
      int inner = 0;
      for (;;) {
        ++inner;
        const int above = node_index - 1;
        if (above < 0) break;  // nothing above: give up
        node_index = above;
        node = stack_[static_cast<std::size_t>(node_index)];
        if (node == formatting) break;  // 13.4: reached it
        // 13.5: from the fourth step on the node is too far from the formatting
        // element to be worth re-parenting: drop it from the stack and keep
        // walking up instead of cloning it.
        if (inner > 3) {
          stack_.erase(stack_.begin() + node_index);
          continue;
        }
        const int node_afe_index = index_in_afe(node);
        if (node_afe_index < 0) {  // 13.5: not active, drop and keep walking up
          stack_.erase(stack_.begin() + node_index);
          continue;
        }
        // 13.6-13.10: clone, then push everything we already collected into the clone.
        const dom::NodeId clone = clone_element(node);
        afe_[static_cast<std::size_t>(node_afe_index)] = clone;
        stack_[static_cast<std::size_t>(node_index)] = clone;
        if (last_node == furthest_block) bookmark = node_afe_index + 1;
        detach(last_node);
        document_.append_child(clone, last_node);
        last_node = clone;
        node = clone;
      }

      // 16. Place the last node under the common ancestor (not under the furthest
      //     block: that is the whole trick that keeps the tree sane). When the
      //     common ancestor is a table part the node is foster-parented instead.
      detach(last_node);
      insert_at(common_ancestor, last_node);

      // 17.-18. A fresh copy of the formatting element adopts the furthest block's
      //         former children and is appended to the furthest block.
      const dom::NodeId replacement = clone_element(formatting);
      for (const dom::NodeId child : document_.children(furthest_block)) {
        document_.unlink(child);
        document_.append_child(replacement, child);
      }
      document_.append_child(furthest_block, replacement);

      // 19. Swap the formatting element for its replacement in the active list.
      afe_.erase(afe_.begin() + formatting_index);
      if (bookmark > formatting_index) --bookmark;
      bookmark = std::clamp(bookmark, 0, static_cast<int>(afe_.size()));
      afe_.insert(afe_.begin() + bookmark, replacement);

      // 20. Same in the stack of open elements, immediately *below* the furthest
      //     block — the stack grows towards the current node, so that is the slot
      //     right after it (html5lib: index(furthestBlock) + 1).
      remove_from_stack(index_in_stack(formatting));
      const int furthest_now = index_in_stack(furthest_block);
      stack_.insert(stack_.begin() + (furthest_now < 0 ? static_cast<int>(stack_.size()) : furthest_now + 1), replacement);
    }
  }

  /// "Table scope": the same walk as tag_in_scope(), but only html/table/template
  /// stop it — a cell or a row does not.
  [[nodiscard]] bool in_table_scope(std::string_view tag) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view top = tag_of(stack_[i - 1]);
      if (is_html_named(stack_[i - 1], tag)) return true;
      if (top == "html" || top == "table" || top == "template") return false;
    }
    return false;
  }

  /// The element types that end a plain "in scope" walk. `<select>` is one of
  /// them since the customizable-`<select>` rules replaced the old "in select"
  /// insertion mode: it keeps `</font>` from reaching a formatting element that
  /// sits outside the select (`<font><select><option>a</option></font>`).
  [[nodiscard]] static bool is_default_scope_boundary(std::string_view tag) {
    return tag == "applet" || tag == "caption" || tag == "html" || tag == "table" || tag == "td" ||
           tag == "th" || tag == "marquee" || tag == "object" || tag == "select" || tag == "template";
  }

  /// Foreign integration points are scope boundaries too: `</p>` must not reach out
  /// of an SVG `<title>` (`<p><svg><title><p>` keeps the inner `<p>` inside the
  /// title, it does not close the outer paragraph).
  [[nodiscard]] bool is_scope_boundary(dom::NodeId node) const {
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return false;
    const std::string_view tag = tag_of(node);
    switch (document_.element_namespace(node)) {
      case dom::Namespace::Html:
        return is_default_scope_boundary(tag);
      case dom::Namespace::Mathml:
        return tag == "mi" || tag == "mo" || tag == "mn" || tag == "ms" || tag == "mtext" ||
               tag == "annotation-xml";
      case dom::Namespace::Svg:
        return tag == "foreignObject" || tag == "desc" || tag == "title";
    }
    return false;
  }

  /// True when `tag` is open above the nearest scope boundary.
  [[nodiscard]] bool tag_in_scope(std::string_view tag, bool list_scope = false) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view top = tag_of(stack_[i - 1]);
      if (is_html_named(stack_[i - 1], tag)) return true;
      if (is_scope_boundary(stack_[i - 1])) return false;
      if (list_scope && (top == "ol" || top == "ul" || top == "dl")) return false;
    }
    return false;
  }

  /// Scope check used by the adoption agency: <html>, <table>, <td>, …
  [[nodiscard]] bool in_scope(dom::NodeId node) const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      if (stack_[i - 1] == node) return true;
      if (is_scope_boundary(stack_[i - 1])) return false;
    }
    return false;
  }

  // --- foreign content (SVG / MathML) ------------------------------------
  //
  // Everything below implements the "in foreign content" rules and the tree
  // construction dispatcher that decides when they apply: an SVG or MathML element
  // is not an HTML element, so the ordinary insertion modes must not see its
  // children until an integration point re-enters HTML.

  /// The namespace of the "adjusted current node" — the fragment context element
  /// while the stack is down to the fake root, the current node otherwise.
  [[nodiscard]] dom::Namespace current_namespace() const {
    const dom::NodeId node = current();
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return dom::Namespace::Html;
    return document_.element_namespace(node);
  }

  /// MathML `mi`, `mo`, `mn`, `ms` and `mtext`: their *children* are HTML again,
  /// except for `<mglyph>`/`<malignmark>`.
  [[nodiscard]] bool is_mathml_text_integration_point(dom::NodeId node) const {
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return false;
    if (document_.element_namespace(node) != dom::Namespace::Mathml) return false;
    const std::string_view tag = document_.tag_name(node);
    return tag == "mi" || tag == "mo" || tag == "mn" || tag == "ms" || tag == "mtext";
  }

  /// `annotation-xml` with an HTML encoding, and SVG's `foreignObject`/`desc`/`title`:
  /// their children are parsed with the HTML rules.
  [[nodiscard]] bool is_html_integration_point(dom::NodeId node) const {
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return false;
    const std::string_view tag = document_.tag_name(node);
    switch (document_.element_namespace(node)) {
      case dom::Namespace::Mathml: {
        if (tag != "annotation-xml") return false;
        const std::string_view encoding = document_.attribute(node, "encoding");
        return equals_ascii_ci(encoding, "text/html") || equals_ascii_ci(encoding, "application/xhtml+xml");
      }
      case dom::Namespace::Svg:
        return tag == "foreignObject" || tag == "desc" || tag == "title";
      case dom::Namespace::Html:
        return false;
    }
    return false;
  }

  /// The dispatcher (HTML §13.2.6.5): true when the token has to be handled by the
  /// "in foreign content" rules instead of the current insertion mode.
  [[nodiscard]] bool use_foreign_rules(const Token& token) const {
    const dom::NodeId node = current();
    if (!node.valid() || document_.type(node) != dom::NodeType::Element) return false;
    const dom::Namespace ns = document_.element_namespace(node);
    if (ns == dom::Namespace::Html) return false;
    if (token.type == TokenType::EndOfFile) return false;
    if (token.type == TokenType::Doctype) return false;  // "in body" ignores it too
    if (is_mathml_text_integration_point(node)) {
      if (token.type == TokenType::Characters) return false;
      if (token.type == TokenType::StartTag && token.name != "mglyph" && token.name != "malignmark") return false;
    } else if (is_html_integration_point(node)) {
      if (token.type == TokenType::Characters) return false;
      if (token.type == TokenType::StartTag) return false;
    } else if (ns == dom::Namespace::Mathml && document_.tag_name(node) == "annotation-xml" &&
               token.type == TokenType::StartTag && token.name == "svg") {
      return false;
    }
    return true;
  }

  /// Pops foreign elements until HTML (or an integration point) is current again.
  void pop_to_html_or_integration_point() {
    while (stack_.size() > (fragment_mode_ ? std::size_t{1} : std::size_t{0})) {
      const dom::NodeId node = current();
      if (document_.type(node) != dom::NodeType::Element) break;
      if (document_.element_namespace(node) == dom::Namespace::Html) break;
      if (is_mathml_text_integration_point(node) || is_html_integration_point(node)) break;
      stack_.pop_back();
    }
  }

  /// `adjust SVG attributes` / `adjust MathML attributes`: fix up the case of the
  /// handful of names that are not all lowercase.
  static void adjust_attributes_for(dom::Namespace ns, Token& token) {
    const NamePair* table = nullptr;
    std::size_t count = 0;
    if (ns == dom::Namespace::Svg) {
      table = kSvgAttributeNames;
      count = std::size(kSvgAttributeNames);
    }
    if (table == nullptr) {
      if (ns == dom::Namespace::Mathml) {
        for (auto& [key, value] : token.attributes) {
          if (key == "definitionurl") key = "definitionURL";
        }
      }
      return;
    }
    for (auto& [key, value] : token.attributes) {
      for (std::size_t i = 0; i < count; ++i) {
        if (key == table[i].from) {
          key = table[i].to;
          break;
        }
      }
    }
  }

  void insert_foreign_element(const Token& token, dom::Namespace ns) {
    const dom::NodeId element = document_.create_element(token.name, ns);
    for (const auto& [key, value] : token.attributes) {
      // Only the names in the table become namespaced attributes; `xml:base`, for
      // instance, stays a literal attribute name (`xml:baaah=""` in the outlines).
      const ForeignAttribute* known = nullptr;
      for (const ForeignAttribute& entry : kForeignAttributes) {
        if (key == entry.name) {
          known = &entry;
          break;
        }
      }
      // A bare `xmlns` is a namespaced attribute with no prefix: it keeps its name.
      if (known != nullptr && std::string_view(known->prefix) != known->local) {
        document_.set_attribute(element, known->prefix, known->local, value);
      } else {
        document_.set_attribute(element, key, value);
      }
    }
    insert_node(element);
    stack_.push_back(element);
  }

  /// `<svg>` / `<math>` seen in HTML content ("in body"): the element starts a new
  /// foreign subtree, so it goes in with its own namespace and, from then on, the
  /// dispatcher routes its children through the foreign rules.
  void insert_html_foreign_root(const Token& token) {
    const dom::Namespace ns = token.name == "svg" ? dom::Namespace::Svg : dom::Namespace::Mathml;
    Token adjusted = token;
    adjust_attributes_for(ns, adjusted);
    reconstruct_formatting();
    insert_foreign_element(adjusted, ns);
    if (adjusted.self_closing) stack_.pop_back();
  }

  void foreign_start_tag(const Token& token) {
    const std::string_view name = token.name;
    const bool breakout_font =
        name == "font" && (token.has_attribute("color") || token.has_attribute("face") || token.has_attribute("size"));
    if (contains(kForeignBreakoutTags, name) || breakout_font) {
      report("html.breakout_from_foreign_content",
             "<" + std::string(name) + "> ends the foreign content");
      pop_to_html_or_integration_point();
      handle_start_tag(token);
      return;
    }
    Token adjusted = token;
    const dom::Namespace ns = current_namespace();
    if (ns == dom::Namespace::Svg) {
      for (const NamePair& pair : kSvgTagNames) {
        if (adjusted.name == pair.from) {
          adjusted.name = pair.to;
          break;
        }
      }
    }
    adjust_attributes_for(ns, adjusted);
    insert_foreign_element(adjusted, ns);
    if (adjusted.self_closing) stack_.pop_back();
  }

  /// Characters inside foreign content: inserted as they are (NUL becomes U+FFFD),
  /// with no reconstruction and no whitespace gymnastics.
  void foreign_characters(std::string_view text) {
    std::string data;
    data.reserve(text.size());
    for (const char c : text) {
      if (c == '\0') {
        append_utf8(data, kReplacementCharacter);
      } else {
        data.push_back(c);
      }
    }
    if (!data.empty()) document_.append_text(insertion_parent(), data);
    // Whitespace and NUL (already replaced by U+FFFD above) do not close the door on
    // a <frameset>; any other character does.
    for (const char c : text) {
      if (c != '\0' && !is_ascii_space(c)) {
        frameset_ok_ = false;
        break;
      }
    }
  }

  void foreign_end_tag(std::string_view name) {
    if (name == "br" || name == "p") {  // `<br>`/`<p>` end tags are breakout tags too
      pop_to_html_or_integration_point();
      handle_end_tag_name(name);
      return;
    }
    if (name == "script" && current_namespace() == dom::Namespace::Svg && current_is("script")) {
      stack_.pop_back();  // SVG script: the script itself is simply closed
      return;
    }
    int index = static_cast<int>(stack_.size()) - 1;
    dom::NodeId node = stack_[static_cast<std::size_t>(index)];
    if (!equals_ascii_ci(tag_of(node), name)) {
      report("html.misnested_foreign_end_tag", "</" + std::string(name) + "> does not match the current node");
    }
    for (;;) {
      if (index == 0) return;  // fragment case: the fake root is the topmost element
      if (equals_ascii_ci(tag_of(node), name)) {
        stack_.resize(static_cast<std::size_t>(index));
        return;
      }
      --index;
      node = stack_[static_cast<std::size_t>(index)];
      if (document_.element_namespace(node) == dom::Namespace::Html) {
        handle_end_tag_name(name);  // back in HTML: the ordinary rules decide
        return;
      }
    }
  }

  /// "In caption" folded into the body rules: `mode()` reports Body while a caption
  /// is the nearest table-related element on the stack, so the table-structure
  /// tokens that the spec's "in caption" mode owns are intercepted here. They close
  /// the caption and are reprocessed one insertion mode up (`<caption><p></table>`
  /// must close the caption *and* the table, not stop at the paragraph).
  [[nodiscard]] bool caption_context() const {
    return mode() == Mode::Body && in_table_scope("caption");
  }

  void close_caption() {
    generate_implied_end_tags();
    pop_through("caption");
    clear_to_last_marker();
  }

  [[nodiscard]] static bool is_table_structure(std::string_view name) {
    return name == "caption" || name == "col" || name == "colgroup" || name == "tbody" || name == "td" ||
           name == "tfoot" || name == "th" || name == "thead" || name == "tr" || name == "table";
  }

  /// Block-level end tags: `<div>`, `<ul>`, `<button>`, … The spec pops them after
  /// generating implied end tags, which is how `</div>` closes a dangling `<p>`.
  void block_end_tag(std::string_view name) {
    if (!tag_in_scope(name)) {  // a scope boundary (<template>, <td>, <table>, …) wins
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored: <" + std::string(name) +
                                         "> is not in scope");
      return;
    }
    const std::string_view top = stack_.empty() ? std::string_view{} : tag_of(stack_.back());
    generate_implied_end_tags(name);
    if (!close_open(name)) {
      report("html.stray_end_tag", "unmatched </" + std::string(name) + ">");
    } else if (top != name) {
      report("html.misnested_end_tag", "</" + std::string(name) + "> closed <" + std::string(top) + ">");
    }
  }

  /// "Any other end tag" (HTML §13.2.6.4.7, "in body"): walk down the stack from
  /// the current node. A matching element is popped together with everything above
  /// it; a "special" element — `<p>`, `<table>`, `<template>`, … — stops the search
  /// and the tag is ignored. Without that rule `</div>` climbs out of a
  /// `<template>` and `</span>` climbs out of an open `<button>`.
  void generic_end_tag(std::string_view name) {
    if (contains(kBlockEndTags, name)) {
      block_end_tag(name);
      return;
    }
    for (int i = static_cast<int>(stack_.size()) - 1; i >= 0; --i) {
      const dom::NodeId node = stack_[static_cast<std::size_t>(i)];
      if (tag_of(node) == name) {
        generate_implied_end_tags(name);
        const int index = index_in_stack(node);
        if (index >= 0) stack_.resize(static_cast<std::size_t>(index));
        return;
      }
      if (is_special(tag_of(node))) {
        report("html.ignored_end_tag",
               "</" + std::string(name) + "> ignored: <" + std::string(tag_of(node)) + "> is a boundary");
        return;
      }
    }
    report("html.stray_end_tag", "unmatched </" + std::string(name) + ">");
  }

  // --- insertion modes ---------------------------------------------------
  //
  // The parser is not one "in body" loop with table special-cases sprinkled in:
  // the current node selects the insertion mode, and the mode owns the rules.
  // `<table>` → Table, `<tbody>` → TableBody, `<tr>` → Row, `<td>` → Cell,
  // `<colgroup>` → ColumnGroup. Everything else is Body (which also covers the
  // content of a `<template>`, because the template element is the current node).
  /// The nearest element that defines a mode wins; formatting and block elements
  /// sitting above it (foster-parented or inside a cell) do not change the mode,
  /// which is what keeps `<table><a>1<td>` in the "in table" rules.
  [[nodiscard]] Mode mode() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      // Foreign elements do not own an insertion mode: `<svg template>` must not
      // pretend a template's mode is current.
      if (document_.element_namespace(stack_[i - 1]) != dom::Namespace::Html) continue;
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "td" || tag == "th") return Mode::Cell;
      if (tag == "tr") return Mode::Row;
      if (tag == "tbody" || tag == "thead" || tag == "tfoot") return Mode::TableBody;
      if (tag == "colgroup") return Mode::ColumnGroup;
      if (tag == "table") return Mode::Table;
      if (tag == "template") {
        return template_modes_.empty() ? Mode::Body : template_modes_.back();
      }
      // <caption> holds ordinary content; <html> is the outermost element.
      if (tag == "caption") return Mode::Body;
      if (tag == "html") return fragment_mode_ ? fragment_initial_mode_ : Mode::Body;
    }
    return fragment_mode_ ? fragment_initial_mode_ : Mode::Body;
  }

  /// Pop the stack back to the table (or template/html) that started the context.
  void clear_to_table_context() {
    while (!stack_.empty()) {
      const std::string_view top = tag_of(stack_.back());
      if (top == "table" || top == "template" || top == "html") break;
      stack_.pop_back();
    }
  }

  void clear_to_table_body_context() {
    while (!stack_.empty()) {
      const std::string_view top = tag_of(stack_.back());
      if (top == "tbody" || top == "thead" || top == "tfoot" || top == "template" || top == "html") break;
      stack_.pop_back();
    }
  }

  void clear_to_row_context() {
    while (!stack_.empty()) {
      const std::string_view top = tag_of(stack_.back());
      if (top == "tr" || top == "template" || top == "html") break;
      stack_.pop_back();
    }
  }

  /// Pop up to and including the innermost cell (or row / table body).
  /// Returns true when something was actually popped: the fragment case has no
  /// element to pop (the context element is not on the stack), and callers must
  /// not reprocess the token when nothing was closed — that would loop.
  bool pop_through(std::string_view tag_a, std::string_view tag_b = {}, std::string_view tag_c = {}) {
    bool popped = false;
    while (!stack_.empty()) {
      const std::string_view top = tag_of(stack_.back());
      const dom::NodeId top_node = stack_.back();
      const bool hit = is_html_named(top_node, tag_a) ||
                       (tag_b.size() != 0 && is_html_named(top_node, tag_b)) ||
                       (tag_c.size() != 0 && is_html_named(top_node, tag_c));
      if (stack_.size() == 1) {  // never pop the document's <html> element
        report("html.misnested_end_tag", "pop through <" + std::string(top) + "> stopped at <html>");
        return popped;
      }
      stack_.pop_back();
      popped = true;
      if (hit) return true;
    }
    return popped;
  }

  void close_cell() {
    generate_implied_end_tags();
    pop_through("td", "th");
    clear_to_last_marker();
  }

  /// A token with no attributes — for synthetic elements (`<tbody>`, `<tr>`,
  /// `<colgroup>`) and the `</br>`/`</p>` start tags the spec conjures up.
  [[nodiscard]] static Token plain_token(TokenType type, std::string_view name, bool self_closing = false) {
    Token token;
    token.type = type;
    token.name = std::string(name);
    token.self_closing = self_closing;
    return token;
  }

  /// Inserts a synthetic element (`<tbody>`, `<tr>`, `<colgroup>`) — the implicit
  /// parts of a table that authors leave out.
  void insert_synthetic(std::string_view name) { insert_element(plain_token(TokenType::StartTag, name), true); }

  [[nodiscard]] bool is_table_part(std::string_view name) const {
    return name == "caption" || name == "col" || name == "colgroup" || name == "tbody" || name == "tfoot" ||
           name == "thead" || name == "td" || name == "th" || name == "tr";
  }

  /// "In table" start tags. Returns true when the token was consumed.
  bool table_start(const Token& token) {
    const std::string_view name = token.name;
    if (name == "caption") {
      clear_to_table_context();
      insert_element(token, true);
      push_marker();
      return true;
    }
    if (name == "colgroup") {
      clear_to_table_context();
      insert_element(token, true);
      return true;
    }
    if (name == "col") {
      clear_to_table_context();
      insert_synthetic("colgroup");
      insert_element(token, false);
      return true;
    }
    if (name == "tbody" || name == "tfoot" || name == "thead") {
      clear_to_table_context();
      insert_element(token, true);
      return true;
    }
    if (name == "td" || name == "th" || name == "tr") {
      clear_to_table_context();
      insert_synthetic("tbody");
      if (name != "tr") insert_synthetic("tr");
      insert_element(token, true);
      if (name != "tr") push_marker();
      return true;
    }
    if (name == "table") {  // a table inside a table closes the outer one
      if (!in_table_scope("table")) {  // fragment case: no table to close
        report("html.ignored_in_table", "<table> ignored: no table in scope");
        return true;
      }
      report("html.nested_table", "<table> inside <table> closes the outer table");
      pop_through("table");
      handle_start_tag(token);
      return true;
    }
    if (name == "style" || name == "script" || name == "template") return false;  // "in head" rules
    if (name == "form") {  // the form element itself never stays open inside a table
      if (form_.valid()) {
        report("html.nested_form", "<form> ignored: a form is already open");
        return true;
      }
      insert_element(token, true);
      form_ = current();
      stack_.pop_back();
      return true;
    }
    if (name == "input" && is_hidden_input(token)) {
      insert_element(token, false);
      return true;
    }
    // `<svg>`/`<math>` open foreign content from inside a table too; they are
    // fostered out of the table like any other content.
    if (name == "svg" || name == "math") {
      insert_html_foreign_root(token);
      return true;
    }
    // Anything else is foster-parented and processed by the "in body" rules: that
    // matters for formatting elements, which have to stay in the active list so a
    // later character token can reconstruct them.
    if (is_formatting(name)) {
      start_formatting(token);
      return true;
    }
    implied_end_tags(name);
    // "Any other start tag" reconstructs the active formatting elements; with
    // foster parenting still on, those clones land in front of the table
    // (`<table><center><font>a</center> <img>` keeps the <font> alive).
    if (!contains(kNoReconstruct, name)) reconstruct_formatting();
    const dom::NodeId element = make_element(token);
    insert_node(element);
    if (!contains(kVoid, name)) stack_.push_back(element);
    return true;
  }

  /// "In table" end tags.
  bool table_end(std::string_view name) {
    if (name == "table") {
      if (!in_table_scope("table")) {
        report("html.ignored_end_tag", "</table> without a table in scope");
        return true;
      }
      pop_through("table");
      return true;
    }
    if (name == "body" || name == "caption" || name == "col" || name == "colgroup" || name == "html" ||
        name == "tbody" || name == "td" || name == "tfoot" || name == "th" || name == "thead" || name == "tr") {
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored inside <table>");
      return true;
    }
    return false;  // "in body" rules (e.g. </template>, </div>, formatting end tags)
  }

  /// "In table body": `<tr>`, cells, and closing the section.
  bool table_body_start(const Token& token) {
    const std::string_view name = token.name;
    if (name == "tr") {
      clear_to_table_body_context();
      insert_element(token, true);
      return true;
    }
    if (name == "td" || name == "th") {
      clear_to_table_body_context();
      insert_synthetic("tr");
      insert_element(token, true);
      push_marker();
      return true;
    }
    if (name == "caption" || name == "col" || name == "colgroup" || name == "tbody" || name == "tfoot" ||
        name == "thead") {
      if (!in_table_scope("tbody") && !in_table_scope("thead") && !in_table_scope("tfoot")) {
        report("html.ignored_in_table", "<" + std::string(name) + "> ignored: no table body in scope");
        return true;
      }
      pop_through("tbody", "thead", "tfoot");
      handle_start_tag(token);  // reprocess in the "in table" mode
      return true;
    }
    return table_start(token);
  }

  bool table_body_end(std::string_view name) {
    if (name == "tbody" || name == "tfoot" || name == "thead") {
      if (!in_table_scope(name)) return true;
      pop_through(name);
      return true;
    }
    if (name == "table") {
      if (!in_table_scope("tbody") && !in_table_scope("thead") && !in_table_scope("tfoot")) return true;
      pop_through("tbody", "thead", "tfoot");
      handle_end_tag_name(name);
      return true;
    }
    if (name == "body" || name == "caption" || name == "col" || name == "colgroup" || name == "html" ||
        name == "td" || name == "th" || name == "tr") {
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored in a table body");
      return true;
    }
    return false;
  }

  bool row_start(const Token& token) {
    const std::string_view name = token.name;
    if (name == "td" || name == "th") {
      clear_to_row_context();
      insert_element(token, true);
      push_marker();
      return true;
    }
    if (name == "caption" || name == "col" || name == "colgroup" || name == "tbody" || name == "tfoot" ||
        name == "thead" || name == "tr") {
      if (!in_table_scope("tr")) {
        report("html.ignored_in_table", "<" + std::string(name) + "> ignored: no row in scope");
        return true;
      }
      pop_through("tr");
      handle_start_tag(token);  // reprocess in the "in table body" mode
      return true;
    }
    return table_start(token);
  }

  bool row_end(std::string_view name) {
    if (name == "tr") {
      if (!in_table_scope("tr")) return true;
      pop_through("tr");
      return true;
    }
    if (name == "table") {
      if (!in_table_scope("tr")) {  // fragment case: no row on the stack to close
        report("html.ignored_end_tag", "</table> ignored: no row in table scope");
        return true;
      }
      pop_through("tr");
      handle_end_tag_name(name);
      return true;
    }
    if (name == "tbody" || name == "tfoot" || name == "thead") {
      if (!in_table_scope(name)) return true;
      pop_through("tr");
      handle_end_tag_name(name);
      return true;
    }
    if (name == "body" || name == "caption" || name == "col" || name == "colgroup" || name == "html" ||
        name == "td" || name == "th") {
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored in a row");
      return true;
    }
    return false;
  }

  /// "In cell": any table part closes the cell and is reprocessed one mode up.
  /// Without a cell in table scope the token is ignored (the fragment case).
  bool cell_start(const Token& token) {
    if (!is_table_part(token.name)) return false;  // ordinary content: "in body" rules
    if (!in_table_scope("td") && !in_table_scope("th")) return true;
    close_cell();
    handle_start_tag(token);
    return true;
  }

  bool cell_end(std::string_view name) {
    if (name == "td" || name == "th") {
      if (!in_table_scope(name)) return true;
      close_cell();
      return true;
    }
    if (name == "table" || name == "tbody" || name == "tfoot" || name == "thead" || name == "tr") {
      if (!in_table_scope(name)) return true;
      close_cell();
      handle_end_tag_name(name);
      return true;
    }
    if (name == "body" || name == "caption" || name == "col" || name == "colgroup" || name == "html") {
      report("html.ignored_end_tag", "</" + std::string(name) + "> ignored in a cell");
      return true;
    }
    return false;
  }

  /// True when a real <colgroup> element is open above the nearest <table>,
  /// <template> or <html>. A column group that exists only as a template
  /// insertion mode has nothing to pop, so tokens there are simply dropped.
  [[nodiscard]] bool colgroup_open() const {
    for (std::size_t i = stack_.size(); i > 0; --i) {
      const std::string_view tag = tag_of(stack_[i - 1]);
      if (tag == "colgroup") return true;
      if (tag == "table" || tag == "template" || tag == "html") return false;
    }
    return false;
  }

  bool column_group_start(const Token& token) {
    if (token.name == "col") {
      insert_element(token, false);
      return true;
    }
    if (token.name == "template") return false;
    if (!colgroup_open()) {
      report("html.ignored_in_colgroup", "<" + std::string(token.name) + "> ignored inside <colgroup>");
      return true;
    }
    if (!pop_through("colgroup")) {  // fragment case: the context is not on the stack
      report("html.ignored_in_colgroup", "<" + std::string(token.name) + "> ignored inside <colgroup>");
      return true;
    }
    handle_start_tag(token);
    return true;
  }

  bool column_group_end(std::string_view name) {
    if (name == "colgroup") {
      pop_through("colgroup");
      return true;
    }
    if (name == "col") return true;
    if (name == "template") return false;
    if (!pop_through("colgroup")) return true;
    handle_end_tag_name(name);
    return true;
  }

  /// Start tag for a formatting element: the `<a>`/`<nobr>` special cases, then
  /// reconstruction, insertion and the active-formatting entry.
  void start_formatting(const Token& token) {
    const std::string_view name = token.name;
    if (is_formatting(name)) {
      if (name == "nobr" && has_open("nobr")) {
        adoption_agency("nobr");
        reconstruct_formatting();
      }
      if (name == "a") {
        dom::NodeId open_a;  // copy first: the adoption agency rewrites the list
        for (std::size_t i = afe_.size(); i > 0; --i) {
          if (!afe_[i - 1].valid()) break;
          if (document_.tag_name(afe_[i - 1]) == "a") {
            open_a = afe_[i - 1];
            break;
          }
        }
        if (open_a.valid()) {
          // "Run the adoption agency algorithm for the token, then remove that
          // element from the list of active formatting elements and the stack of
          // open elements if the algorithm didn't already remove it" — the second
          // half matters when the element is not in table scope, where the
          // algorithm bails out without touching anything.
          adoption_agency("a");
          if (index_in_afe(open_a) >= 0) remove_from_afe(open_a);
          if (const int at = index_in_stack(open_a); at >= 0) remove_from_stack(at);
        }
      }
      reconstruct_formatting();
      implied_end_tags(name);
      const dom::NodeId element = make_element(token);
      insert_node(element);
      stack_.push_back(element);
      push_formatting(element);
      return;
    }

  }

  /// Creates an element with its attributes; `<template>` also gets its contents
  /// fragment, which is where its children will actually land.
  [[nodiscard]] dom::NodeId make_element(const Token& token) {
    const dom::NodeId element = document_.create_element(token.name);
    for (const auto& [key, value] : token.attributes) document_.set_attribute(element, key, value);
    if (token.name == "template") document_.append_child(element, document_.create_fragment());
    return element;
  }

  void insert_element(const Token& token, bool push) {
    const dom::NodeId element = make_element(token);
    document_.append_child(insertion_parent(), element);
    if (push) stack_.push_back(element);
  }

  /// Appends every whitespace run of `text` to `target` and drops the rest — the
  /// "only whitespace belongs here" rule of <frameset> and "after frameset".
  void append_whitespace_runs(dom::NodeId target, std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
      std::size_t run = i;
      while (run < text.size() && is_ascii_space(text[run])) ++run;
      if (run > i) document_.append_text(target, text.substr(i, run - i));
      i = run;
      while (i < text.size() && !is_ascii_space(text[i])) ++i;
    }
  }

  void append_text(std::string_view text) {
    if (text.empty()) return;
    if (table_context() && !trim(text).empty()) {  // text inside a table is moved out
      foster_text(text);
      return;
    }
    document_.append_text(insertion_parent(), text);
  }

  void report(std::string_view code, std::string_view message) {
    if (diagnostics_ == nullptr) return;
    diagnostics_->add(Severity::Warning, SourcePos{}, std::string(code) + ": " + std::string(message));
  }

  // --- token handling ----------------------------------------------------
  void process(const Token& token) {
    // "After body": comments keep landing on the <html> element until real content
    // arrives (which switches back to "in body") — what makes `</html><!--x-->`
    // put the comment next to <head>/<body> rather than inside <body>.
    if (after_html_) {
      // "After after body": a comment is the last child of the Document, anything
      // else puts the parser back into "in body" and the token is reprocessed.
      // The one exception is `<noframes>` in "after after frameset", which the
      // spec hands to the "in head" rules *without* leaving the mode — that is why
      // `<frameset></frameset></html><noframes></noframes><!--x-->` still puts the
      // comment on the Document.
      if (token.type == TokenType::Comment) {
        document_.append_child(document_.root(), document_.create_comment(token.data));
        return;
      }
      // `<noframes>` (and the RAWTEXT it swallows) is handled by the "in head"
      // rules without leaving the mode.
      const bool noframes_tag =
          token.name == "noframes" && (token.type == TokenType::StartTag || token.type == TokenType::EndTag);
      const bool noframes_text = token.type == TokenType::Characters && has_open("noframes");
      const bool stays = frameset_document() && (noframes_tag || noframes_text);
      // Whitespace is inserted with the "in body" rules but does not leave the mode.
      const bool space = token.type == TokenType::Characters && trim(token.data).empty();
      if (token.type != TokenType::EndOfFile && !stays && !space) after_html_ = false;
    }
    if (after_body_) {
      if (token.type == TokenType::Comment && html_.valid()) {
        document_.append_child(html_, document_.create_comment(token.data));
        return;
      }
      const bool space = token.type == TokenType::Characters && trim(token.data).empty();
      if (token.type != TokenType::EndOfFile && !space) after_body_ = false;
    }
    if (ignore_next_lf_ && token.type != TokenType::Characters) ignore_next_lf_ = false;
    switch (token.type) {
      case TokenType::Doctype: {
        // Only the very first doctype counts; later ones are ignored.
        if (document_element_seen_ || doctype_seen_) return;
        doctype_seen_ = true;
        // A missing, non-"html" or "quirky" doctype leaves the document in quirks
        // mode (HTML §13.2.6.4.1); the only rule that cares here is the
        // `<p>`/`<table>` interaction above.
        quirks_ = doctype_mode(token) == DoctypeMode::Quirks;
        const dom::NodeId doctype =
            token.has_public_identifier || token.has_system_identifier
                ? document_.create_doctype(token.data, token.public_identifier, token.system_identifier)
                : document_.create_doctype(token.data);
        document_.append_child(document_.root(), doctype);
        return;
      }
      case TokenType::Comment: {
        // Before <html> a comment belongs to the document; between <html> and <head>/<body>
        // it is a child of <html>; afterwards it goes to the current node.
        dom::NodeId target = document_.root();
        if (html_.valid()) {
          target = current();
          if (!head_created() && !body_created()) target = html_;
          if (frameset_document() && !in_frameset()) {
            // "after frameset" keeps comments on <html>, "after after frameset" (past
            // </html>) puts them back on the Document.
            target = after_html_ ? document_.root() : html_;
          }
          if (tag_of(target) == "template") {  // the comment belongs to the contents
            const dom::NodeId content = template_content(target);
            if (content.valid()) target = content;
          }
        }
        document_.append_child(target, document_.create_comment(token.data));
        return;
      }
      case TokenType::Characters:
        handle_characters(token.data);
        return;
      case TokenType::StartTag:
        process_start_tag(token);
        return;
      case TokenType::EndTag:
        process_end_tag(token.name);
        return;
      case TokenType::EndOfFile:
        return;
    }
  }

  /// NUL characters reach the tree builder from the tokenizer (the data state
  /// passes them through). "In body" ignores them, table text turns them into
  /// U+FFFD — exactly what the spec asks for.
  [[nodiscard]] static std::string sanitize_text(std::string_view text, bool replacement) {
    if (text.find('\0') == std::string_view::npos) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      if (c != '\0') {
        out.push_back(c);
      } else if (replacement) {
        out.append("\xEF\xBF\xBD");  // U+FFFD
      }
    }
    return out;
  }

  void handle_characters(std::string_view text) {
    {
      Token probe;
      probe.type = TokenType::Characters;
      if (use_foreign_rules(probe)) {
        foreign_characters(text);
        return;
      }
    }

    if (ignore_next_lf_) {  // "ignore a LF right after <pre>/<listing>/<textarea>"
      ignore_next_lf_ = false;
      if (!text.empty() && text.front() == '\n') {
        text.remove_prefix(1);
        if (text.empty()) return;
      }
    }
    if (in_head_noscript()) {
      if (!trim(text).empty()) {  // pop the noscript and reprocess in "in head"
        close_open("noscript");
        handle_characters(text);
      } else {
        append_text(text);
      }
      return;
    }
    const std::string_view top = tag_of(current());
    if (top == "frameset" || top == "frame") {  // only whitespace survives here
      append_whitespace_runs(current(), text);
      if (!trim(text).empty()) report("html.ignored_in_frameset", "text ignored inside <frameset>");
      return;
    }
    // A frameset document has no body: whitespace still lands in <html>, anything
    // else after </frameset> is dropped (<noframes>/<template> keep real content).
    if (frameset_document() && !has_open("noframes") && !has_open("template")) {
      if (!in_frameset()) append_whitespace_runs(html_, text);
      return;
    }
    if (!body_created()) {
      const dom::NodeId top_node = current();
      const bool inside_element = html_.valid() && top_node != html_ && top_node != head_;
      if (inside_element) {  // text inside <title>/<style>/<script> in the head
        append_text(text);
        return;
      }
      // html5lib's tokenizer emits whitespace runs as their own tokens, so the
      // leading run of a mixed token is handled on its own: "before head" drops it,
      // "in head"/"after head" inserts it into the current node (head/html) — and
      // only the rest of the token creates the body.
      std::size_t lead = 0;
      while (lead < text.size() && is_ascii_space(text[lead])) ++lead;
      if (lead > 0 && head_.valid()) document_.append_text(current(), text.substr(0, lead));
      if (lead == text.size()) return;
      text.remove_prefix(lead);
      ensure_body();
    }
    const Mode current_mode = mode();
    if (current_mode == Mode::ColumnGroup) {
      if (trim(text).empty()) {
        document_.append_text(current(), text);  // whitespace stays in the colgroup
        return;
      }
      // Mixed text: the leading whitespace run still belongs to the colgroup, the
      // rest pops it and is reprocessed (html5lib splits character tokens that way).
      std::size_t lead = 0;
      while (lead < text.size() && is_ascii_space(text[lead])) ++lead;
      if (lead > 0) {
        document_.append_text(current(), text.substr(0, lead));
        text.remove_prefix(lead);
      }
      // "Anything else": pop the colgroup and let the table rules reprocess the text
      // (the fragment case has nothing to pop, so it is dropped there).
      if (!colgroup_open() || !pop_through("colgroup")) {
        report("html.ignored_in_colgroup", "text ignored inside <colgroup>");
        return;
      }
      handle_characters(text);
      return;
    }
    const bool in_table_text = current_mode == Mode::Table || current_mode == Mode::TableBody ||
                               current_mode == Mode::Row;
    std::string sanitized;  // keeps the cleaned text alive while we use the view
    if (text.find('\0') != std::string_view::npos) {
      // Whitespace stays inside the table, so its NULs become U+FFFD there; the
      // non-whitespace part is fostered out and the "in body" rule drops them.
      sanitized = sanitize_text(text, in_table_text && trim(text).empty());
      text = sanitized;
    }
    if (text.empty()) return;
    if (in_table_text) {
      if (trim(text).empty()) {
        document_.append_text(current(), text);  // whitespace may stay inside the table
      } else {
        reconstruct_formatting();  // a formatting element may have been popped above
        insert_text(text);
      }
      return;
    }
    if (!trim(text).empty()) frameset_ok_ = false;
    reconstruct_formatting();
    insert_text(text);
  }

  // --- the tree construction dispatcher ----------------------------------
  //
  // Every token enters through these two functions: they decide whether the current
  // insertion mode sees it (HTML content) or the "in foreign content" rules do. The
  // `handle_*` functions below are the HTML side only — "reprocess the token in HTML
  // content" calls them directly, so a breakout can never bounce back into the
  // foreign rules it just left.

  void process_start_tag(const Token& token) {
    if (use_foreign_rules(token)) {
      foreign_start_tag(token);
      return;
    }
    handle_start_tag(token);
  }

  void process_end_tag(std::string_view name) {
    Token probe;
    probe.type = TokenType::EndTag;
    probe.name = std::string(name);
    if (use_foreign_rules(probe)) {
      foreign_end_tag(name);
      return;
    }
    handle_end_tag_name(name);
  }

  void handle_start_tag(const Token& token) {
    const std::string_view name = token.name;
    document_element_seen_ = true;

    // Fragment parsing: the stack holds only the fake <html> root, so the skeleton
    // tags of a document (`<html>`, `<head>`, `<body>`, `<frameset>`) are parse
    // errors and ignored — they cannot restructure the fragment.
    if (fragment_mode_ && stack_.size() <= 1 &&
        (name == "html" || name == "head" || name == "body" || name == "frameset")) {
      report("html.ignored_in_fragment", "<" + std::string(name) + "> ignored inside a fragment");
      return;
    }

    if (name == "image") {  // "Parse error. Change the token's tag name to img."
      Token renamed = token;
      renamed.name = "img";
      report("html.image_renamed", "<image> treated as <img>");
      handle_start_tag(renamed);
      return;
    }

    // A <select> fragment has no select on the stack, so the "in select" scope is
    // empty. html5lib's corpus drops a bare <input> here (that rule owns input
    // only) while keygen/textarea and unknown tags use the ordinary body rules.
    if (fragment_mode_ && tag_of(context_) == "select" && name == "input") {
      report("html.ignored_in_select", "<input> ignored inside a <select> fragment");
      return;
    }

    if (name == "html") {
      if (has_open("template")) {  // "if the stack has a template element, ignore"
        report("html.ignored_in_template", "<html> ignored inside <template>");
        return;
      }
      if (!html_.valid()) {
        ensure_html();
      }
      for (const auto& [key, value] : token.attributes) {
        if (document_.attribute(html_, key).empty()) document_.set_attribute(html_, key, value);
      }
      return;
    }

    if (name == "head") {
      if (head_.valid()) return;
      ensure_head();
      for (const auto& [key, value] : token.attributes) document_.set_attribute(head_, key, value);
      return;
    }

    if (name == "body") {
      if (has_open("template")) {  // same: attributes must not reach the body either
        report("html.ignored_in_template", "<body> ignored inside <template>");
        return;
      }
      if (body_.valid()) {
        for (const auto& [key, value] : token.attributes) {
          if (document_.attribute(body_, key).empty()) document_.set_attribute(body_, key, value);
        }
        frameset_ok_ = false;  // the body merge is content: <frameset> is out
        return;
      }
      ensure_body();
      frameset_ok_ = false;  // the first <body> is content too: <frameset> is out
      for (const auto& [key, value] : token.attributes) document_.set_attribute(body_, key, value);
      return;
    }

    if (in_head_noscript()) {
      // Only the "in head" elements are processed in place; a nested <head>/<noscript>
      // is a parse error, anything else closes the noscript and reprocesses (which is
      // what turns `<head><noscript><p>x` into a body paragraph).
      const bool allowed = name == "basefont" || name == "bgsound" || name == "link" || name == "meta" ||
                           name == "noframes" || name == "style";
      if (!allowed) {
        if (name == "head" || name == "noscript") {
          report("html.ignored_in_head_noscript",
                 "<" + std::string(name) + "> ignored inside <noscript> in <head>");
          return;
        }
        close_open("noscript");
        handle_start_tag(token);
        return;
      }
    }

    if (name == "frameset") {
      if (in_frameset()) {  // a nested frameset is just another child
        insert_element(token, true);
        return;
      }
      if (body_created() && !frameset_ok_) {
        report("html.ignored_frameset", "<frameset> ignored: the body already has content");
        return;
      }
      // "in body": a frameset can only replace a body. While a <template> is open
      // the stack has no body below the content (`<template><div><frameset>`), so
      // the tag is a parse error and gets dropped.
      if (has_open("template")) {
        report("html.ignored_frameset", "<frameset> ignored: a <template> is open");
        return;
      }
      ensure_html();
      if (!head_created()) {
        ensure_head();
        close_head();
      }
      enter_frameset(token);
      return;
    }

    if (name == "frame" && !in_frameset()) {  // <frame> outside a frameset is dropped
      report("html.stray_frame", "<frame> outside <frameset> ignored");
      return;
    }

    if (frameset_document() && !in_frameset() && name != "html" && name != "noframes" && name != "template") {
      report("html.ignored_after_frameset", "<" + std::string(name) + "> ignored after </frameset>");
      return;
    }

    if (in_frameset()) {
      if (name == "frame") {
        insert_element(token, false);
        return;
      }
      if (name != "noframes") {  // <template> is not honoured inside a frameset either
        report("html.ignored_in_frameset", "<" + std::string(name) + "> ignored inside <frameset>");
        return;
      }
    }

    if (!body_created() && contains(kHeadElements, name)) {
      ensure_head();
      // "After head": these tags still belong to <head>, so it goes back on the
      // stack for the duration of the insertion (`<title>` then stays open).
      const bool temporary_head = index_in_stack(head_) < 0;
      if (temporary_head) stack_.push_back(head_);
      insert_element(token, !contains(kVoid, name));
      if (temporary_head) remove_from_stack(index_in_stack(head_));
      if (name == "template") {  // a template opened in "in head"/"after head"
        push_marker();
        template_modes_.push_back(Mode::Template);  // the "in template" mode
      }
      return;
    }


    // `<input type=hidden>` is the one exception in the list above.
    if (contains(kClearsFramesetOk, name) && !(name == "input" && is_hidden_input(token))) {
      frameset_ok_ = false;
    }

    // Content of an open <template> is parsed as if it were in <body>, but it must
    // stay inside the template instead of creating the document's <body>.
    if (!body_created() && !has_open("template")) ensure_body();

    // `<plaintext>` swallows the rest of the document, so it is handled before the
    // insertion modes: inside a table it is fostered out and then eats everything.
    if (name == "plaintext") {
      if (!body_created() && !has_open("template")) ensure_body();
      // "in column group" hands the token to "in table", which fosters it out.
      if (mode() == Mode::ColumnGroup && colgroup_open()) pop_through("colgroup");
      implied_end_tags(name);
      const dom::NodeId element = make_element(token);
      insert_node(element);  // fosters out of a table
      stack_.push_back(element);
      tokenizer_->enter_plain_text();
      return;
    }

    // The form element pointer: a second <form> is ignored while one is open.
    if (name == "form" && !has_open("template")) {
      if (form_.valid()) {
        report("html.nested_form", "<form> ignored: a form is already open");
        return;
      }
    }

    // --- "in select in table": table tokens close the select first ----------
    if (select_in_table() && is_table_part(name)) {
      report("html.select_closed_by_table", "<" + std::string(name) + "> closes the surrounding <select>");
      pop_through("select");
      handle_start_tag(token);
      return;
    }

    // --- "in select" insertion mode ---------------------------------------
    // Options and optgroups are the elements <select> is *about*; `input`,
    // `keygen` and `textarea` close the select outright. Everything else is no
    // longer dropped: the modern (customizable <select>) content model lets
    // ordinary content — <div>, <button>, <datalist>, <selectedcontent>, … — be
    // parsed with the "in body" rules, so those tokens simply fall through.
    if (select_context()) {
      if (name == "option" || name == "optgroup") {
        if (current_is("option")) stack_.pop_back();
        if (name == "optgroup" && current_is("optgroup")) stack_.pop_back();
        reconstruct_formatting();
        insert_element(token, true);
        return;
      }
      if (name == "hr") {  // `<hr>` is allowed inside a <select> as a separator
        if (current_is("option")) stack_.pop_back();
        if (current_is("optgroup")) stack_.pop_back();
        insert_element(token, false);
        return;
      }
      if (name == "select") {
        // "Parse error. Ignore the token. Pop elements from the stack of open
        // elements until a select element has been popped from the stack."
        report("html.nested_select", "<select> nested in <select> closes the outer one");
        close_open("select");
        return;
      }
      if (name == "input") {
        if (!select_in_select_scope()) {
          report("html.ignored_in_select", "<" + std::string(name) + "> ignored: not in select scope");
          return;
        }
        if (!close_open("select")) {  // fragment case: nothing on the stack to close
          report("html.ignored_in_select", "<" + std::string(name) + "> ignored inside <select>");
          return;
        }
        report("html.select_closed_by_input", "<" + std::string(name) + "> inside <select> closes it");
      }
    }

    // --- "in caption": a table part closes the caption and is reprocessed ------
    // Otherwise `<table><caption><td>` would drop the cell: the caption is "in
    // body" for ordinary content, but a table token has to pop it first.
    if (is_table_part(name) && in_table_scope("caption")) {
      report("html.caption_closed_by_table_part",
             "<" + std::string(name) + "> closes the surrounding <caption>");
      generate_implied_end_tags();
      pop_through("caption");
      clear_to_last_marker();
      handle_start_tag(token);
      return;
    }

    // --- "in template": the first content token picks the template's mode ------
    // A freshly opened <template> is "in template": table-ish tokens switch to the
    // matching table mode, everything else switches to "in body". The choice is
    // remembered per template (that is what `mode()` reports while the template is
    // the current node), so content *after* that first token is no longer
    // table-ish: `<template><div>x</div><tr>` drops the row, it does not grow a
    // table, and `<template><tr>` still builds one inside the contents. The
    // head-ish tokens (`<script>`, `<meta>`, `<template>`, …) are handed to the
    // "in head" rules below and leave the template's mode untouched.
    if (mode() == Mode::Template && !(name != "noscript" && contains(kHeadElements, name))) {
      Mode& current = template_modes_.back();
      const std::string_view t = name;
      if (t == "caption" || t == "colgroup" || t == "tbody" || t == "tfoot" || t == "thead") {
        current = Mode::Table;
      } else if (t == "col") {
        current = Mode::ColumnGroup;
      } else if (t == "tr") {
        current = Mode::TableBody;
      } else if (t == "td" || t == "th") {
        current = Mode::Row;
      } else {
        current = Mode::Body;
      }
      handle_start_tag(token);
      return;
    }

    // --- insertion modes --------------------------------------------------
    // The table modes own <caption>/<col>/<tbody>/<tr>/<td> and foster-parent
    // everything else; the body mode (below) handles ordinary content.
    switch (mode()) {
      case Mode::Table:
        if (table_start(token)) return;
        break;
      case Mode::TableBody:
        if (table_body_start(token)) return;
        break;
      case Mode::Row:
        if (row_start(token)) return;
        break;
      case Mode::Cell:
        if (cell_start(token)) return;
        break;
      case Mode::ColumnGroup:
        if (column_group_start(token)) return;
        break;
      case Mode::Body:
      case Mode::Template:  // unreachable: "in template" is handled above
        break;
    }

    // "In caption": a table-structure token closes the caption and is reprocessed
    // by the "in table" rules.
    if (name != "table" && is_table_structure(name) && caption_context()) {
      close_caption();
      handle_start_tag(token);
      return;
    }

    // "Anything else" in body, first the table parts: with no table cell above
    // them these are parse errors and must be dropped (`<tr>` outside a table
    // does not get to grow a table-less row).
    if (name == "caption" || name == "col" || name == "colgroup" || name == "tbody" || name == "td" ||
        name == "tfoot" || name == "th" || name == "thead" || name == "tr") {
      report("html.ignored_table_part", "<" + std::string(name) + "> ignored outside a table");
      return;
    }

    // `<svg>` and `<math>` open foreign content from HTML: the element goes into its
    // own namespace and every child token below it follows the foreign rules until
    // an integration point hands control back.
    if (name == "svg" || name == "math") {
      insert_html_foreign_root(token);
      return;
    }

    if (is_formatting(name)) {  // reconstruct what an implicit end tag closed
      start_formatting(token);
      return;
    }

    implied_end_tags(name);
    if (!contains(kNoReconstruct, name)) reconstruct_formatting();  // "any other start tag"
    const bool is_void = contains(kVoid, name);
    insert_element(token, !is_void);
    if (name == "form") form_ = current();
    if (name == "pre" || name == "listing" || name == "textarea") ignore_next_lf_ = true;
    if (name == "td" || name == "th" || name == "caption" || name == "applet" || name == "object" ||
        name == "marquee" || name == "template") {
      push_marker();
      if (name == "template") template_modes_.push_back(Mode::Template);
    }
  }

  void handle_end_tag_name(std::string_view name) {
    // "Before html" treats an end tag for html/head/body/br like "anything else":
    // the <html> element is created first, so a following comment lands inside it.
    if (!html_.valid() && (name == "html" || name == "head" || name == "body" || name == "br")) {
      ensure_html();
    }

    if (in_head_noscript() && name != "noscript" && name != "template") {
      if (name == "br") {  // "anything else": back to "in head" and reprocess
        close_open("noscript");
        handle_end_tag_name(name);
        return;
      }
      report("html.ignored_in_head_noscript", "</" + std::string(name) + "> ignored in <noscript>");
      return;
    }

    // "Before head" and "after head" ignore every end tag except these: `</p>`
    // before <body> must not conjure a paragraph.
    const dom::NodeId tag_top = current();
    if (!body_created() && (tag_top == html_ || tag_top == head_) && name != "head" && name != "body" &&
        name != "html" && name != "br") {
      report("html.ignored_before_body", "</" + std::string(name) + "> ignored before <body>");
      return;
    }

    if (in_frameset()) {
      if (name == "frameset") {
        close_open("frameset");  // body_ keeps pointing at it: this is a frameset document
        return;
      }
      if (name != "noframes" && name != "template") {
        report("html.ignored_in_frameset", "</" + std::string(name) + "> ignored inside <frameset>");
        return;
      }
    }
    if (select_in_table() && (is_table_part(name) || name == "caption")) {
      if (!in_table_scope(name)) {
        report("html.ignored_end_tag", "</" + std::string(name) + "> ignored: not in table scope");
        return;
      }
      pop_through("select");
      handle_end_tag_name(name);
      return;
    }
    // "In caption" end tags: close the caption, then reprocess.
    if (is_table_structure(name) && caption_context()) {
      if (name == "caption") {
        close_caption();
        return;
      }
      close_caption();
      handle_end_tag_name(name);
      return;
    }

    switch (mode()) {
      case Mode::Table:
        if (table_end(name)) return;
        break;
      case Mode::TableBody:
        if (table_body_end(name)) return;
        break;
      case Mode::Row:
        if (row_end(name)) return;
        break;
      case Mode::Cell:
        if (cell_end(name)) return;
        break;
      case Mode::ColumnGroup:
        if (column_group_end(name)) return;
        break;
      case Mode::Template:
        // "In template" ignores every end tag but `</template>` itself.
        if (name != "template") {
          report("html.ignored_in_template", "</" + std::string(name) + "> ignored in <template>");
          return;
        }
        break;
      case Mode::Body:
        break;
    }

    if (name == "html" || name == "body") {  // the tree is closed at EOF: just switch mode
      // `</html>` before any content creates <head>/<body> (the "before head" and
      // "after head" rules), so a following comment lands after them.
      if (!fragment_mode_ && !body_.valid()) ensure_body();
      if (body_.valid() && tag_in_scope("body")) after_body_ = true;
      // `</html>` reaches "after after body" / "after after frameset", where a
      // comment is a child of the Document instead of <html>. A fragment has no
      // document to fall out of, so there the tag is simply ignored (see
      // parse_fragment(), which folds stray Document comments back into the root).
      if (name == "html" && !fragment_mode_) after_html_ = true;
      return;
    }

    // "After frameset" ignores every end tag but html/body/noframes/template.
    if (frameset_document() && !in_frameset() && name != "noframes" && name != "template") {
      report("html.ignored_after_frameset", "</" + std::string(name) + "> ignored after </frameset>");
      return;
    }
    if (name == "head") {
      if (!fragment_mode_ && !head_.valid()) ensure_head();  // "before head" implies it
      close_head();
      return;
    }
    if (name == "br") {
      if (!body_created()) ensure_body();  // "in head" treats a stray </br> as content
      // It behaves exactly like a <br> start tag, fostering out of a table.
      const dom::NodeId element = make_element(plain_token(TokenType::StartTag, "br", true));
      insert_node(element);
      return;
    }
    if (name == "p") {
      // "If the stack does not have a p element in button scope": a <p> *inside*
      // a <button> does not count, so a new (empty) paragraph is created instead.
      if (!p_in_scope()) {
        // "Insert an HTML element for a 'p' start tag token with no attributes,
        // then close a p element" — an empty paragraph, fostered out of a table.
        const dom::NodeId element = make_element(plain_token(TokenType::StartTag, "p"));
        insert_node(element);
        stack_.push_back(element);
        close_open("p");
        report("html.stray_end_tag", "</p> without an open <p>");
      } else {
        close_open("p");
      }
      return;
    }
    if (select_in_table() && (is_table_part(name) || name == "caption")) {
      if (!in_table_scope(name)) {
        report("html.ignored_end_tag", "</" + std::string(name) + "> ignored: not in table scope");
        return;
      }
      pop_through("select");
      handle_end_tag_name(name);
      return;
    }
    if (select_context()) {
      if (name == "option") {
        if (current_is("option")) stack_.pop_back();
        return;
      }
      if (name == "optgroup") {
        if (stack_.size() >= 2 && current_is("option") && tag_of(stack_[stack_.size() - 2]) == "optgroup") {
          stack_.pop_back();
        }
        if (current_is("optgroup")) stack_.pop_back();
        return;
      }
      if (name == "hr") {
        if (!select_in_select_scope()) {  // nothing to close: drop the tag
          report("html.ignored_in_select", "</hr> ignored: not in select scope");
          return;
        }
        generate_implied_end_tags();
        pop_through("hr");
        return;
      }
      if (name == "select") {
        if (!select_in_select_scope()) {  // `</select>` with a <button> open: ignored
          report("html.ignored_in_select", "</select> ignored: not in select scope");
          return;
        }
        close_open("select");
        return;
      }
      // Every other end tag falls through: the modern content model parses the
      // select's children with the "in body" rules (`<select><div><i></div>`).
    }
    if (name == "li" || name == "dd" || name == "dt") {
      if (!tag_in_scope(name, true)) {
        report("html.ignored_end_tag", "</" + std::string(name) + "> without a matching list item");
        return;
      }
      generate_implied_end_tags(name);
      pop_through(name);
      return;
    }
    if (is_heading(name)) {
      if (!tag_in_scope("h1") && !tag_in_scope("h2") && !tag_in_scope("h3") && !tag_in_scope("h4") &&
          !tag_in_scope("h5") && !tag_in_scope("h6")) {
        report("html.ignored_end_tag", "</" + std::string(name) + "> without an open heading");
        return;
      }
      generate_implied_end_tags();
      while (!stack_.empty()) {  // pop through whichever heading is open
        const bool heading = is_heading(tag_of(stack_.back()));
        stack_.pop_back();
        if (heading) break;
      }
      return;
    }
    if (name == "applet" || name == "marquee" || name == "object") {
      // The element pushed a marker onto the active-formatting list; closing it
      // drops everything that was opened inside (`<marquee><a></marquee>` must not
      // keep reconstructing that <a>).
      if (!tag_in_scope(name)) {
        report("html.ignored_end_tag", "</" + std::string(name) + "> without a matching element");
        return;
      }
      generate_implied_end_tags();
      pop_through(name);
      clear_to_last_marker();
      return;
    }
    if (name == "button") {
      if (!tag_in_scope("button")) {
        report("html.ignored_end_tag", "</button> without an open button");
        return;
      }
      generate_implied_end_tags();
      pop_through("button");
      return;
    }
    if (name == "template") {
      // Pop everything up to and including the template element, then drop the
      // active-formatting entries that were created inside it (back to the marker).
      for (std::size_t i = stack_.size(); i > 0; --i) {
        if (is_html_named(stack_[i - 1], "template")) {  // never an <svg template>
          stack_.resize(i - 1);
          clear_to_last_marker();
          if (!template_modes_.empty()) template_modes_.pop_back();
          return;
        }
      }
      report("html.stray_end_tag", "</template> without an open <template>");
      return;
    }
    if (name == "form") {
      // "</form>" is driven by the form element pointer, not by the stack: inside a
      // table the form element is popped as soon as it is inserted, so the token
      // has to reach it through the pointer and drop it from the stack only when
      // it is still open (`</form>` inside `<table>` clears the pointer and
      // leaves the table alone).
      if (!has_open("template")) {
        const dom::NodeId node = form_;
        form_ = dom::NodeId{};
        if (!node.valid() || !in_scope(node)) {
          report("html.ignored_end_tag", "</form> without an open form");
          return;
        }
        generate_implied_end_tags();
        remove_from_stack(index_in_stack(node));  // random access: not the current node
        return;
      }
      if (!tag_in_scope("form")) {
        report("html.ignored_end_tag", "</form> ignored: no form in scope");
        return;
      }
      generate_implied_end_tags();
      pop_through("form");
      return;
    }
    if (is_formatting(name)) {
      adoption_agency(name);
      return;
    }
    generic_end_tag(name);
  }

  void finish() {
    if (!fragment_mode_) {
      if (!body_created()) ensure_body();
      if (!head_created()) ensure_head();
    }
    stack_.clear();
    update_selected_content();
  }

  /// Collects the descendants of `node` whose tag is `tag`, in tree order.
  void collect_descendants(dom::NodeId node, std::string_view tag, std::vector<dom::NodeId>& out) const {
    for (const dom::NodeId child : document_.children(node)) {
      if (document_.type(child) == dom::NodeType::Element && document_.tag_name(child) == tag) {
        out.push_back(child);
      }
      collect_descendants(child, tag, out);  // also walks into <template> contents
    }
  }

  /// Deep-copies the children of `source` under `target` (elements with their
  /// attributes, text and comments — the nodes a <selectedcontent> can hold).
  void clone_children(dom::NodeId source, dom::NodeId target) {
    for (const dom::NodeId child : document_.children(source)) {
      switch (document_.type(child)) {
        case dom::NodeType::Element: {
          const dom::NodeId copy = document_.create_element(document_.tag_name(child));
          for (const dom::Attribute& attribute : document_.attributes(child)) {
            document_.set_attribute(copy, attribute.name, attribute.value);
          }
          document_.append_child(target, copy);
          clone_children(child, copy);
          break;
        }
        case dom::NodeType::Text:
          document_.append_child(target, document_.create_text(document_.text_data(child)));
          break;
        case dom::NodeType::Comment:
          document_.append_child(target, document_.create_comment(document_.text_data(child)));
          break;
        default:
          break;
      }
    }
  }

  /// "Update a select's descendant selectedcontent element" (customizable
  /// <select>): the contents of the selected <option> are mirrored into the
  /// <selectedcontent> the parser puts inside the select's <button>, so the tree
  /// already shows what a UA would display.
  void update_selected_content() {
    std::vector<dom::NodeId> selects;
    collect_descendants(document_.root(), "select", selects);
    for (const dom::NodeId select : selects) {
      std::vector<dom::NodeId> options;
      collect_descendants(select, "option", options);
      if (options.empty()) continue;
      std::vector<dom::NodeId> contents;
      collect_descendants(select, "selectedcontent", contents);
      if (contents.empty()) continue;
      // The selected option: the last one carrying a `selected` attribute, or the
      // first option otherwise.
      dom::NodeId chosen = options.front();
      for (const dom::NodeId option : options) {
        for (const dom::Attribute& attribute : document_.attributes(option)) {
          if (attribute.name == "selected") {
            chosen = option;
            break;
          }
        }
      }
      const dom::NodeId content = contents.front();
      while (document_.first_child(content).valid()) {
        document_.remove_child(content, document_.first_child(content));
      }
      clone_children(chosen, content);
    }
  }

  dom::Document& document_;
  Diagnostics* diagnostics_ = nullptr;
  ParseOptions options_;
  bool fragment_mode_ = false;
  bool after_body_ = false;  // "after body": true once </body> or </html> was seen
  dom::NodeId form_;  // the form element pointer (null when no form is open)
  bool after_html_ = false;  // </html> seen: "after after body" / "after after frameset"
  bool doctype_seen_ = false;  // a DOCTYPE token was already accepted
  bool quirks_ = true;         // no doctype yet: quirks mode
  bool ignore_next_lf_ = false;  // <pre>/<listing>/<textarea> swallow the first newline
  dom::NodeId context_;  // fragment parsing: the element whose children we build
  Mode fragment_initial_mode_ = Mode::Body;  // "reset the insertion mode", fragment case
  dom::Namespace fragment_namespace_ = dom::Namespace::Html;  // "svg tag" / "math tag"
  std::string_view fragment_tag_;            // non-empty: run() starts the fragment algorithm
  std::unique_ptr<Tokenizer> tokenizer_;

  bool document_element_seen_ = false;
  /// Stack of "template insertion modes": one entry per open <template>. It starts
  /// as Body and switches to Table/TableBody/Row/ColumnGroup when a table-ish tag
  /// shows up inside the template, exactly like the spec's stack.
  std::vector<Mode> template_modes_;

  /// "frameset-ok": true while the body has no content that a <frameset> could not
  /// replace. `<p><frameset>` therefore still yields a frameset, `<b><frameset>`
  /// does not.
  bool frameset_ok_ = true;
  std::vector<dom::NodeId> stack_;
  std::vector<dom::NodeId> afe_;  // active formatting elements; invalid NodeId = marker
  dom::NodeId html_;
  dom::NodeId head_;
  dom::NodeId body_;
};

}  // namespace

dom::NodeId parse_fragment(std::string_view bytes, std::string_view context_tag, dom::Document& document,
                           Diagnostics* diagnostics, ParseOptions options) {
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.remove_prefix(3);
  }
  TreeBuilder builder(document, diagnostics, options);
  if (context_tag == "html") {
    // `document.documentElement.innerHTML`: the document algorithm already puts
    // <head> and <body> under <html>, and those are exactly the children the
    // fragment consists of, so no synthetic context element is needed.
    builder.run(bytes);
    const dom::NodeId root = builder.html_element();
    // A comment past `</html>` is a child of the Document in the document
    // algorithm; the fragment's result *is* the html element, so it has to move
    // back inside it to survive.
    bool after_root = false;
    std::vector<dom::NodeId> strays;
    for (const dom::NodeId child : document.children(document.root())) {
      if (child == root) {
        after_root = true;
        continue;
      }
      if (after_root && document.type(child) == dom::NodeType::Comment) strays.push_back(child);
    }
    for (const dom::NodeId stray : strays) {
      document.unlink(stray);
      document.append_child(root, stray);
    }
    return root;
  }
  builder.set_fragment_context(context_tag);
  builder.run(bytes);
  return builder.fragment_context_element();
}

Status parse_html(std::string_view bytes, dom::Document& document, Diagnostics* diagnostics, ParseOptions options) {
  // A leading UTF-8 BOM belongs to the encoding layer: drop it here so the
  // tokenizer itself stays byte-exact (which is what the conformance corpus checks).
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    bytes.remove_prefix(3);
  }
  TreeBuilder builder(document, diagnostics, options);
  builder.run(bytes);
  return Status::success();
}

Result<dom::Document> parse_document(std::string_view bytes, std::string_view source_url,
                                    Diagnostics* diagnostics) {
  dom::Document document{std::string(source_url)};
  const Status status = parse_html(bytes, document, diagnostics);
  if (status.is_error()) return Result<dom::Document>::failure(status.error());
  return Result<dom::Document>::success(std::move(document));
}

}  // namespace bonk::html
