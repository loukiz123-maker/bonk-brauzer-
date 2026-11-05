#include "bonk/dom/dom.hpp"

#include <algorithm>
#include <cassert>

#include "bonk/core/utf8.hpp"

namespace bonk::dom {
namespace {

const char* const kVoidElements[] = {"area", "base",  "basefont", "bgsound", "br",   "col",    "embed",
                                     "frame", "hr",   "img",      "input",   "keygen", "link", "meta",
                                     "param", "source", "track",   "wbr"};

bool is_void_element(std::string_view tag) noexcept {
  for (const char* name : kVoidElements) {
    if (tag == name) return true;
  }
  return false;
}

void escape_text_into(std::string_view text, std::string& out) {
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out.push_back(c);
    }
  }
}

void escape_attribute_into(std::string_view value, std::string& out) {
  for (const char c : value) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '<':
        out += "&lt;";
        break;
      default:
        out.push_back(c);
    }
  }
}

void escape_debug_into(std::string_view text, std::string& out) {
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        out.push_back(c);
    }
  }
}

}  // namespace

std::string_view node_type_name(NodeType type) noexcept {
  switch (type) {
    case NodeType::Document:
      return "document";
    case NodeType::Element:
      return "element";
    case NodeType::Text:
      return "text";
    case NodeType::Comment:
      return "comment";
    case NodeType::Doctype:
      return "doctype";
    case NodeType::Fragment:
      return "fragment";
  }
  return "unknown";
}

Document::Document() : Document(std::string()) {}

Document::Document(std::string source_url) : source_url_(std::move(source_url)) {
  (void)intern(std::string_view{});  // id 0 = empty string
  Node document;
  document.type = NodeType::Document;
  document.generation = 1;
  nodes_.push_back(document);
}

NodeId Document::id_for(std::uint32_t index) const {
  if (index == kNoNode || index >= nodes_.size()) return NodeId{};
  return NodeId{index, nodes_[index].generation};
}

const Document::Node& Document::node_ref(NodeId id) const {
  assert(id.index < nodes_.size() && nodes_[id.index].alive);
  return nodes_[id.index];
}

Document::Node& Document::node_ref(NodeId id) {
  assert(id.index < nodes_.size() && nodes_[id.index].alive);
  return nodes_[id.index];
}

bool Document::is_alive(NodeId id) const noexcept {
  return id.index < nodes_.size() && nodes_[id.index].alive && nodes_[id.index].generation == id.generation;
}

std::uint32_t Document::intern(std::string_view value) {
  const auto it = string_ids_.find(std::string(value));
  if (it != string_ids_.end()) return it->second;
  const auto id = static_cast<std::uint32_t>(strings_.size());
  strings_.emplace_back(value);
  string_ids_.emplace(strings_.back(), id);
  return id;
}

std::string_view Document::string_of(std::uint32_t id) const {
  return id < strings_.size() ? std::string_view(strings_[id]) : std::string_view{};
}

NodeId Document::create_element(std::string_view name) {
  Node node;
  node.type = NodeType::Element;
  node.generation = 1;
  node.name = intern(name);
  const auto index = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(node);
  return id_for(index);
}

NodeId Document::create_fragment() {
  Node node;
  node.type = NodeType::Fragment;
  node.generation = 1;
  const auto index = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(node);
  return id_for(index);
}

NodeId Document::create_text(std::string_view text) {
  Node node;
  node.type = NodeType::Text;
  node.generation = 1;
  node.text_offset = static_cast<std::uint32_t>(text_pool_.size());
  node.text_length = static_cast<std::uint32_t>(text.size());
  text_pool_.append(text);
  const auto index = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(node);
  return id_for(index);
}

NodeId Document::create_comment(std::string_view text) {
  NodeId id = create_text(text);
  node_ref(id).type = NodeType::Comment;
  return id;
}

NodeId Document::create_doctype(std::string_view text) {
  NodeId id = create_text(text);
  node_ref(id).type = NodeType::Doctype;
  return id;
}

NodeId Document::create_doctype(std::string_view text, std::string_view public_id, std::string_view system_id) {
  NodeId id = create_doctype(text);
  node_ref(id).public_id = intern(public_id);  // even when empty: the identifier was there
  node_ref(id).system_id = intern(system_id);
  return id;
}

