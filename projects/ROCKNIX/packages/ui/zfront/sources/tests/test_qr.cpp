// qr: the QR Code encoder against values of ISO/IEC 18004 worked out independently of the encoder (Reed-Solomon of the classic
// "HELLO WORLD" 1-M symbol, generator polynomials, BCH of format and version information), structure of the symbol, capacity
// limits and masks. With ZF_QR_DUMP=<dir> it writes PGM pictures (4 light modules of quiet zone, 8 px per module) that an
// independent decoder reads back (C:\tmp\zt\x10\qrdecode\qr_decode.py).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.hpp"
#include "qr.hpp"

using namespace zf;
using std::string;

namespace {

uint8_t alpha(int e) {  // α^e in GF(256) / 0x11D, by repeated doubling (independent of the encoder's tables)
  unsigned v = 1;
  for (int i = 0; i < e; ++i) {
    v <<= 1;
    if (v & 0x100) v ^= 0x11D;
  }
  return uint8_t(v);
}

void test_gf_and_rs() {
  CHECK_EQ(int(qr::gf_mul(0, 77)), 0);
  CHECK_EQ(int(qr::gf_mul(1, 77)), 77);
  CHECK_EQ(int(qr::gf_mul(2, 0x80)), 0x1D);  // x * x^7 = x^8 = x^4 + x^3 + x^2 + 1
  for (int a = 1; a < 256; a += 17)
    for (int b = 1; b < 256; b += 23) CHECK_EQ(qr::gf_mul(uint8_t(a), uint8_t(b)), qr::gf_mul(uint8_t(b), uint8_t(a)));
  // generator of degree 10, exponents of α from x^9 down to x^0 (thonky.com / ISO 18004 annex A)
  const int g10[] = {251, 67, 46, 61, 118, 70, 64, 94, 32, 45};
  std::vector<uint8_t> g = qr::rs_generator(10);
  CHECK_EQ(g.size(), size_t(10));
  for (size_t i = 0; i < g.size() && i < 10; ++i) CHECK_EQ(int(g[i]), int(alpha(g10[i])));
  // degree 7 (version 1-L): α^87 α^229 α^146 α^149 α^238 α^102 α^21
  const int g7[] = {87, 229, 146, 149, 238, 102, 21};
  g = qr::rs_generator(7);
  CHECK_EQ(g.size(), size_t(7));
  for (size_t i = 0; i < g.size() && i < 7; ++i) CHECK_EQ(int(g[i]), int(alpha(g7[i])));
  // the data codewords of "HELLO WORLD" at 1-M (alphanumeric) and their 10 error correction codewords
  const std::vector<uint8_t> data = {32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17};
  const std::vector<uint8_t> ec = {196, 35, 39, 119, 235, 215, 231, 226, 93, 23};
  CHECK(qr::rs_remainder(data, 10) == ec);
}

// format: 2 bits of level (L 01, M 00, Q 11, H 10) + 3 of mask, BCH(15,5) with 0x537, XOR 0x5412
uint32_t ref_format(qr::Ecc e, int mask) {
  const unsigned lv[] = {1, 0, 3, 2};
  uint32_t d = (lv[int(e)] << 3) | unsigned(mask), r = d << 10;
  for (int i = 14; i >= 10; --i)
    if (r & (1u << i)) r ^= 0x537u << (i - 10);
  return ((d << 10) | r) ^ 0x5412;
}

void test_format_version() {
  for (int e = 0; e < 4; ++e)
    for (int m = 0; m < 8; ++m) CHECK_EQ(qr::format_bits(qr::Ecc(e), m), ref_format(qr::Ecc(e), m));
  CHECK_EQ(qr::format_bits(qr::Ecc::M, 0), 0x5412u ^ 0x0000u);  // 101010000010010: M = 00 and mask 0 are all zero bits
  CHECK_EQ(qr::format_bits(qr::Ecc::L, 4), 0x662Fu);  // 110011000101111, the table in the standard
  CHECK_EQ(qr::version_bits(6), 0u);
  CHECK_EQ(qr::version_bits(7), 0x07C94u);
  CHECK_EQ(qr::version_bits(8), 0x085BCu);
  CHECK_EQ(qr::version_bits(9), 0x09A99u);
  CHECK_EQ(qr::version_bits(10), 0x0A4D3u);
  CHECK(qr::alignment_positions(1).empty());
  CHECK(qr::alignment_positions(2) == (std::vector<int>{6, 18}));
  CHECK(qr::alignment_positions(7) == (std::vector<int>{6, 22, 38}));
  CHECK(qr::alignment_positions(10) == (std::vector<int>{6, 28, 50}));
}

void test_capacity() {
  struct Cap {
    int v;
    int l, m, q, h;
  } caps[] = {{1, 17, 14, 11, 7}, {2, 32, 26, 20, 14}, {5, 106, 84, 60, 44}, {7, 154, 122, 86, 64}, {10, 271, 213, 151, 119}};
  for (const Cap& c : caps) {
    CHECK_EQ(qr::capacity_bytes(c.v, qr::Ecc::L), c.l);
    CHECK_EQ(qr::capacity_bytes(c.v, qr::Ecc::M), c.m);
    CHECK_EQ(qr::capacity_bytes(c.v, qr::Ecc::Q), c.q);
    CHECK_EQ(qr::capacity_bytes(c.v, qr::Ecc::H), c.h);
  }
  CHECK_EQ(qr::capacity_bytes(0, qr::Ecc::M), 0);
  CHECK_EQ(qr::capacity_bytes(11, qr::Ecc::M), 0);
  // totals of ISO 18004 table 9: data codewords and blocks
  CHECK_EQ(qr::data_codewords_total(1, qr::Ecc::M), 16);
  CHECK_EQ(qr::ec_per_block(1, qr::Ecc::M), 10);
  CHECK_EQ(qr::data_codewords_total(5, qr::Ecc::Q), 62);
  CHECK_EQ(qr::block_count(5, qr::Ecc::Q), 4);
  CHECK_EQ(qr::ec_per_block(5, qr::Ecc::Q), 18);
  CHECK_EQ(qr::data_codewords_total(10, qr::Ecc::H), 122);
  CHECK_EQ(qr::block_count(10, qr::Ecc::H), 8);
  // the largest text of each version fits there; one byte more goes to the next version
  for (int v = 1; v <= 10; ++v) {
    const int n = qr::capacity_bytes(v, qr::Ecc::M);
    qr::Code c;
    CHECK(qr::encode(string(size_t(n), 'a'), qr::Ecc::M, c));
    CHECK_EQ(c.version, v);
    CHECK_EQ(c.size, 17 + 4 * v);
    const bool fits = qr::encode(string(size_t(n) + 1, 'a'), qr::Ecc::M, c);
    if (v < 10) {
      CHECK(fits);
      CHECK_EQ(c.version, v + 1);
    } else {
      CHECK(!fits);
    }
  }
  CHECK_EQ(qr::data_codewords(string(15, 'x'), 1, qr::Ecc::M).size(), size_t(0));
  std::vector<uint8_t> d = qr::data_codewords("AB", 1, qr::Ecc::M);
  CHECK_EQ(d.size(), size_t(16));
  if (d.size() == 16) {  // 0100 (byte) 00000010 (count) 'A' 'B' 0000 then 236 17 ...
    CHECK_EQ(int(d[0]), 0x40);
    CHECK_EQ(int(d[1]), 0x24);
    CHECK_EQ(int(d[2]), 0x14);
    CHECK_EQ(int(d[3]), 0x20);
    CHECK_EQ(int(d[4]), 236);
    CHECK_EQ(int(d[5]), 17);
  }
}

bool finder_at(const qr::Code& c, int x0, int y0) {
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 7; ++x) {
      const bool ring = x == 0 || y == 0 || x == 6 || y == 6, core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
      if (c.at(x0 + x, y0 + y) != (ring || core)) return false;
    }
  return true;
}

