// JS <-> DOM bridge: window, document, element wrappers, events, inline scripts.
#include "bonk/bindings/bindings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bonk/core/version.hpp"
#include "bonk/html/html.hpp"
#include "bonk/net/url.hpp"

namespace bonk::bindings {
namespace {

using dom::Document;
using dom::NodeId;
using dom::NodeType;
using js::Function;
using js::Object;
using js::Property;
using js::Value;

std::uint64_t node_key(NodeId id) noexcept {
  return (static_cast<std::uint64_t>(id.generation) << 32) | static_cast<std::uint64_t>(id.index);
}

std::string ascii_upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string camel_to_kebab(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back('-');
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

/// Splits "color: red; font-size: 12px" into pairs (trimmed).
std::vector<std::pair<std::string, std::string>> parse_inline_style(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> declarations;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(';', start);
    const std::string_view chunk = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const std::size_t colon = chunk.find(':');
    if (colon != std::string_view::npos) {
      auto trim = [](std::string_view value) {
        const std::size_t begin = value.find_first_not_of(" \t\n\r");
        if (begin == std::string_view::npos) return std::string();
        const std::size_t last = value.find_last_not_of(" \t\n\r");
        return std::string(value.substr(begin, last - begin + 1));
      };
      declarations.emplace_back(ascii_lower(trim(chunk.substr(0, colon))), trim(chunk.substr(colon + 1)));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return declarations;
}

std::string serialize_inline_style(const std::vector<std::pair<std::string, std::string>>& declarations) {
  std::string out;
  for (const auto& [name, value] : declarations) {
    if (value.empty()) continue;
    if (!out.empty()) out += "; ";
    out += name;
    out += ": ";
    out += value;
  }
  return out;
}

const char* const kStyleProperties[] = {
    "color",      "background",       "backgroundColor", "border",        "borderColor", "borderWidth",
    "display",    "width",            "height",          "minWidth",      "minHeight",   "maxWidth",
    "margin",     "marginTop",        "marginRight",     "marginBottom",  "marginLeft",  "padding",
    "paddingTop", "paddingRight",     "paddingBottom",   "paddingLeft",   "fontSize",    "fontWeight",
    "fontFamily", "textAlign",        "textDecoration",  "lineHeight",    "opacity",     "position",
    "top",        "right",            "bottom",          "left",          "zIndex",      "float",
    "visibility", "overflow",         "transform",       "cursor",        "letterSpacing"};
}  // namespace

struct Bindings::Impl {
  js::Context& context;
  Document& document;

  std::unordered_map<std::uint64_t, Value> wrappers;
  std::unordered_map<std::uint64_t, std::vector<std::pair<std::string, Value>>> listeners;
  std::vector<ScriptRun> runs;
  Value window;
  Value document_object;
  Value location_object;
  Value console_object;

  Impl(js::Context& context_in, Document& document_in) : context(context_in), document(document_in) {}

  // --- wrapper plumbing ---------------------------------------------------
  Value wrap(NodeId node);
  bool node_of(const Value& value, NodeId& out) const;
  Value make_native(std::string name, Function::Native native) {
    auto function = std::make_shared<Function>();
    function->name = std::move(name);
    function->native = std::move(native);
    return Value::function_of(std::move(function));
  }
  void method(const std::shared_ptr<Object>& object, const std::string& name, Function::Native native) {
    object->properties[name] = Property{make_native(name, std::move(native))};
  }
  void getter(const std::shared_ptr<Object>& object, const std::string& name, Object::Getter getter_fn) {
    object->getters[name] = std::move(getter_fn);
  }
  void setter(const std::shared_ptr<Object>& object, const std::string& name, Object::Setter setter_fn) {
    object->setters[name] = std::move(setter_fn);
  }

  // --- DOM helpers --------------------------------------------------------
  std::string inner_html(NodeId node) const;
  void set_inner_html(NodeId node, const std::string& html);
  void set_text_content(NodeId node, const std::string& text);
  std::vector<NodeId> element_list(const std::vector<NodeId>& nodes) const;
  std::string resolved_url(std::string_view attribute) const;
  void copy_into(Document& source, NodeId source_node, NodeId target_parent);
  Value make_style_object(NodeId node);
  Value make_event(const std::string& type, NodeId target, NodeId current, Value detail);
  bool dispatch(NodeId target, const std::string& type, Value detail);

  // --- objects ------------------------------------------------------------
  void install_window();
  void install_document();
  void install_element(const std::shared_ptr<Object>& object, NodeId node);
  void install_document_node(const std::shared_ptr<Object>& object, NodeId node);
};

// ------------------------------------------------------------- wrappers -----

Value Bindings::Impl::wrap(NodeId node) {
  if (!node.valid() || !document.is_alive(node)) return Value::null();
  const std::uint64_t key = node_key(node);
  const auto found = wrappers.find(key);
  if (found != wrappers.end()) return found->second;

  auto object = std::make_shared<Object>();
  object->properties["__nodeIndex"] = Property{Value::number_of(static_cast<double>(node.index))};
  object->properties["__nodeGeneration"] = Property{Value::number_of(static_cast<double>(node.generation))};

  const NodeType type = document.type(node);
  if (type == NodeType::Element) {
    install_element(object, node);
  } else if (type == NodeType::Text || type == NodeType::Comment) {
    install_document_node(object, node);
    getter(object, "nodeType", [&document = document, node]() {
      return Value::number_of(document.type(node) == NodeType::Text ? 3.0 : 8.0);
    });
    getter(object, "nodeName", [&document = document, node]() {
      return Value::string_of(std::string(document.type(node) == NodeType::Text ? "#text" : "#comment"));
    });
    getter(object, "data", [&document = document, node]() { return Value::string_of(std::string(document.text_data(node))); });
    getter(object, "textContent", [&document = document, node]() {
      return Value::string_of(std::string(document.text_data(node)));
    });
    setter(object, "textContent", [&document = document, node](const Value& value) {
      if (document.type(node) == NodeType::Text) document.append_text(node, js::to_string(value));
    });
  } else {
    install_document_node(object, node);
    getter(object, "nodeType", []() { return Value::number_of(9.0); });
    getter(object, "nodeName", []() { return Value::string_of("#document"); });
  }

  const Value wrapper = Value::object_of(object);
  wrappers[key] = wrapper;
  return wrapper;
}

bool Bindings::Impl::node_of(const Value& value, NodeId& out) const {
  if (value.type != js::ValueType::Object || value.object == nullptr) return false;
  const auto index = value.object->properties.find("__nodeIndex");
  const auto generation = value.object->properties.find("__nodeGeneration");
  if (index == value.object->properties.end() || generation == value.object->properties.end()) return false;
  if (!index->second.value.is_number() || !generation->second.value.is_number()) return false;
  NodeId id;
  id.index = static_cast<std::uint32_t>(index->second.value.number);
  id.generation = static_cast<std::uint32_t>(generation->second.value.number);
  if (!document.is_alive(id)) return false;
  out = id;
  return true;
}

std::string Bindings::Impl::inner_html(NodeId node) const {
  std::string out;
  NodeId child = document.first_child(node);
  while (child.valid()) {
    out += document.to_html(child);
    child = document.next_sibling(child);
  }
  return out;
}

void Bindings::Impl::copy_into(Document& source, NodeId source_node, NodeId target_parent) {
  switch (source.type(source_node)) {
    case NodeType::Element: {
      const NodeId created = document.create_element(source.tag_name(source_node));
      for (const dom::Attribute& attribute : source.attributes(source_node)) {
        document.set_attribute(created, attribute.name, attribute.value);
      }
      document.append_child(target_parent, created);
      NodeId child = source.first_child(source_node);
      while (child.valid()) {
        copy_into(source, child, created);
        child = source.next_sibling(child);
      }
      break;
    }
    case NodeType::Text:
      document.append_text(target_parent, source.text_data(source_node));
      break;
    case NodeType::Comment: {
      const NodeId comment = document.create_comment(source.text_data(source_node));
      document.append_child(target_parent, comment);
      break;
    }
    default:
      break;
  }
}

void Bindings::Impl::set_inner_html(NodeId node, const std::string& html) {
  if (node.valid() == false) return;
  // Wipe existing children.
  while (document.first_child(node).valid()) {
    document.remove_child(node, document.first_child(node));
  }
  if (html.empty()) return;
  // Fragment parsing reuses the real HTML parser: parse into a scratch document
  // and deep-copy the body children into the target.
  Result<Document> parsed = html::parse_document("<body>" + html + "</body>", document.source_url());
  if (parsed.is_error()) return;
  auto scratch = std::make_shared<Document>(std::move(parsed.value()));
  const NodeId body = scratch->body();
  if (!body.valid()) return;
  NodeId child = scratch->first_child(body);
  while (child.valid()) {
    copy_into(*scratch, child, node);
    child = scratch->next_sibling(child);
  }
}

void Bindings::Impl::set_text_content(NodeId node, const std::string& text) {
  while (document.first_child(node).valid()) document.remove_child(node, document.first_child(node));
  if (!text.empty()) document.append_text(node, text);
}

std::vector<NodeId> Bindings::Impl::element_list(const std::vector<NodeId>& nodes) const {
  std::vector<NodeId> out;
  for (const NodeId node : nodes) {
    if (document.type(node) == NodeType::Element) out.push_back(node);
  }
  return out;
}

std::string Bindings::Impl::resolved_url(std::string_view attribute) const {
  if (attribute.empty()) return std::string(attribute);
  if (attribute.find("://") != std::string_view::npos) return std::string(attribute);
  const Result<net::Url> base = net::parse_url(document.source_url());
  if (base.is_error()) return std::string(attribute);
  const Result<net::Url> resolved = net::parse_url(std::string(attribute), &base.value());
  if (resolved.is_error()) return std::string(attribute);
  return resolved.value().serialize();
}

// --------------------------------------------------------------- styles -----

Value Bindings::Impl::make_style_object(NodeId node) {
  auto style = std::make_shared<Object>();
  auto read = [this, node](const std::string& name) {
    for (const auto& [key, value] : parse_inline_style(document.attribute(node, "style"))) {
      if (key == name) return value;
    }
    return std::string();
  };
  auto write = [this, node](const std::string& name, const std::string& value) {
    auto declarations = parse_inline_style(document.attribute(node, "style"));
    bool replaced = false;
    for (auto& entry : declarations) {
      if (entry.first == name) {
        entry.second = value;
        replaced = true;
      }
    }
    if (!replaced) declarations.emplace_back(name, value);
    document.set_attribute(node, "style", serialize_inline_style(declarations));
  };

  for (const char* property : kStyleProperties) {
    const std::string camel(property);
    const std::string kebab = camel_to_kebab(camel);
    style->getters[camel] = [read, kebab]() { return Value::string_of(read(kebab)); };
    style->setters[camel] = [write, kebab](const Value& value) { write(kebab, js::to_string(value)); };
  }
  style->properties["length"] = Property{Value::number_of(static_cast<double>(sizeof(kStyleProperties) / sizeof(kStyleProperties[0])))};
  style->getters["cssText"] = [this, node]() { return Value::string_of(std::string(document.attribute(node, "style"))); };
  style->setters["cssText"] = [this, node](const Value& value) {
    document.set_attribute(node, "style", js::to_string(value));
  };
  method(style, "setProperty", [write](const std::vector<Value>& arguments, Value) {
    if (arguments.size() >= 2) write(js::to_string(arguments[0]), js::to_string(arguments[1]));
    return Value::undefined();
  });
  method(style, "getPropertyValue", [read](const std::vector<Value>& arguments, Value) {
    return Value::string_of(arguments.empty() ? std::string() : read(js::to_string(arguments[0])));
  });
  method(style, "removeProperty", [write](const std::vector<Value>& arguments, Value) {
    if (!arguments.empty()) write(js::to_string(arguments[0]), std::string());
    return Value::undefined();
  });
  return Value::object_of(style);
}

// --------------------------------------------------------------- events -----

Value Bindings::Impl::make_event(const std::string& type, NodeId target, NodeId current, Value detail) {
  auto event = std::make_shared<Object>();
  event->properties["type"] = Property{Value::string_of(type)};
  event->properties["bubbles"] = Property{Value::boolean_of(true)};
  event->properties["defaultPrevented"] = Property{Value::boolean_of(false)};
  event->properties["detail"] = Property{std::move(detail)};
  event->properties["target"] = Property{wrap(target)};
  event->properties["srcElement"] = Property{wrap(target)};
  event->properties["currentTarget"] = Property{wrap(current)};

  auto state = std::make_shared<bool>(false);
  event->properties["preventDefault"] = Property{make_native("preventDefault", [state](const std::vector<Value>&, Value) {
                                                    *state = true;
                                                    return Value::undefined();
                                                  })};
  event->properties["stopPropagation"] =
      Property{make_native("stopPropagation", [](const std::vector<Value>&, Value) { return Value::undefined(); })};
  // The flag is mirrored into a shared cell so the dispatcher can read it back.
  event->properties["__prevented"] =
      Property{make_native("__prevented", [state](const std::vector<Value>&, Value) {
                 return Value::boolean_of(*state);
               })};
  return Value::object_of(event);
}

bool Bindings::Impl::dispatch(NodeId target, const std::string& type, Value detail) {
  if (!target.valid()) return false;
  bool prevented = false;
  NodeId current = target;
  while (current.valid()) {
    const Value event = make_event(type, target, current, detail);
    const auto found = listeners.find(node_key(current));
    if (found != listeners.end()) {
      for (const auto& [registered_type, handler] : found->second) {
        if (registered_type != type) continue;
        std::vector<Value> arguments{event};
        (void)context.runtime().call(handler, arguments);
        const Value flag = context.runtime().call(
            event.object->properties.at("__prevented").value, {});
        if (js::to_boolean(flag)) prevented = true;
      }
    }
    current = document.parent(current);
  }
  // Document-level listeners last (registered on the root node).
  const auto root_listeners = listeners.find(node_key(document.root()));
  if (root_listeners != listeners.end()) {
    const Value event = make_event(type, target, document.root(), detail);
    for (const auto& [registered_type, handler] : root_listeners->second) {
      if (registered_type != type) continue;
      std::vector<Value> arguments{event};
      (void)context.runtime().call(handler, arguments);
      const Value flag = context.runtime().call(event.object->properties.at("__prevented").value, {});
      if (js::to_boolean(flag)) prevented = true;
    }
  }
  return prevented;
}

// ----------------------------------------------------------- element API ----

void Bindings::Impl::install_document_node(const std::shared_ptr<Object>& object, NodeId node) {
  getter(object, "parentNode", [this, node]() { return wrap(document.parent(node)); });
  getter(object, "parentElement", [this, node]() {
    const NodeId parent = document.parent(node);
    return parent.valid() && document.type(parent) == NodeType::Element ? wrap(parent) : Value::null();
  });
  getter(object, "nextSibling", [this, node]() { return wrap(document.next_sibling(node)); });
  getter(object, "childNodes", [this, node]() {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    for (const NodeId child : document.children(node)) array->elements.push_back(wrap(child));
    return Value::object_of(array);
  });

  method(object, "appendChild", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    NodeId child;
    if (!node_of(arguments[0], child)) return Value::null();
    document.append_child(node, child);
    return arguments[0];
  });
  method(object, "removeChild", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    NodeId child;
    if (!node_of(arguments[0], child)) return Value::null();
    document.remove_child(node, child);
    return arguments[0];
  });
  method(object, "addEventListener", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.size() < 2 || arguments[1].type != js::ValueType::Function) return Value::undefined();
    listeners[node_key(node)].emplace_back(js::to_string(arguments[0]), arguments[1]);
    return Value::undefined();
  });
  method(object, "removeEventListener", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.size() < 2) return Value::undefined();
    const auto found = listeners.find(node_key(node));
    if (found == listeners.end()) return Value::undefined();
    const std::string type = js::to_string(arguments[0]);
    auto& entries = found->second;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const std::pair<std::string, Value>& entry) {
                                   return entry.first == type && entry.second.function == arguments[1].function;
                                 }),
                  entries.end());
    return Value::undefined();
  });
  method(object, "dispatchEvent", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty() || arguments[0].type != js::ValueType::Object) return Value::boolean_of(true);
    const auto type_property = arguments[0].object->properties.find("type");
    const std::string type = type_property == arguments[0].object->properties.end()
                                 ? std::string("event")
                                 : js::to_string(type_property->second.value);
    const bool prevented = dispatch(node, type, arguments[0]);
    return Value::boolean_of(!prevented);
  });
}