bool Document::has_doctype_identifiers(NodeId id) const {
  const Node& node = node_ref(id);
  return node.public_id != kNoNode || node.system_id != kNoNode;
}

std::string_view Document::doctype_public_id(NodeId id) const {
  const Node& node = node_ref(id);
  return node.public_id == kNoNode ? std::string_view{} : string_of(node.public_id);
}

std::string_view Document::doctype_system_id(NodeId id) const {
  const Node& node = node_ref(id);
  return node.system_id == kNoNode ? std::string_view{} : string_of(node.system_id);
}

void Document::detach(NodeId child) {
  Node& node = node_ref(child);
  if (node.parent == kNoNode) return;
  Node& parent = node_ref(id_for(node.parent));
  if (node.prev_sibling != kNoNode) {
    node_ref(id_for(node.prev_sibling)).next_sibling = node.next_sibling;
  } else {
    parent.first_child = node.next_sibling;
  }
  if (node.next_sibling != kNoNode) {
    node_ref(id_for(node.next_sibling)).prev_sibling = node.prev_sibling;
  } else {
    parent.last_child = node.prev_sibling;
  }
  node.parent = kNoNode;
  node.next_sibling = kNoNode;
  node.prev_sibling = kNoNode;
}

void Document::append_child(NodeId parent_id, NodeId child_id) {
  Node& child = node_ref(child_id);
  if (child.parent != kNoNode) detach(child_id);
  Node& parent = node_ref(parent_id);

  child.parent = parent_id.index;
  child.next_sibling = kNoNode;
  child.prev_sibling = parent.last_child;
  if (parent.last_child != kNoNode) {
    node_ref(id_for(parent.last_child)).next_sibling = child_id.index;
  } else {
    parent.first_child = child_id.index;
  }
  parent.last_child = child_id.index;
}

void Document::insert_before(NodeId parent_id, NodeId child_id, NodeId reference) {
  if (!reference.valid()) {
    append_child(parent_id, child_id);
    return;
  }
  Node& child = node_ref(child_id);
  if (child.parent != kNoNode) detach(child_id);
  Node& parent = node_ref(parent_id);
  const Node& ref = node_ref(reference);

  child.parent = parent_id.index;
  child.next_sibling = reference.index;
  child.prev_sibling = ref.prev_sibling;
  if (ref.prev_sibling != kNoNode) {
    node_ref(id_for(ref.prev_sibling)).next_sibling = child_id.index;
  } else {
    parent.first_child = child_id.index;
  }
  node_ref(reference).prev_sibling = child_id.index;
}

void Document::unlink(NodeId child_id) { detach(child_id); }

void Document::remove_child(NodeId parent_id, NodeId child_id) {
  Node& child = node_ref(child_id);
  if (child.parent != parent_id.index) return;
  detach(child_id);
  child.alive = false;
  child.generation += 1;  // invalidates every outstanding NodeId
}

void Document::set_attribute(NodeId element, std::string_view name, std::string_view value) {
  Node& node = node_ref(element);
  for (std::uint32_t i = node.attr_head; i != kNoNode; i = attrs_[i].next) {
    if (string_of(attrs_[i].name) == name) {
      attrs_[i].value = intern(value);
      return;
    }
  }
  const auto record = static_cast<std::uint32_t>(attrs_.size());
  attrs_.push_back(AttrRecord{intern(name), intern(value), node.attr_head});
  node.attr_head = record;
}

void Document::remove_attribute(NodeId id, std::string_view name) {
  if (!id.valid()) return;
  Node& node = node_ref(id);
  std::uint32_t previous = kNoNode;
  for (std::uint32_t i = node.attr_head; i != kNoNode; i = attrs_[i].next) {
    if (string_of(attrs_[i].name) != name) {
      previous = i;
      continue;
    }
    if (previous == kNoNode) {
      node.attr_head = attrs_[i].next;
    } else {
      attrs_[previous].next = attrs_[i].next;
    }
    return;
  }
}

void Document::append_text(NodeId parent_id, std::string_view text) {
  if (text.empty()) return;
  Node& parent = node_ref(parent_id);
  if (parent.last_child != kNoNode) {
    Node& last = node_ref(id_for(parent.last_child));
    if (last.type == NodeType::Text && last.text_offset + last.text_length == text_pool_.size()) {
      text_pool_.append(text);
      last.text_length += static_cast<std::uint32_t>(text.size());
      return;
    }
  }
  append_child(parent_id, create_text(text));
}

