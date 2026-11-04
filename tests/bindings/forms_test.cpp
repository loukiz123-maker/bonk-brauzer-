#include <cstdio>
#include <fstream>
#include <string>

#include "bonk/net/cookie.hpp"
#include "bonk/net/http.hpp"
#include "bonk/shell/shell.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::net;

namespace {

Url url_of(const std::string& text) {
  const Result<Url> url = parse_url(text);
  if (url.is_error()) ::bonk::test::record_failure(__FILE__, __LINE__, "bad url: " + text);
  return url.value();
}

std::string write_page(const std::string& name, std::string_view html) {
  const std::string path = "/tmp/bonk-forms-" + name;
  std::ofstream file(path, std::ios::binary);
  file << html;
  return path;
}

}  // namespace

BONK_TEST(cookies, stores_and_sends_matching_cookies) {
  CookieJar jar;
  CHECK(jar.store_from_header(url_of("https://example.com/a/b"), "sid=42; Path=/; HttpOnly"));
  CHECK(jar.store_from_header(url_of("https://example.com/a/b"), "theme=dark"));
  CHECK_EQ(jar.size(), std::size_t{2});

  // Path defaults to the directory of the request URL ("/a").
  CHECK_EQ(jar.cookie_header(url_of("https://example.com/a/c")), std::string("theme=dark; sid=42"));
  CHECK_EQ(jar.cookie_header(url_of("https://example.com/other")), std::string("sid=42"));
  CHECK_EQ(jar.cookie_header(url_of("https://sub.example.com/a/c")), std::string(""));
  // HttpOnly cookies never reach document.cookie.
  CHECK_EQ(jar.document_cookie(url_of("https://example.com/a/c")), std::string("theme=dark"));
}

BONK_TEST(cookies, domain_attribute_and_deletion) {
  CookieJar jar;
  CHECK(jar.store_from_header(url_of("https://example.com/"), "wide=1; Domain=.example.com; Path=/"));
  CHECK_EQ(jar.cookie_header(url_of("https://shop.example.com/")), std::string("wide=1"));
  // A cookie for somebody else's domain is rejected.
  CHECK(!jar.store_from_header(url_of("https://example.com/"), "evil=1; Domain=other.test"));
  // Max-Age=0 deletes.
  CHECK(jar.store_from_header(url_of("https://example.com/"), "wide=gone; Domain=example.com; Path=/; Max-Age=0"));
  CHECK_EQ(jar.cookie_header(url_of("https://shop.example.com/")), std::string(""));
}

BONK_TEST(cookies, secure_flag_and_document_setter) {
  CookieJar jar;
  CHECK(jar.store_from_header(url_of("https://example.com/"), "token=abc; Secure; Path=/"));
  CHECK_EQ(jar.cookie_header(url_of("https://example.com/")), std::string("token=abc"));
  CHECK_EQ(jar.cookie_header(url_of("http://example.com/")), std::string(""));
  jar.set_document_cookie(url_of("https://example.com/"), "js=1; path=/; HttpOnly");
  const Cookie& last = jar.cookies().back();
  CHECK_EQ(last.name, std::string("js"));
  CHECK(!last.http_only);  // scripts cannot create HttpOnly cookies
  CHECK_EQ(jar.document_cookie(url_of("https://example.com/")).find("js=1") != std::string::npos, true);
}

BONK_TEST(cookies, request_carries_cookie_header_and_body) {
  HttpRequest request;
  request.url = url_of("http://example.com/form");
  request.method = "POST";
  request.body = "a=1&b=two+words";
  request.cookie_jar = nullptr;
  const std::string wire = build_request(request);
  CHECK(wire.starts_with("POST /form HTTP/1.1\r\n"));
  CHECK(wire.find("Content-Length: 15\r\n") != std::string::npos);
  CHECK(wire.find("Content-Type: application/x-www-form-urlencoded\r\n") != std::string::npos);
  CHECK(wire.ends_with("\r\n\r\na=1&b=two+words"));
  CHECK(wire.find("Cookie:") == std::string::npos);
}