void test_structure() {
  for (const char* text : {"x", "http://192.168.1.42:8080/files#pin=1365", "http://[fe80::1234:5678:9abc:def0]:8080/files#pin=0042"}) {
    for (int e = 0; e < 4; ++e) {
      qr::Code c;
      if (!qr::encode(text, qr::Ecc(e), c)) continue;
      const int n = c.size;
      CHECK_EQ(int(c.dark.size()), n * n);
      CHECK(finder_at(c, 0, 0));
      CHECK(finder_at(c, n - 7, 0));
      CHECK(finder_at(c, 0, n - 7));
      for (int i = 0; i < 8; ++i) {  // separators are light
        CHECK(!c.at(7, i) && !c.at(i, 7));
        CHECK(!c.at(n - 8, i) && !c.at(n - 1 - i, 7));
        CHECK(!c.at(7, n - 1 - i) && !c.at(i, n - 8));
      }
      for (int i = 8; i < n - 8; ++i) {  // timing patterns
        CHECK_EQ(c.at(i, 6), i % 2 == 0);
        CHECK_EQ(c.at(6, i), i % 2 == 0);
      }
      CHECK(c.at(8, n - 8));  // the dark module
      const std::vector<int> al = qr::alignment_positions(c.version);
      for (int ay : al)
        for (int ax : al) {
          if ((ax == 6 && ay == 6) || (ax == 6 && ay == n - 7) || (ax == n - 7 && ay == 6)) continue;
          CHECK(c.at(ax, ay));
          CHECK(!c.at(ax + 1, ay) && !c.at(ax, ay - 1));
          CHECK(c.at(ax + 2, ay + 2) && c.at(ax - 2, ay - 2));
        }
      // the format information read back (copy around the top-left finder) is the one for (level, mask)
      uint32_t f = 0;
      for (int i = 0; i <= 5; ++i) f = (f << 1) | (c.at(8, i) ? 1u : 0u);
      f = (f << 1) | (c.at(8, 7) ? 1u : 0u);
      f = (f << 1) | (c.at(8, 8) ? 1u : 0u);
      f = (f << 1) | (c.at(7, 8) ? 1u : 0u);
      for (int i = 5; i >= 0; --i) f = (f << 1) | (c.at(i, 8) ? 1u : 0u);
      // bits 14..0 are read in the order of ISO 18004 figure 25 (bit 14 first at (8,0)); some encoders store it the other way
      uint32_t rev = 0;
      for (int b = 0; b < 15; ++b) rev |= ((f >> b) & 1u) << (14 - b);
      const uint32_t want = qr::format_bits(c.ecc, c.mask);
      CHECK(f == want || rev == want);
      CHECK_EQ(c.ecc, qr::Ecc(e));
    }
  }
  qr::Code a, b;
  CHECK(qr::encode_with_mask("mask test", qr::Ecc::M, 3, a));
  CHECK_EQ(a.mask, 3);
  CHECK(qr::encode_with_mask("mask test", qr::Ecc::M, 5, b));
  CHECK(a.dark != b.dark);
  CHECK(!qr::encode_with_mask("mask test", qr::Ecc::M, 8, b));
  CHECK(!qr::encode(string(300, 'z'), qr::Ecc::L, b));
}

