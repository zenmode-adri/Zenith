// Hostile input: files that are not files, images that announce absurd sizes, XML nested far beyond anything real.
// Each case used to abort or overflow the stack on the device (found by the 28-oct-2026 review).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "gamelist.hpp"
#include "imgdec.hpp"
#include "systems.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
using std::string;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void be32(string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s.push_back(char((v >> (8 * i)) & 0xff));
}

// PNG signature + IHDR announcing w x h, 8-bit RGBA; stb_image does not check the chunk CRCs
string png_header(uint32_t w, uint32_t h) {
  string s("\x89PNG\r\n\x1a\n", 8);
  be32(s, 13);
  s += "IHDR";
  be32(s, w);
  be32(s, h);
  s += string("\x08\x06\x00\x00\x00", 5);
  be32(s, 0);  // CRC (ignored)
  be32(s, 0);
  s += "IDAT";
  be32(s, 0);
  return s;
}

// RIFF/WEBP with a VP8X chunk announcing a canvas of w x h
string webp_vp8x(uint32_t w, uint32_t h) {
  string body = "WEBPVP8X";
  auto le32 = [](string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xff));
  };
  le32(body, 10);
  body += string("\x10\x00\x00\x00", 4);  // flags (alpha), reserved
  for (uint32_t v : {w - 1, h - 1})
    for (int i = 0; i < 3; ++i) body.push_back(char((v >> (8 * i)) & 0xff));
  string s = "RIFF";
  le32(s, uint32_t(body.size()));
  return s + body;
}

}  // namespace

int main() {
  string d = string("/tmp/zf-hardening-") + std::to_string(::getpid());
  if (std::system(("rm -rf " + d + " && mkdir -p " + d).c_str()) != 0) std::abort();

  // ---- read_file: only regular files of a sane size
  {
    string out = "untouched";
    CHECK(!read_file(d, out));              // a directory: used to abort in resize(LONG_MAX)
    CHECK(!read_file("/dev/null", out));    // a device
    CHECK(!read_file(d + "/missing", out));
    std::ofstream(d + "/ok.txt") << "hello";
    CHECK(read_file(d + "/ok.txt", out) && out == "hello");
    std::ofstream(d + "/empty.txt").flush();
    CHECK(read_file(d + "/empty.txt", out) && out.empty());
    string big = d + "/big.bin";  // sparse file just over the limit: refused without reading
    FILE* f = std::fopen(big.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      CHECK(::ftruncate(::fileno(f), off_t(kMaxReadFile) + 1) == 0);
      std::fclose(f);
      auto t0 = std::chrono::steady_clock::now();
      CHECK(!read_file(big, out));
      CHECK(ms_since(t0) < 1000);
    }
  }

  // ---- images that announce absurd dimensions are refused from the header alone
  {
    Img im;
    auto t0 = std::chrono::steady_clock::now();
    string p = png_header(40000, 40000);  // 1.6 Gpx: ~6 GB of RGBA
    CHECK(!decode_image(reinterpret_cast<const uint8_t*>(p.data()), p.size(), im));
    p = png_header(5000, 5000);  // 25 Mpx: over the 16 Mpx limit
    CHECK(!decode_image(reinterpret_cast<const uint8_t*>(p.data()), p.size(), im));
    string w = webp_vp8x(0x1000000, 0x1000000);
    CHECK(!decode_image(reinterpret_cast<const uint8_t*>(w.data()), w.size(), im));
    w = webp_vp8x(20000, 20000);
    CHECK(!decode_image(reinterpret_cast<const uint8_t*>(w.data()), w.size(), im));
    CHECK(ms_since(t0) < 1000);
    // a directory used as an <image> path (a gamelist can say anything)
    CHECK(!decode_image_file(d, im));
  }

  // ---- XML nested far beyond anything real: no stack overflow, the normal entries still load
  {
    const int kDepth = 300000;
    string sys =
        "<system><name>gb</name><fullname>GB</fullname><path>/tmp/zf-none</path><extension>.gb</extension>"
        "<command>x</command><platform>gb</platform></system>";
    string xml = "<systemList>" + sys;
    for (int i = 0; i < kDepth; ++i) xml += "<a>";
    xml += "<system><name>deep</name></system>";
    for (int i = 0; i < kDepth; ++i) xml += "</a>";
    xml += "</systemList>";
    std::vector<System> v;
    string err;
    load_systems_text(xml, "/tmp/zf-none", true, v, err, true);
    CHECK(v.size() == 1 && v[0].name == "gb");  // the one beyond the depth limit is ignored

    string vars = "<theme><variables><foo>bar</foo></variables>";
    for (int i = 0; i < kDepth; ++i) vars += "<a>";
    vars += "<variables><deep>1</deep></variables>";
    for (int i = 0; i < kDepth; ++i) vars += "</a>";
    vars += "</theme>";
    Theme t;
    CHECK(Theme::from_xml(vars, vars, "1/1", t, err));
    CHECK_EQ(string(t.str("foo")), string("bar"));
    CHECK_EQ(string(t.str("deep")), string());
  }

  // ---- one <game> bigger than the 16 MiB read window: that one is skipped, the rest of the list still loads
  {
    string xml = "<?xml version=\"1.0\"?>\n<gameList>\n"
        "<game><path>./a.gb</path><name>A</name></game>\n"
        "<game><path>./huge.gb</path><desc>";
    xml.append(size_t(17) << 20, 'x');
    xml += "</desc></game>\n"
           "<game><path>./b.gb</path><name>B</name></game>\n"
           "<folder><path>./f</path></folder>\n"
           "<game><path>./c.gb</path><name>C</name></game>\n</gameList>\n";
    string gl = d + "/gamelist.xml";
    std::ofstream(gl, std::ios::binary) << xml;
    string got;
    auto cb = [&](const GameEntry& e) { got += string(e.path) + ","; };
    auto t0 = std::chrono::steady_clock::now();
    GamelistInfo info = gamelist_scan_file(gl, EntryFn::of(cb));
    CHECK_EQ(got, string("./a.gb,./b.gb,./c.gb,"));
    CHECK(info.ok);
    CHECK_EQ(info.games, size_t(3));
    CHECK(ms_since(t0) < 5000);
    // a file that ends inside the oversized element: what came before is kept, the structure is reported incomplete
    string cut = xml.substr(0, xml.find("</desc>"));
    std::ofstream(gl, std::ios::binary) << cut;
    got.clear();
    info = gamelist_scan_file(gl, EntryFn::of(cb));
    CHECK_EQ(got, string("./a.gb,"));
    CHECK(!info.ok);
  }

  std::system(("rm -rf " + d).c_str());
  return zt::done();
}