void Document::extend_text(NodeId text_node, std::string_view text) {
  Node& node = node_ref(text_node);
  if (node.type != NodeType::Text || text.empty()) return;
  std::string combined(text_data(text_node));
  combined.append(text);
  node.text_offset = static_cast<std::uint32_t>(text_pool_.size());
  node.text_length = static_cast<std::uint32_t>(combined.size());
  text_pool_.append(combined);
}

NodeType Document::type(NodeId id) const { return node_ref(id).type; }

std::string_view Document::tag_name(NodeId id) const {
  const Node& node = node_ref(id);
  return node.type == NodeType::Element ? string_of(node.name) : std::string_view{};
}

std::string_view Document::text_data(NodeId id) const {
  const Node& node = node_ref(id);
  if (node.type != NodeType::Text && node.type != NodeType::Comment && node.type != NodeType::Doctype) {
    return std::string_view{};
  }
  return std::string_view(text_pool_).substr(node.text_offset, node.text_length);
}

std::span<const Attribute> Document::attributes(NodeId id) const {
  attribute_view_.clear();
  for (std::uint32_t i = node_ref(id).attr_head; i != kNoNode; i = attrs_[i].next) {
    attribute_view_.push_back(Attribute{string_of(attrs_[i].name), string_of(attrs_[i].value)});
  }
  std::reverse(attribute_view_.begin(), attribute_view_.end());  // preserve source order
  return attribute_view_;
}

std::string_view Document::attribute(NodeId id, std::string_view name) const {
  for (std::uint32_t i = node_ref(id).attr_head; i != kNoNode; i = attrs_[i].next) {
    if (string_of(attrs_[i].name) == name) return string_of(attrs_[i].value);
  }
  return std::string_view{};
}

bool Document::has_attribute(NodeId id, std::string_view name) const {
  if (!id.valid()) return false;
  for (std::uint32_t i = node_ref(id).attr_head; i != kNoNode; i = attrs_[i].next) {
    if (string_of(attrs_[i].name) == name) return true;
  }
  return false;
}

bool Document::has_class(NodeId id, std::string_view class_name) const {
  const std::string_view classes = attribute(id, "class");
  std::size_t start = 0;
  while (start <= classes.size()) {
    const std::size_t space = classes.find(' ', start);
    const std::string_view token =
        classes.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
    if (!token.empty() && token == class_name) return true;
    if (space == std::string_view::npos) break;
    start = space + 1;
  }
  return false;
}

NodeId Document::parent(NodeId id) const { return id_for(node_ref(id).parent); }

NodeId Document::first_child(NodeId id) const { return id_for(node_ref(id).first_child); }

NodeId Document::next_sibling(NodeId id) const { return id_for(node_ref(id).next_sibling); }

std::vector<NodeId> Document::children(NodeId id) const {
  std::vector<NodeId> out;
  for (std::uint32_t i = node_ref(id).first_child; i != kNoNode; i = nodes_[i].next_sibling) {
    out.push_back(id_for(i));
  }
  return out;
}