void dump(const string& dir, const string& name, const qr::Code& c) {
  const int q = 4, px = 8, side = (c.size + 2 * q) * px;
  FILE* f = std::fopen((dir + "/" + name + ".pgm").c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P5\n%d %d\n255\n", side, side);
  std::vector<unsigned char> row(static_cast<size_t>(side));
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) row[size_t(x)] = c.at(x / px - q, y / px - q) ? 0 : 255;
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

}  // namespace

int main() {
  test_gf_and_rs();
  test_format_version();
  test_capacity();
  test_structure();
  if (const char* d = std::getenv("ZF_QR_DUMP")) {
    struct T {
      const char* name;
      const char* text;
      qr::Ecc e;
    } ts[] = {{"url-m", "http://192.168.1.42:8080/files#pin=1365", qr::Ecc::M},
              {"short-l", "zfront", qr::Ecc::L},
              {"v7-q", "http://192.168.100.200:8080/files#pin=9999 and a longer text so that the symbol needs version seven or more....", qr::Ecc::Q},
              {"utf8-h", "Pok\xC3\xA9mon \xE2\x98\x85 consola", qr::Ecc::H}};
    for (const T& t : ts) {
      qr::Code c;
      CHECK(qr::encode(t.text, t.e, c));
      dump(d, t.name, c);
      std::printf("%s v%d mask %d\n", t.name, c.version, c.mask);
    }
  }
  return zt::done();
}