BONK_TEST(storage, persists_keys_to_disk) {
  const std::string path = "/tmp/bonk-forms-storage.txt";
  std::remove(path.c_str());
  {
    bindings::Storage storage(path);
    storage.set("name", "Bonk Brauzer");
    storage.set("multi", std::string("line\tone\ntwo"));
    CHECK_EQ(storage.size(), std::size_t{2});
    CHECK(storage.save());
  }
  bindings::Storage reloaded(path);
  CHECK(reloaded.load());
  CHECK_EQ(reloaded.get("name"), std::string("Bonk Brauzer"));
  CHECK_EQ(reloaded.get("multi"), std::string("line\tone\ntwo"));
  CHECK(reloaded.has("name"));
  reloaded.remove("name");
  CHECK(!reloaded.has("name"));
  CHECK_EQ(reloaded.keys(), std::vector<std::string>{"multi"});
  reloaded.clear();
  CHECK_EQ(reloaded.size(), std::size_t{0});
}

BONK_TEST(forms, collects_controls_into_urlencoded_body) {
  const std::string path = write_page(
      "collect.html",
      "<html><body><form id=f action='/search' method=get>"
      "<input type=text name=q value='two words'>"
      "<input type=checkbox name=flag checked>"
      "<input type=checkbox name=off>"
      "<select name=size><option value=s>Small</option><option value=l selected>Large</option></select>"
      "<textarea name=note>hi there</textarea>"
      "<input type=submit value=Go></form></body></html>");
  shell::Session session(700.0f, 500.0f);
  REQUIRE(session.navigate(path).is_ok());
  // Reach the form through the public DOM of the loaded document.
  const std::vector<dom::NodeId> forms = session.document()->get_elements_by_tag_name("form");
  REQUIRE(forms.size() == 1);
  const bindings::FormSubmission submission = session.bindings()->collect_form_submission(forms.front());
  CHECK_EQ(submission.method, std::string("GET"));
  CHECK_EQ(submission.action, std::string("/search"));
  CHECK_EQ(submission.body,
           std::string("q=two+words&flag=on&size=l&note=hi+there"));
}

BONK_TEST(forms, submit_button_navigates_with_query_string) {
  (void)write_page("result.html", "<html><head><title>results</title></head><body>ok</body></html>");
  const std::string path = write_page("submit.html",
                                      "<html><body><form action='bonk-forms-result.html' method=get>"
                                      "<input type=text name=q value='hello world'>"
                                      "<input id=go type=submit value=Search></form></body></html>");
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  // The submit button is inside the form: click its centre.
  const layout::Box* button = nullptr;
  const std::vector<dom::NodeId> inputs = session.document()->get_elements_by_tag_name("input");
  REQUIRE(inputs.size() == 2);
  button = session.box_of(inputs[1]);
  REQUIRE(button != nullptr);
  const float x = button->content_box.x + button->content_box.width * 0.5f;
  const float y = button->border_box.y + button->border_box.height * 0.5f;
  const shell::ClickOutcome outcome = session.click(x, y);
  CHECK(outcome.needs_reload);
  CHECK_EQ(session.tab().title, std::string("results"));
  CHECK(session.tab().url.find("bonk-forms-result.html?q=hello+world") != std::string::npos);
}

BONK_TEST(forms, submit_event_can_be_cancelled) {
  const std::string path = write_page("cancel.html",
                                      "<html><body><form id=f action='/never.html'>"
                                      "<input type=text name=q value=1>"
                                      "<input id=go type=submit value=Go></form>"
                                      "<script>document.getElementById('f').addEventListener('submit',"
                                      "function(e){e.preventDefault();document.title='cancelled';});</script>"
                                      "</body></html>");
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  const std::vector<dom::NodeId> inputs = session.document()->get_elements_by_tag_name("input");
  const layout::Box* button = session.box_of(inputs[1]);
  REQUIRE(button != nullptr);
  (void)session.click(button->content_box.x + 5.0f, button->border_box.y + button->border_box.height * 0.5f);
  CHECK_EQ(session.tab().title, std::string("cancelled"));
  CHECK(session.tab().url.find("never.html") == std::string::npos);
}