void Bindings::Impl::install_element(const std::shared_ptr<Object>& object, NodeId node) {
  install_document_node(object, node);

  getter(object, "nodeType", []() { return Value::number_of(1.0); });
  getter(object, "tagName", [this, node]() { return Value::string_of(ascii_upper(document.tag_name(node))); });
  getter(object, "nodeName", [this, node]() { return Value::string_of(ascii_upper(document.tag_name(node))); });
  getter(object, "localName", [this, node]() { return Value::string_of(std::string(document.tag_name(node))); });
  getter(object, "id", [this, node]() { return Value::string_of(std::string(document.attribute(node, "id"))); });
  setter(object, "id", [this, node](const Value& value) {
    document.set_attribute(node, "id", js::to_string(value));
  });
  getter(object, "className", [this, node]() {
    return Value::string_of(std::string(document.attribute(node, "class")));
  });
  setter(object, "className", [this, node](const Value& value) {
    document.set_attribute(node, "class", js::to_string(value));
  });
  getter(object, "textContent", [this, node]() { return Value::string_of(document.text_content(node)); });
  setter(object, "textContent", [this, node](const Value& value) { set_text_content(node, js::to_string(value)); });
  getter(object, "innerText", [this, node]() { return Value::string_of(document.text_content(node)); });
  setter(object, "innerText", [this, node](const Value& value) { set_text_content(node, js::to_string(value)); });
  getter(object, "innerHTML", [this, node]() { return Value::string_of(inner_html(node)); });
  setter(object, "innerHTML", [this, node](const Value& value) { set_inner_html(node, js::to_string(value)); });
  getter(object, "outerHTML", [this, node]() { return Value::string_of(document.to_html(node)); });
  getter(object, "style", [this, node]() { return make_style_object(node); });
  getter(object, "children", [this, node]() {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    for (const NodeId child : element_list(document.children(node))) array->elements.push_back(wrap(child));
    return Value::object_of(array);
  });
  getter(object, "firstElementChild", [this, node]() {
    for (const NodeId child : document.children(node)) {
      if (document.type(child) == NodeType::Element) return wrap(child);
    }
    return Value::null();
  });
  getter(object, "nextElementSibling", [this, node]() {
    NodeId sibling = document.next_sibling(node);
    while (sibling.valid()) {
      if (document.type(sibling) == NodeType::Element) return wrap(sibling);
      sibling = document.next_sibling(sibling);
    }
    return Value::null();
  });
  getter(object, "value", [this, node]() {
    const std::string_view value = document.attribute(node, "value");
    if (!value.empty()) return Value::string_of(std::string(value));
    if (document.tag_name(node) == "textarea") return Value::string_of(document.text_content(node));
    return Value::string_of(std::string());
  });
  setter(object, "value", [this, node](const Value& value) {
    document.set_attribute(node, "value", js::to_string(value));
  });
  getter(object, "href", [this, node]() { return Value::string_of(resolved_url(document.attribute(node, "href"))); });
  setter(object, "href", [this, node](const Value& value) {
    document.set_attribute(node, "href", js::to_string(value));
  });
  getter(object, "src", [this, node]() { return Value::string_of(resolved_url(document.attribute(node, "src"))); });
  getter(object, "checked", [this, node]() {
    return Value::boolean_of(document.attribute(node, "checked") == "checked");
  });

  method(object, "getAttribute", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    const std::string_view value = document.attribute(node, js::to_string(arguments[0]));
    return value.empty() ? Value::null() : Value::string_of(std::string(value));
  });
  method(object, "setAttribute", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.size() < 2) return Value::undefined();
    document.set_attribute(node, js::to_string(arguments[0]), js::to_string(arguments[1]));
    return Value::undefined();
  });
  method(object, "hasAttribute", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::boolean_of(false);
    for (const dom::Attribute& attribute : document.attributes(node)) {
      if (attribute.name == js::to_string(arguments[0])) return Value::boolean_of(true);
    }
    return Value::boolean_of(false);
  });
  method(object, "removeAttribute", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::undefined();
    document.set_attribute(node, js::to_string(arguments[0]), "");
    return Value::undefined();
  });
  method(object, "hasClass", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::boolean_of(false);
    return Value::boolean_of(document.has_class(node, js::to_string(arguments[0])));
  });
  method(object, "querySelector", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    return wrap(document.query_selector(node, js::to_string(arguments[0])));
  });
  method(object, "querySelectorAll", [this, node](const std::vector<Value>& arguments, Value) {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    if (!arguments.empty()) {
      for (const NodeId match : document.query_selector_all(node, js::to_string(arguments[0]))) {
        array->elements.push_back(wrap(match));
      }
    }
    return Value::object_of(array);
  });
  method(object, "getElementsByTagName", [this, node](const std::vector<Value>& arguments, Value) {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    if (!arguments.empty()) {
      const std::string tag = js::to_string(arguments[0]);
      for (const NodeId descendant : document.descendants(node)) {
        if (document.type(descendant) == NodeType::Element && (tag == "*" || document.tag_name(descendant) == tag)) {
          array->elements.push_back(wrap(descendant));
        }
      }
    }
    return Value::object_of(array);
  });
  method(object, "matches", [this, node](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::boolean_of(false);
    for (const NodeId match : document.query_selector_all(document.parent(node).valid() ? document.parent(node)
                                                                                        : node,
                                                           js::to_string(arguments[0]))) {
      if (match == node) return Value::boolean_of(true);
    }
    return Value::boolean_of(false);
  });
  method(object, "click", [this, node](const std::vector<Value>&, Value) {
    (void)dispatch(node, "click", Value::undefined());
    return Value::undefined();
  });
  method(object, "focus", [](const std::vector<Value>&, Value) { return Value::undefined(); });
  method(object, "blur", [](const std::vector<Value>&, Value) { return Value::undefined(); });

  // classList: a small object with add/remove/toggle/contains over the class attribute.
  auto class_list = std::make_shared<Object>();
  getter(class_list, "length", [this, node]() {
    std::size_t count = 0;
    const std::string_view classes = document.attribute(node, "class");
    std::size_t start = 0;
    while (start < classes.size()) {
      const std::size_t end = classes.find(' ', start);
      if (end != start) ++count;
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return Value::number_of(static_cast<double>(count));
  });
  auto classes_of = [this, node]() {
    std::vector<std::string> out;
    const std::string_view classes = document.attribute(node, "class");
    std::size_t start = 0;
    while (start < classes.size()) {
      std::size_t end = classes.find(' ', start);
      if (end == std::string_view::npos) end = classes.size();
      if (end > start) out.emplace_back(classes.substr(start, end - start));
      start = end + 1;
    }
    return out;
  };
  auto store_classes = [this, node](const std::vector<std::string>& classes) {
    std::string joined;
    for (const std::string& name : classes) {
      if (!joined.empty()) joined += ' ';
      joined += name;
    }
    document.set_attribute(node, "class", joined);
  };
  method(class_list, "contains", [classes_of](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::boolean_of(false);
    const std::string name = js::to_string(arguments[0]);
    const std::vector<std::string> classes = classes_of();
    return Value::boolean_of(std::find(classes.begin(), classes.end(), name) != classes.end());
  });
  method(class_list, "add", [classes_of, store_classes](const std::vector<Value>& arguments, Value) {
    std::vector<std::string> classes = classes_of();
    for (const Value& argument : arguments) {
      const std::string name = js::to_string(argument);
      if (std::find(classes.begin(), classes.end(), name) == classes.end()) classes.push_back(name);
    }
    store_classes(classes);
    return Value::undefined();
  });
  method(class_list, "remove", [classes_of, store_classes](const std::vector<Value>& arguments, Value) {
    std::vector<std::string> classes = classes_of();
    for (const Value& argument : arguments) {
      const std::string name = js::to_string(argument);
      classes.erase(std::remove(classes.begin(), classes.end(), name), classes.end());
    }
    store_classes(classes);
    return Value::undefined();
  });
  method(class_list, "toggle", [classes_of, store_classes](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::boolean_of(false);
    const std::string name = js::to_string(arguments[0]);
    std::vector<std::string> classes = classes_of();
    const auto found = std::find(classes.begin(), classes.end(), name);
    const bool present = found != classes.end();
    if (present) {
      classes.erase(found);
    } else {
      classes.push_back(name);
    }
    store_classes(classes);
    return Value::boolean_of(!present);
  });
  object->properties["classList"] = Property{Value::object_of(class_list)};
}

