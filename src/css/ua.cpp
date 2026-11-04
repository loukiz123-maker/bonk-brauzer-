#include "bonk/css/css.hpp"

namespace bonk::css {

// The user-agent stylesheet is written in the same CSS subset the engine accepts
// and parsed by the engine's own parser. If the default page looks right, the
// CSS parser, cascade and selector matcher all work.
std::string_view user_agent_stylesheet() {
  return R"CSS(
html, body, div, p, header, footer, main, nav, section, article, aside,
blockquote, pre, figure, figcaption, dl, dt, dd, address, form, fieldset,
h1, h2, h3, h4, h5, h6, ul, ol, li, hr, table, tbody, thead, tfoot, tr {
  display: block;
}
head, title, meta, link, base, script, style, template, noscript {
  display: none;
}
span, a, b, i, u, s, em, strong, code, small, big, sub, sup, label, abbr,
cite, q, mark, time, font, br, img, input, button, select, textarea {
  display: inline;
}

html {
  font-size: 16px;
  color: #000000;
}
body {
  margin: 8px;
  line-height: 1.25;
  font-family: sans-serif;
}
p {
  margin: 1em 0;
}
h1 {
  font-size: 2em;
  font-weight: 700;
  margin: 0.67em 0;
  line-height: 1.15;
}
h2 {
  font-size: 1.5em;
  font-weight: 700;
  margin: 0.83em 0;
}
h3 {
  font-size: 1.17em;
  font-weight: 700;
  margin: 1em 0;
}
h4 {
  font-size: 1em;
  font-weight: 700;
  margin: 1.33em 0;
}
h5 {
  font-size: 0.83em;
  font-weight: 700;
  margin: 1.67em 0;
}
h6 {
  font-size: 0.67em;
  font-weight: 700;
  margin: 2.33em 0;
}
b, strong {
  font-weight: 700;
}
i, em, cite {
  font-style: italic;
}
u {
  text-decoration: underline;
}
a {
  color: #0000ee;
  text-decoration: underline;
}
ul, ol {
  margin: 1em 0;
  padding: 0 0 0 40px;
}
li {
  display: list-item;
  margin: 0.25em 0;
}
blockquote {
  margin: 1em 40px;
}
pre, code, kbd, samp {
  font-family: monospace;
  white-space: pre;
}
pre {
  margin: 1em 0;
}
hr {
  margin: 0.5em 0;
  border-width: 1px;
  border-color: #808080;
  min-height: 1px;
}
img {
  display: inline-block;
}
small {
  font-size: 0.83em;
}
sub {
  font-size: 0.83em;
}
sup {
  font-size: 0.83em;
}
table {
  margin: 1em 0;
}
td, th {
  display: block;
  padding: 2px 4px;
}
)CSS";
}

}  // namespace bonk::css
