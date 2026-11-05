// Bonk Brauzer — document object model.
//
// Design: nodes are POD records in document-owned pools, addressed by 32-bit
// indices and linked through parent/child/sibling indices. Consequences:
//   * a node never moves in memory (stable for the lifetime of the document);
//   * building and destroying a tree is a handful of vector clears (no recursion,
//     no per-node allocation, no shared_ptr cycles);
//   * external references (JS bindings, devtools, selection) use NodeId, which
//     carries a generation so a stale id is detected instead of being a UAF.
// Everything that needs a destructor (attribute list, listener list) lives in
// side tables keyed by node index, never inside the node.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bonk/dom/selector.hpp"

namespace bonk::dom {

inline constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

enum class NodeType : std::uint8_t {
  Document,
  Element,
  Text,
  Comment,
  Doctype,
  /// A document fragment. Today only used for `<template>` contents, where the
  /// spec keeps the parsed children *outside* the element itself.
  Fragment,
};

[[nodiscard]] std::string_view node_type_name(NodeType type) noexcept;

/// A stable, generational reference to a node.
struct NodeId {
  std::uint32_t index = kNoNode;
  std::uint32_t generation = 0;

  [[nodiscard]] bool valid() const noexcept { return index != kNoNode; }
  friend bool operator==(const NodeId& a, const NodeId& b) noexcept {
    return a.index == b.index && a.generation == b.generation;
  }
  friend bool operator!=(const NodeId& a, const NodeId& b) noexcept { return !(a == b); }
};

/// The namespaces the HTML parser can produce. `<svg>` and `<math>` switch their
/// subtree into one of the foreign namespaces; everything else is HTML.
enum class Namespace : std::uint8_t { Html, Svg, Mathml };

[[nodiscard]] std::string_view namespace_name(Namespace ns) noexcept;

struct Attribute {
  std::string_view name;    // local name (case-adjusted inside SVG/MathML)
  std::string_view value;
  std::string_view prefix;  // `xlink` / `xml` / `xmlns` for foreign attributes, else empty
};

class Document {
 public:
  Document();
  explicit Document(std::string source_url);

  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;
  Document(Document&&) = default;
  Document& operator=(Document&&) = default;

  // --- construction (driven by the html tree builder) --------------------
  [[nodiscard]] NodeId root() const noexcept { return NodeId{0, nodes_[0].generation}; }
  [[nodiscard]] NodeId create_element(std::string_view name);
  [[nodiscard]] NodeId create_element(std::string_view name, Namespace ns);
  [[nodiscard]] NodeId create_text(std::string_view text);
  [[nodiscard]] NodeId create_comment(std::string_view text);
  [[nodiscard]] NodeId create_doctype(std::string_view text);
  /// The identifiers are optional as a pair: a doctype without PUBLIC/SYSTEM keeps
  /// both absent, which `has_doctype_identifiers()` reports (the tree serialiser
  /// then prints `<!DOCTYPE html>` instead of `<!DOCTYPE html "" "">`).
  [[nodiscard]] NodeId create_doctype(std::string_view text, std::string_view public_id, std::string_view system_id);
  [[nodiscard]] NodeId create_fragment();

  void append_child(NodeId parent, NodeId child);
  void insert_before(NodeId parent, NodeId child, NodeId reference);
  void remove_child(NodeId parent, NodeId child);  // unlinks *and* destroys the subtree
  /// Detaches `child` from its parent but keeps it (and its subtree) alive, so it
  /// can be re-attached elsewhere. This is what a tree "move" needs; `remove_child`
  /// on the other hand invalidates every NodeId that pointed at the removed node.
  void unlink(NodeId child);
  void set_attribute(NodeId element, std::string_view name, std::string_view value);
  /// Foreign attributes keep their namespace prefix (`xlink:href`, `xml:lang`).
  void set_attribute(NodeId element, std::string_view prefix, std::string_view name, std::string_view value);
  void append_text(NodeId parent, std::string_view text);  // merges with a trailing Text node
  /// Appends to the data of an existing Text node (foster parenting needs this:
  /// text moved in front of a table must join the text node already there).
  void extend_text(NodeId text_node, std::string_view text);