// ------------------------------------------------------------- document -----

void Bindings::Impl::install_document() {
  auto object = std::make_shared<Object>();
  object->properties["__nodeIndex"] = Property{Value::number_of(static_cast<double>(document.root().index))};
  object->properties["__nodeGeneration"] = Property{Value::number_of(static_cast<double>(document.root().generation))};

  install_document_node(object, document.root());
  getter(object, "nodeType", []() { return Value::number_of(9.0); });
  getter(object, "body", [this]() { return wrap(document.body()); });
  getter(object, "documentElement", [this]() { return wrap(document.document_element()); });
  getter(object, "readyState", []() { return Value::string_of("complete"); });
  getter(object, "URL", [this]() { return Value::string_of(document.source_url()); });
  getter(object, "location", [this]() { return location_object; });
  getter(object, "title", [this]() {
    for (const NodeId node : document.get_elements_by_tag_name("title")) return Value::string_of(document.text_content(node));
    return Value::string_of(std::string());
  });
  setter(object, "title", [this](const Value& value) {
    const std::string title = js::to_string(value);
    for (const NodeId node : document.get_elements_by_tag_name("title")) {
      set_text_content(node, title);
      return;
    }
    const NodeId head = document.query_selector(document.root(), "head");
    if (!head.valid()) return;
    const NodeId element = document.create_element("title");
    document.append_child(head, element);
    document.append_text(element, title);
  });

  method(object, "getElementById", [this](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    const NodeId found = document.get_element_by_id(js::to_string(arguments[0]));
    return found.valid() ? wrap(found) : Value::null();
  });
  method(object, "querySelector", [this](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    const NodeId found = document.query_selector(document.root(), js::to_string(arguments[0]));
    return found.valid() ? wrap(found) : Value::null();
  });
  method(object, "querySelectorAll", [this](const std::vector<Value>& arguments, Value) {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    if (!arguments.empty()) {
      for (const NodeId match : document.query_selector_all(document.root(), js::to_string(arguments[0]))) {
        array->elements.push_back(wrap(match));
      }
    }
    return Value::object_of(array);
  });
  method(object, "getElementsByTagName", [this](const std::vector<Value>& arguments, Value) {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    if (!arguments.empty()) {
      const std::string tag = ascii_lower(js::to_string(arguments[0]));
      for (const NodeId node : document.get_elements_by_tag_name(tag)) array->elements.push_back(wrap(node));
    }
    return Value::object_of(array);
  });
  method(object, "getElementsByClassName", [this](const std::vector<Value>& arguments, Value) {
    auto array = std::make_shared<Object>();
    array->kind = Object::Kind::Array;
    if (!arguments.empty()) {
      const std::string name = js::to_string(arguments[0]);
      for (const NodeId node : document.descendants(document.root())) {
        if (document.type(node) == NodeType::Element && document.has_class(node, name)) {
          array->elements.push_back(wrap(node));
        }
      }
    }
    return Value::object_of(array);
  });
  method(object, "createElement", [this](const std::vector<Value>& arguments, Value) {
    if (arguments.empty()) return Value::null();
    const NodeId created = document.create_element(ascii_lower(js::to_string(arguments[0])));
    return wrap(created);
  });
  method(object, "createTextNode", [this](const std::vector<Value>& arguments, Value) {
    const NodeId created = document.create_text(arguments.empty() ? std::string() : js::to_string(arguments[0]));
    return wrap(created);
  });
  method(object, "createComment", [this](const std::vector<Value>& arguments, Value) {
    const NodeId created = document.create_comment(arguments.empty() ? std::string() : js::to_string(arguments[0]));
    return wrap(created);
  });

  document_object = Value::object_of(object);
}