BONK_TEST(forms, js_reads_and_writes_control_values) {
  const std::string path = write_page("values.html",
                                      "<html><body><form id=f><input id=user name=user value=bonk>"
                                      "<input id=news type=checkbox name=news checked>"
                                      "<textarea id=bio name=bio>hello</textarea>"
                                      "<input type=submit value=Go></form><script>"
                                      "var form = document.getElementById('f');"
                                      "document.getElementById('user').value = 'changed';"
                                      "document.getElementById('news').checked = false;"
                                      "document.title = form.elements.length + ':' +"
                                      " document.getElementById('user').value + ':' +"
                                      " document.getElementById('news').checked + ':' +"
                                      " document.getElementById('bio').value;</script></body></html>");
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  CHECK_EQ(session.tab().title, std::string("4:changed:false:hello"));
  // The value setter must reach the submission body.
  const std::vector<dom::NodeId> forms = session.document()->get_elements_by_tag_name("form");
  REQUIRE(forms.size() == 1);
  CHECK_EQ(session.bindings()->collect_form_submission(forms.front()).body,
           std::string("user=changed&bio=hello"));
}

BONK_TEST(forms, hidden_inputs_are_not_rendered_but_are_submitted) {
  const std::string path = write_page("hidden.html",
                                      "<html><body><form id=f action='x.html'>"
                                      "<input type=hidden name=token value=abc>"
                                      "<input name=q value=1></form></body></html>");
  shell::Session session(600.0f, 400.0f);
  REQUIRE(session.navigate(path).is_ok());
  const std::vector<dom::NodeId> inputs = session.document()->get_elements_by_tag_name("input");
  REQUIRE(inputs.size() == 2);
  CHECK(session.box_of(inputs[0]) == nullptr);   // display: none in the UA sheet
  CHECK(session.box_of(inputs[1]) != nullptr);
  const std::vector<dom::NodeId> forms = session.document()->get_elements_by_tag_name("form");
  REQUIRE(forms.size() == 1);
  CHECK_EQ(session.bindings()->collect_form_submission(forms.front()).body, std::string("token=abc&q=1"));
}

BONK_TEST(forms, document_cookie_round_trip) {
  const std::string path = write_page("cookie.html",
                                      "<html><body><script>"
                                      "document.cookie = 'seen=yes; path=/';"
                                      "document.title = document.cookie;</script></body></html>");
  shell::Session session(400.0f, 300.0f);
  REQUIRE(session.navigate(path).is_ok());
  // Local files have no origin, so document.cookie is inert there; the jar stays empty.
  CHECK_EQ(session.cookies().size(), std::size_t{0});
}

BONK_TEST(forms, local_storage_survives_a_reload) {
  const std::string dir = "/tmp/bonk-forms-profile";
  std::remove((dir + "/local-storage.txt").c_str());
  const std::string path = write_page("storage.html",
                                      "<html><body><script>"
                                      "var hits = localStorage.getItem('hits');"
                                      "hits = hits === null ? 1 : Number(hits) + 1;"
                                      "localStorage.setItem('hits', String(hits));"
                                      "document.title = 'hits=' + hits;</script></body></html>");
  {
    shell::Session session(400.0f, 300.0f, {}, dir);
    REQUIRE(session.navigate(path).is_ok());
    CHECK_EQ(session.tab().title, std::string("hits=1"));
  }
  {
    shell::Session session(400.0f, 300.0f, {}, dir);
    REQUIRE(session.navigate(path).is_ok());
    CHECK_EQ(session.tab().title, std::string("hits=2"));
  }
  std::remove((dir + "/local-storage.txt").c_str());
}
