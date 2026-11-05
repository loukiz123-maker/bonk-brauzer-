// JS <-> DOM bridge: wrappers, accessors, events and inline scripts.
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "bonk/bindings/bindings.hpp"
#include "bonk/html/html.hpp"
#include "bonk/js/js.hpp"

using namespace bonk;
using namespace bonk::bindings;
using namespace bonk::js;

namespace {

struct Fixture {
  dom::Document document;
  Context context;
  Bindings bindings;

  explicit Fixture(const std::string& html, const std::string& url = "file:/page.html")
      : document(url), context(), bindings(context, document) {
    const Status parsed = html::parse_html(html, document);
    CHECK(parsed.is_ok());
    bindings.install();
  }

  ExecutionResult run(const std::string& source) { return context.evaluate(source, "test"); }

  Value global(const std::string& name) { return context.runtime().global(name); }
  std::string text(const std::string& name) { return to_string(global(name)); }
  double number(const std::string& name) { return to_number(global(name)); }
};

}  // namespace

BONK_TEST(bindings, wrapper_identity_is_stable) {
  Fixture fixture("<html><body><div id='a'></div><div id='b'></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var a = document.getElementById('a');\n"
      "var b = document.getElementById('b');\n"
      "var same = (a === document.getElementById('a'));\n"
      "var different = (a !== b);\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("same"), 1.0);
  CHECK_EQ(fixture.number("different"), 1.0);
}

BONK_TEST(bindings, text_content_and_attributes_write_through) {
  Fixture fixture("<html><body><p id='p' class='x'>old</p></body></html>");
  const ExecutionResult result = fixture.run(
      "var p = document.getElementById('p');\n"
      "p.textContent = 'new text';\n"
      "p.setAttribute('data-role', 'note');\n"
      "p.className = 'renamed';\n"
      "var read = p.textContent;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.text("read"), std::string("new text"));

  const dom::NodeId node = fixture.document.get_element_by_id("p");
  CHECK_EQ(std::string(fixture.document.text_content(node)), std::string("new text"));
  CHECK_EQ(std::string(fixture.document.attribute(node, "data-role")), std::string("note"));
  CHECK(fixture.document.has_class(node, "renamed"));
}

BONK_TEST(bindings, create_element_and_append_child_mutate_the_tree) {
  Fixture fixture("<html><body><ul id='list'></ul></body></html>");
  const ExecutionResult result = fixture.run(
      "var list = document.getElementById('list');\n"
      "for (var i = 0; i < 3; i++) {\n"
      "  var item = document.createElement('li');\n"
      "  item.textContent = 'row ' + i;\n"
      "  list.appendChild(item);\n"
      "}\n"
      "var count = list.children.length;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("count"), 3.0);

  const dom::NodeId list = fixture.document.get_element_by_id("list");
  const std::vector<dom::NodeId> children = fixture.document.children(list);
  REQUIRE(children.size() == 3);
  CHECK_EQ(std::string(fixture.document.text_content(children[2])), std::string("row 2"));
  CHECK_EQ(std::string(fixture.document.tag_name(children[0])), std::string("li"));
}

BONK_TEST(bindings, inner_html_is_parsed_and_serialized) {
  Fixture fixture("<html><body><div id='host'></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var host = document.getElementById('host');\n"
      "host.innerHTML = '<span class=\"tag\">hi</span><b>bold</b>';\n"
      "var markup = host.innerHTML;\n"
      "var spans = host.getElementsByTagName('span').length;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("spans"), 1.0);
  CHECK(fixture.text("markup").find("hi") != std::string::npos);

  const dom::NodeId host = fixture.document.get_element_by_id("host");
  CHECK_EQ(fixture.document.children(host).size(), std::size_t(2));
}

BONK_TEST(bindings, inner_html_on_a_cell_uses_fragment_parsing) {
  // A `td` context parses with the "in body" rules, so a stray `<tr>` is dropped
  // instead of nesting a second row inside the cell.
  Fixture fixture("<html><body><table><tr><td id='cell'></td></tr></table></body></html>");
  const ExecutionResult result = fixture.run(
      "var cell = document.getElementById('cell');\n"
      "cell.innerHTML = '<tr><td>x</td></tr>y';\n"
      "var rows = document.getElementsByTagName('tr').length;\n"
      "var text = cell.textContent;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("rows"), 1.0);
  CHECK_EQ(fixture.text("text"), std::string("xy"));

  // A `tr` context does grow cells without a table around it.
  const ExecutionResult row_result = fixture.run(
      "var table = document.getElementsByTagName('table')[0];\n"
      "var row = document.createElement('tr');\n"
      "row.innerHTML = '<td>a</td><td>b</td>';\n"
      "table.appendChild(row);\n"
      "var cells = row.children.length;\n");
  CHECK(row_result.ok);
  CHECK_EQ(fixture.number("cells"), 2.0);

  // `textarea` innerHTML is RCDATA: markup stays text.
  const ExecutionResult text_result = fixture.run(
      "var area = document.createElement('textarea');\n"
      "area.innerHTML = '<b>x</b>';\n"
      "document.getElementById('cell').appendChild(area);\n"
      "var areaText = area.textContent;\n");
  CHECK(text_result.ok);
  CHECK_EQ(fixture.text("areaText"), std::string("<b>x</b>"));
}

BONK_TEST(bindings, class_list_add_remove_toggle) {
  Fixture fixture("<html><body><div id='box' class='one'></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var box = document.getElementById('box');\n"
      "box.classList.add('two');\n"
      "box.classList.remove('one');\n"
      "var hasTwo = box.classList.contains('two');\n"
      "var toggled = box.classList.toggle('two');\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("hasTwo"), 1.0);
  CHECK_EQ(fixture.number("toggled"), 0.0);

  const dom::NodeId box = fixture.document.get_element_by_id("box");
  CHECK(!fixture.document.has_class(box, "two"));
  CHECK_EQ(std::string(fixture.document.attribute(box, "class")), std::string(""));
}

BONK_TEST(bindings, inline_style_is_written_to_the_attribute) {
  Fixture fixture("<html><body><div id='s'></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var s = document.getElementById('s');\n"
      "s.style.setProperty('color', 'red');\n"
      "s.style.fontSize = '20px';\n"
      "var color = s.style.color;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.text("color"), std::string("red"));

  const dom::NodeId node = fixture.document.get_element_by_id("s");
  const std::string style(fixture.document.attribute(node, "style"));
  CHECK(style.find("color: red") != std::string::npos);
  CHECK(style.find("font-size: 20px") != std::string::npos);
}

BONK_TEST(bindings, document_title_reads_and_writes_the_title_element) {
  Fixture fixture("<html><head><title>Original</title></head><body></body></html>");
  const ExecutionResult result = fixture.run(
      "var before = document.title;\n"
      "document.title = 'Changed';\n"
      "var after = document.title;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.text("before"), std::string("Original"));
  CHECK_EQ(fixture.text("after"), std::string("Changed"));
}

BONK_TEST(bindings, query_selector_helpers) {
  Fixture fixture(
      "<html><body><div class='card'><p id='first'>a</p></div><div class='card'><p>b</p></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var cards = document.querySelectorAll('.card').length;\n"
      "var p = document.querySelector('#first');\n"
      "var paragraphs = document.getElementsByTagName('p').length;\n"
      "var text = p.textContent;\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("cards"), 2.0);
  CHECK_EQ(fixture.number("paragraphs"), 2.0);
  CHECK_EQ(fixture.text("text"), std::string("a"));
}

BONK_TEST(bindings, click_listeners_run_and_bubble) {
  Fixture fixture(
      "<html><body><div id='outer'><button id='inner'>go</button></div></body></html>");
  const ExecutionResult result = fixture.run(
      "var outer = document.getElementById('outer');\n"
      "var inner = document.getElementById('inner');\n"
      "var log = [];\n"
      "inner.addEventListener('click', function(event) { log.push('inner:' + event.type); });\n"
      "outer.addEventListener('click', function() { log.push('outer'); });\n");
  CHECK(result.ok);

  const dom::NodeId inner = fixture.document.get_element_by_id("inner");
  const bool prevented = fixture.bindings.dispatch(inner, "click");
  CHECK(!prevented);
  CHECK_EQ(fixture.text("log"), std::string("inner:click,outer"));
}

BONK_TEST(bindings, prevent_default_is_reported) {
  Fixture fixture("<html><body><a id='link' href='/next'>next</a></body></html>");
  const ExecutionResult result = fixture.run(
      "var link = document.getElementById('link');\n"
      "link.addEventListener('click', function(event) { event.preventDefault(); });\n");
  CHECK(result.ok);

  const dom::NodeId link = fixture.document.get_element_by_id("link");
  CHECK(fixture.bindings.dispatch(link, "click"));
}

BONK_TEST(bindings, element_click_method_dispatches) {
  Fixture fixture("<html><body><button id='b'>x</button></body></html>");
  const ExecutionResult result = fixture.run(
      "var clicks = 0;\n"
      "var b = document.getElementById('b');\n"
      "b.addEventListener('click', function() { clicks++; });\n"
      "b.click();\n"
      "b.click();\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("clicks"), 2.0);
}

BONK_TEST(bindings, inline_scripts_run_in_document_order) {
  Fixture fixture(
      "<html><body><script>var order = 'one';</script>"
      "<script>order += '-two'; console.log('ran', order);</script></body></html>");
  fixture.bindings.run_scripts();
  CHECK_EQ(fixture.text("order"), std::string("one-two"));
  REQUIRE(fixture.bindings.script_runs().size() == 2);
  CHECK(fixture.bindings.script_runs()[0].ok);
  CHECK(fixture.bindings.script_runs()[1].ok);
  REQUIRE(fixture.context.console().size() == 1);
  CHECK_EQ(fixture.context.console()[0].message, std::string("ran one-two"));
}

BONK_TEST(bindings, failing_script_is_recorded_not_fatal) {
  Fixture fixture("<html><body><script>throw new Error('boom');</script><script>var after = 1;</script></body></html>");
  fixture.bindings.run_scripts();
  REQUIRE(fixture.bindings.script_runs().size() == 2);
  CHECK(!fixture.bindings.script_runs()[0].ok);
  CHECK(fixture.bindings.script_runs()[0].message.find("boom") != std::string::npos);
  CHECK(fixture.bindings.script_runs()[1].ok);
  CHECK_EQ(fixture.number("after"), 1.0);
}

BONK_TEST(bindings, external_and_non_javascript_scripts_are_skipped) {
  Fixture fixture(
      "<html><body>"
      "<script src='app.js'></script>"
      "<script type='text/template'>not code</script>"
      "<script>var ran = 1;</script>"
      "</body></html>");
  fixture.bindings.run_scripts();
  const std::vector<ScriptRun>& runs = fixture.bindings.script_runs();
  REQUIRE(runs.size() == 3);
  CHECK(runs[0].skipped);
  CHECK(runs[0].message.find("external") != std::string::npos);
  CHECK(runs[1].skipped);
  CHECK(runs[2].ok);
  CHECK_EQ(fixture.number("ran"), 1.0);
}

BONK_TEST(bindings, window_and_navigator_are_installed) {
  Fixture fixture("<html><body></body></html>", "https://example.com/dir/page.html");
  const ExecutionResult result = fixture.run(
      "var hasDocument = (typeof document === 'object');\n"
      "var ua = navigator.userAgent;\n"
      "var here = window.location.pathname;\n"
      "var sameWindow = (window.window === window);\n");
  CHECK(result.ok);
  CHECK_EQ(fixture.number("hasDocument"), 1.0);
  CHECK_EQ(fixture.number("sameWindow"), 1.0);
  CHECK(fixture.text("ua").find("BonkBrauzer") != std::string::npos);
  CHECK_EQ(fixture.text("here"), std::string("/dir/page.html"));
}

BONK_TEST(bindings, node_ids_round_trip_through_wrappers) {
  Fixture fixture("<html><body><div id='n'></div></body></html>");
  const dom::NodeId node = fixture.document.get_element_by_id("n");
  const Value wrapper = fixture.bindings.wrap(node);
  dom::NodeId round_tripped;
  CHECK(fixture.bindings.node_of(wrapper, round_tripped));
  CHECK(round_tripped == node);
}