  // --- accessors ---------------------------------------------------------
  [[nodiscard]] bool is_alive(NodeId id) const noexcept;
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] NodeType type(NodeId id) const;
  [[nodiscard]] std::string_view tag_name(NodeId id) const;  // lowercase, interned
  [[nodiscard]] Namespace element_namespace(NodeId id) const;
  /// Attributes are addressed by prefix *and* local name; `attribute()` only sees
  /// the unprefixed ones.
  [[nodiscard]] std::string_view attribute_ns(NodeId id, std::string_view prefix, std::string_view name) const;
  [[nodiscard]] std::string_view text_data(NodeId id) const;
  [[nodiscard]] bool has_doctype_identifiers(NodeId id) const;
  [[nodiscard]] std::string_view doctype_public_id(NodeId id) const;
  [[nodiscard]] std::string_view doctype_system_id(NodeId id) const;
  [[nodiscard]] std::span<const Attribute> attributes(NodeId id) const;
  [[nodiscard]] std::string_view attribute(NodeId id, std::string_view name) const;
  /// True when the attribute is present even with an empty value (`<input checked>`).
  [[nodiscard]] bool has_attribute(NodeId id, std::string_view name) const;
  /// Removes `name` from the element's attribute list (no-op when absent).
  void remove_attribute(NodeId id, std::string_view name);
  [[nodiscard]] bool has_class(NodeId id, std::string_view class_name) const;

  [[nodiscard]] NodeId parent(NodeId id) const;
  [[nodiscard]] NodeId first_child(NodeId id) const;
  [[nodiscard]] NodeId next_sibling(NodeId id) const;
  [[nodiscard]] std::vector<NodeId> children(NodeId id) const;
  [[nodiscard]] std::vector<NodeId> descendants(NodeId id) const;  // depth-first, excluding self

  [[nodiscard]] std::string text_content(NodeId id) const;
  [[nodiscard]] NodeId document_element() const;
  [[nodiscard]] NodeId body() const;
  [[nodiscard]] NodeId get_element_by_id(std::string_view id) const;
  [[nodiscard]] std::vector<NodeId> get_elements_by_tag_name(std::string_view tag) const;

  // --- queries -----------------------------------------------------------
  [[nodiscard]] std::vector<NodeId> query_selector_all(NodeId root, std::string_view selector) const;
  [[nodiscard]] NodeId query_selector(NodeId root, std::string_view selector) const;

  // --- serialization -----------------------------------------------------
  [[nodiscard]] std::string to_html(NodeId root) const;
  /// Indented outline used by `bonk --dump-dom`.
  [[nodiscard]] std::string debug_tree(NodeId root) const;

  [[nodiscard]] const std::string& source_url() const noexcept { return source_url_; }

 private:
  struct Node {
    NodeType type = NodeType::Document;
    std::uint32_t generation = 0;
    std::uint32_t parent = kNoNode;
    std::uint32_t first_child = kNoNode;
    std::uint32_t last_child = kNoNode;
    std::uint32_t next_sibling = kNoNode;
    std::uint32_t prev_sibling = kNoNode;
    std::uint32_t name = 0;       // interned string id (element tag / doctype text)
    Namespace ns = Namespace::Html;  // elements only
    std::uint32_t public_id = kNoNode;  // DOCTYPE only: interned, kNoNode when absent
    std::uint32_t system_id = kNoNode;  // DOCTYPE only: interned, kNoNode when absent
    std::uint32_t attr_head = kNoNode;
    std::uint32_t text_offset = 0;
    std::uint32_t text_length = 0;
    bool alive = true;
  };

  struct AttrRecord {
    std::uint32_t name = 0;
    std::uint32_t value = 0;
    std::uint32_t prefix = kNoNode;  // interned, kNoNode for ordinary attributes
    std::uint32_t next = kNoNode;
  };

  [[nodiscard]] NodeId id_for(std::uint32_t index) const;
  [[nodiscard]] const Node& node_ref(NodeId id) const;  // asserts alive
  [[nodiscard]] Node& node_ref(NodeId id);
  [[nodiscard]] std::uint32_t intern(std::string_view value);
  [[nodiscard]] std::string_view string_of(std::uint32_t id) const;
  void detach(NodeId child);
  void serialize_into(NodeId id, std::string& out) const;
  void debug_into(NodeId id, int depth, std::string& out) const;

  std::vector<Node> nodes_;
  std::vector<AttrRecord> attrs_;
  std::string text_pool_;
  std::vector<std::string> strings_;
  std::unordered_map<std::string, std::uint32_t> string_ids_;
  std::uint32_t scratch_attr_begin_ = 0;  // staging area for attributes() span
  mutable std::vector<Attribute> attribute_view_;
  std::string source_url_;
};

}  // namespace bonk::dom