// --------------------------------------------------------------- window -----

void Bindings::Impl::install_window() {
  auto object = std::make_shared<Object>();

  location_object = Value::object_of(std::make_shared<Object>());
  auto location = location_object.object;
  const Result<net::Url> parsed = net::parse_url(document.source_url());
  const bool has_url = parsed.is_ok();
  location->getters["href"] = [this]() { return Value::string_of(document.source_url()); };
  location->getters["pathname"] = [has_url, parsed]() {
    return Value::string_of(has_url ? parsed.value().path : std::string("/"));
  };
  location->getters["search"] = [has_url, parsed]() {
    return Value::string_of(has_url && !parsed.value().query.empty() ? "?" + parsed.value().query : std::string());
  };
  location->getters["hash"] = [has_url, parsed]() {
    return Value::string_of(has_url && !parsed.value().fragment.empty() ? "#" + parsed.value().fragment
                                                                        : std::string());
  };
  location->getters["protocol"] = [has_url, parsed]() {
    return Value::string_of(has_url ? parsed.value().scheme + ":" : std::string());
  };
  location->getters["host"] = [has_url, parsed]() {
    return Value::string_of(has_url ? parsed.value().host : std::string());
  };
  location->properties["toString"] = Property{make_native("toString", [this](const std::vector<Value>&, Value) {
                                                return Value::string_of(document.source_url());
                                              })};

  object->properties["document"] = Property{document_object};
  object->properties["location"] = Property{location_object};
  object->properties["innerWidth"] = Property{Value::number_of(1024.0)};
  object->properties["innerHeight"] = Property{Value::number_of(768.0)};
  object->properties["devicePixelRatio"] = Property{Value::number_of(1.0)};
  object->properties["name"] = Property{Value::string_of(std::string())};

  auto navigator = std::make_shared<Object>();
  navigator->properties["userAgent"] =
      Property{Value::string_of("Mozilla/5.0 (compatible; BonkBrauzer/" + version_string() + ")")};
  navigator->properties["language"] = Property{Value::string_of("en-US")};
  navigator->properties["platform"] = Property{Value::string_of("BonkOS")};
  object->properties["navigator"] = Property{Value::object_of(navigator)};

  method(object, "addEventListener", [this](const std::vector<Value>& arguments, Value) {
    if (arguments.size() < 2) return Value::undefined();
    listeners[node_key(document.root())].emplace_back(js::to_string(arguments[0]), arguments[1]);
    return Value::undefined();
  });
  method(object, "removeEventListener", [](const std::vector<Value>&, Value) { return Value::undefined(); });
  method(object, "matchMedia", [](const std::vector<Value>& arguments, Value) {
    auto media = std::make_shared<Object>();
    media->properties["matches"] = Property{Value::boolean_of(false)};
    media->properties["media"] = Property{arguments.empty() ? Value::string_of(std::string()) : arguments[0]};
    return Value::object_of(media);
  });
  method(object, "requestAnimationFrame", [](const std::vector<Value>& arguments, Value) {
    // Frames are not scheduled yet; the callback simply runs on the next pump.
    if (!arguments.empty()) {
      std::vector<Value> none;
      (void)none;
    }
    return Value::number_of(0);
  });

  window = Value::object_of(object);
  // Self-references are getters over a weak_ptr: a strong `window.window = window`
  // property would be a reference cycle that no refcount can ever free.
  const std::weak_ptr<Object> weak_window = object;
  auto self_getter = [weak_window]() {
    const std::shared_ptr<Object> strong = weak_window.lock();
    return strong == nullptr ? Value::null() : Value::object_of(strong);
  };
  getter(object, "window", self_getter);
  getter(object, "self", self_getter);
  getter(object, "globalThis", self_getter);
}

