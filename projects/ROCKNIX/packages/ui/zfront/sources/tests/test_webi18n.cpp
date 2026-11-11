// webi18n: the Spanish page. Every text of the page's own markup (labels, buttons, tab names: what is between tags in the body) has
// its entry in the dictionary, the error messages the console sends have theirs, and the script is there and switches on its own.
// What the dictionary does in a browser (patterns, the observer) was looked at by hand with ?lang=es.
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "webtransfer.hpp"

using std::string;

namespace {

bool has_entry(const string& html, const string& text) { return html.find("\"" + text + "\":\"") != string::npos; }

}  // namespace

int main() {
  const string html = zf::webtransfer::page_html();
  CHECK(html.find("navigator.language") != string::npos);
  CHECK(html.find("lang=es") != string::npos && html.find("lang=en") != string::npos);
  CHECK(html.find("new MutationObserver") != string::npos);
  CHECK(html.find("window.confirm=function") != string::npos);
  // the text between tags of the body markup (not the script)
  const size_t body = html.find("<body>"), script = html.find("<script>");
  CHECK(body != string::npos && script != string::npos && body < script);
  int checked = 0;
  for (size_t at = html.find('>', body); at != string::npos && at < script; at = html.find('>', at + 1)) {
    const size_t end = html.find('<', at);
    if (end == string::npos || end > script) break;
    string t = html.substr(at + 1, end - at - 1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    while (!t.empty() && (t[0] == ' ' || t[0] == '\n')) t.erase(0, 1);
    bool letters = false;
    for (char c : t) letters = letters || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (!letters || t == "Zenith" || t == "BIOS") continue;  // the brand and "BIOS" stay as they are
    ++checked;
    if (!has_entry(html, t)) {
      std::fprintf(stderr, "test_webi18n: no Spanish for \"%s\"\n", t.c_str());
      CHECK(false);
    }
  }
  CHECK(checked > 20);
  // what the console answers when something fails (a sample of each module)
  for (const char* e : {"wrong PIN", "not enough free space", "a file with that name already exists", "another upload is running",
                        "unknown game", "the file extension cannot change", "another backup operation is running", "not a BIOS file of the list",
                        "the offset does not match what the console has", "the console cannot ask now", "too many wrong PINs, wait a little",
                        "no picture", "invalid id", "invalid game id", "method not allowed", "offset and length go past the total",
                        "offset and total are required", "system and name are required", "system and path are required", "file is required",
                        "too long", "could not make a token"})
    CHECK(has_entry(html, e));
  std::printf("test_webi18n: %d texts of the markup have their Spanish\n", checked);
  return zt::done();
}