std::vector<NodeId> Document::descendants(NodeId id) const {
  std::vector<NodeId> out;
  std::vector<NodeId> stack;
  for (std::uint32_t i = node_ref(id).last_child; i != kNoNode; i = nodes_[i].prev_sibling) {
    stack.push_back(id_for(i));
  }
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    out.push_back(current);
    std::vector<NodeId> kids = children(current);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

std::string Document::text_content(NodeId id) const {
  const Node& node = node_ref(id);
  if (node.type == NodeType::Text) return std::string(text_data(id));
  std::string out;
  for (const NodeId descendant : descendants(id)) {
    if (nodes_[descendant.index].type == NodeType::Text) out.append(text_data(descendant));
  }
  return out;
}

NodeId Document::document_element() const {
  const std::vector<NodeId> kids = children(root());
  for (const NodeId kid : kids) {
    if (nodes_[kid.index].type == NodeType::Element) return kid;
  }
  return NodeId{};
}

NodeId Document::body() const {
  const NodeId html = document_element();
  if (!html.valid()) return NodeId{};
  for (const NodeId kid : children(html)) {
    if (tag_name(kid) == "body") return kid;
  }
  return NodeId{};
}

NodeId Document::get_element_by_id(std::string_view wanted) const {
  for (const NodeId kid : descendants(root())) {
    if (nodes_[kid.index].type == NodeType::Element && attribute(kid, "id") == wanted) return kid;
  }
  return NodeId{};
}

std::vector<NodeId> Document::get_elements_by_tag_name(std::string_view tag) const {
  std::vector<NodeId> out;
  for (const NodeId kid : descendants(root())) {
    if (nodes_[kid.index].type != NodeType::Element) continue;
    if (tag == "*" || tag_name(kid) == tag) out.push_back(kid);
  }
  return out;
}

std::vector<NodeId> Document::query_selector_all(NodeId root_id, std::string_view selector) const {
  std::vector<NodeId> out;
  const Result<std::vector<ComplexSelector>> parsed = parse_selector_list(selector);
  if (parsed.is_error()) return out;  // invalid selector: no matches, like a throwing browser API would be caught
  for (const NodeId kid : descendants(root_id)) {
    if (nodes_[kid.index].type != NodeType::Element) continue;
    if (matches_any(*this, kid, parsed.value())) out.push_back(kid);
  }
  return out;
}

NodeId Document::query_selector(NodeId root_id, std::string_view selector) const {
  const std::vector<NodeId> all = query_selector_all(root_id, selector);
  return all.empty() ? NodeId{} : all.front();
}

void Document::serialize_into(NodeId id, std::string& out) const {
  const Node& node = node_ref(id);
  switch (node.type) {
    case NodeType::Document:
    case NodeType::Fragment:  // a fragment serialises as its children
      for (const NodeId kid : children(id)) serialize_into(kid, out);
      return;
    case NodeType::Doctype:
      out += "<!DOCTYPE ";
      out += text_data(id);
      if (has_doctype_identifiers(id)) {
        out += " \"";
        out += doctype_public_id(id);
        out += "\" \"";
        out += doctype_system_id(id);
        out += '\"';
      }
      out += ">";
      return;
    case NodeType::Comment:
      out += "<!--";
      out += text_data(id);
      out += "-->";
      return;
    case NodeType::Text:
      escape_text_into(text_data(id), out);
      return;
    case NodeType::Element:
      break;
  }

  const std::string_view tag = tag_name(id);
  out += '<';
  out += tag;
  for (const Attribute& attribute : attributes(id)) {
    out += ' ';
    out += attribute.name;
    out += "=\"";
    escape_attribute_into(attribute.value, out);
    out += '"';
  }
  out += '>';
  if (is_void_element(tag)) return;
  for (const NodeId kid : children(id)) serialize_into(kid, out);
  out += "</";
  out += tag;
  out += '>';
}

std::string Document::to_html(NodeId id) const {
  std::string out;
  serialize_into(id, out);
  return out;
}

void Document::debug_into(NodeId id, int depth, std::string& out) const {
  out.append(static_cast<std::size_t>(depth) * 2, ' ');
  const Node& node = node_ref(id);
  switch (node.type) {
    case NodeType::Document:
      out += "#document";
      break;
    case NodeType::Fragment:
      out += "#fragment";
      break;
    case NodeType::Doctype:
      out += "#doctype \"";
      out += text_data(id);
      out += '"';
      break;
    case NodeType::Comment:
      out += "#comment \"";
      escape_debug_into(text_data(id), out);
      out += '"';
      break;
    case NodeType::Text:
      out += "#text \"";
      escape_debug_into(text_data(id), out);
      out += '"';
      break;
    case NodeType::Element: {
      out += '<';
      out += tag_name(id);
      for (const Attribute& attribute : attributes(id)) {
        out += ' ';
        out += attribute.name;
        out += "=\"";
        escape_debug_into(attribute.value, out);
        out += '"';
      }
      out += '>';
      break;
    }
  }
  out += '\n';
  for (const NodeId kid : children(id)) debug_into(kid, depth + 1, out);
}

std::string Document::debug_tree(NodeId id) const {
  std::string out;
  debug_into(id, 0, out);
  return out;
}

}  // namespace bonk::dom