// ----------------------------------------------------------- public API ----

Bindings::Bindings(js::Context& context, dom::Document& document)
    : impl_(std::make_unique<Impl>(context, document)), context_(&context), document_(&document) {}

Bindings::~Bindings() = default;

void Bindings::install() {
  impl_->install_document();
  impl_->install_window();
  context_->set_global("window", impl_->window);
  context_->set_global("document", impl_->document_object);
  context_->set_global("location", impl_->location_object);
  context_->set_global("navigator", impl_->window.object->properties.at("navigator").value);
  // The engine's own console writes into the context buffer; window.console mirrors it.
  context_->set_global("alert", context_->runtime().global("alert"));
}

void Bindings::run_scripts() {
  impl_->runs.clear();
  std::size_t index = 0;
  for (const NodeId node : document_->get_elements_by_tag_name("script")) {
    ++index;
    ScriptRun run;
    run.url = document_->source_url() + "#script-" + std::to_string(index);
    const std::string_view type = document_->attribute(node, "type");
    if (!type.empty() && type.find("javascript") == std::string_view::npos &&
        type.find("ecmascript") == std::string_view::npos) {
      run.skipped = true;
      run.message = "skipped: type=\"" + std::string(type) + "\"";
      impl_->runs.push_back(std::move(run));
      continue;
    }
    if (!document_->attribute(node, "src").empty()) {
      run.skipped = true;
      run.message = "skipped: external scripts are not fetched yet";
      impl_->runs.push_back(std::move(run));
      continue;
    }
    const std::string source = document_->text_content(node);
    if (source.empty()) {
      run.skipped = true;
      run.message = "skipped: empty script";
      impl_->runs.push_back(std::move(run));
      continue;
    }
    const js::ExecutionResult result = context_->evaluate(source, run.url);
    run.ok = result.ok;
    run.message = result.message;
    impl_->runs.push_back(std::move(run));
  }
  context_->run_pending_callbacks();
}

bool Bindings::dispatch(NodeId target, std::string_view type, js::Value detail) {
  return impl_->dispatch(target, std::string(type), std::move(detail));
}

js::Value Bindings::wrap(NodeId node) { return impl_->wrap(node); }

bool Bindings::node_of(const js::Value& value, NodeId& out) const { return impl_->node_of(value, out); }

const std::vector<ScriptRun>& Bindings::script_runs() const noexcept { return impl_->runs; }

}  // namespace bonk::bindings
