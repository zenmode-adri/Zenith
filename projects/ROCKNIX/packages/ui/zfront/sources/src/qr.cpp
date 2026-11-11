#include "qr.hpp"

#include <algorithm>
#include <cstdlib>

namespace zf {
namespace qr {

namespace {

// Block structure per version (rows 1..10) and level (L, M, Q, H): error correction codewords per block, blocks of the first group and
// their data codewords, blocks of the second group (one more data codeword each) and theirs. ISO/IEC 18004 table 9.
struct Blk {
  uint8_t ec, n1, d1, n2, d2;
};
const Blk kBlk[kMaxVersion][4] = {
    {{7, 1, 19, 0, 0}, {10, 1, 16, 0, 0}, {13, 1, 13, 0, 0}, {17, 1, 9, 0, 0}},
    {{10, 1, 34, 0, 0}, {16, 1, 28, 0, 0}, {22, 1, 22, 0, 0}, {28, 1, 16, 0, 0}},
    {{15, 1, 55, 0, 0}, {26, 1, 44, 0, 0}, {18, 2, 17, 0, 0}, {22, 2, 13, 0, 0}},
    {{20, 1, 80, 0, 0}, {18, 2, 32, 0, 0}, {26, 2, 24, 0, 0}, {16, 4, 9, 0, 0}},
    {{26, 1, 108, 0, 0}, {24, 2, 43, 0, 0}, {18, 2, 15, 2, 16}, {22, 2, 11, 2, 12}},
    {{18, 2, 68, 0, 0}, {16, 4, 27, 0, 0}, {24, 4, 19, 0, 0}, {28, 4, 15, 0, 0}},
    {{20, 2, 78, 0, 0}, {18, 4, 31, 0, 0}, {18, 2, 14, 4, 15}, {26, 4, 13, 1, 14}},
    {{24, 2, 97, 0, 0}, {22, 2, 38, 2, 39}, {22, 4, 18, 2, 19}, {26, 4, 14, 2, 15}},
    {{30, 2, 116, 0, 0}, {22, 3, 36, 2, 37}, {20, 4, 16, 4, 17}, {24, 4, 12, 4, 13}},
    {{18, 2, 68, 2, 69}, {26, 4, 43, 1, 44}, {24, 6, 19, 2, 20}, {28, 6, 15, 2, 16}},
};

const Blk* blk(int v, Ecc e) {
  if (v < kMinVersion || v > kMaxVersion) return nullptr;
  return &kBlk[v - 1][int(e)];
}

struct Gf {
  uint8_t ex[512];
  uint8_t lg[256];
  Gf() {
    unsigned x = 1;
    for (int i = 0; i < 255; ++i) {
      ex[i] = uint8_t(x);
      lg[x] = uint8_t(i);
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; ++i) ex[i] = ex[i - 255];
    lg[0] = 0;
  }
};
const Gf& gf() {
  static const Gf g;
  return g;
}

int total_codewords(int v) {
  static const int t[kMaxVersion] = {26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
  return t[v - 1];
}

int count_bits(int v) { return v <= 9 ? 8 : 16; }  // character count indicator of the byte mode

struct Bits {
  std::vector<uint8_t> b;  // one bit per element, simple and small
  void put(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t((value >> i) & 1u));
  }
};

// ---- the symbol --------------------------------------------------------------------------------------------------------

class Symbol {
 public:
  Symbol(int v, Ecc e) : v_(v), e_(e), n_(17 + 4 * v), m_(size_t(n_) * size_t(n_), 0), fn_(size_t(n_) * size_t(n_), 0) {
    for (int i = 0; i < n_; ++i) {  // timing
      set_fn(6, i, i % 2 == 0);
      set_fn(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(n_ - 4, 3);
    finder(3, n_ - 4);
    const std::vector<int> pos = alignment_positions(v);
    const size_t k = pos.size();
    for (size_t i = 0; i < k; ++i)
      for (size_t j = 0; j < k; ++j)
        if (!((i == 0 && j == 0) || (i == 0 && j == k - 1) || (i == k - 1 && j == 0))) alignment(pos[i], pos[j]);
    format(0);  // reserves the format areas (and the dark module)
    version();
  }

  // The remainder bits (7 in versions 2..6) stay light: the modules left over after the codewords are not written.
  void place(const std::vector<uint8_t>& cw) {
    size_t i = 0;
    const size_t total = cw.size() * 8;
    for (int right = n_ - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;
      for (int vert = 0; vert < n_; ++vert)
        for (int j = 0; j < 2; ++j) {
          const int x = right - j;
          const bool upward = ((right + 1) & 2) == 0;
          const int y = upward ? n_ - 1 - vert : vert;
          if (!fn_[idx(x, y)] && i < total) {
            m_[idx(x, y)] = uint8_t((cw[i >> 3] >> (7 - (i & 7))) & 1);
            ++i;
          }
        }
    }
  }

  void mask(int k) {
    for (int y = 0; y < n_; ++y)
      for (int x = 0; x < n_; ++x) {
        bool inv = false;
        switch (k) {
          case 0: inv = (x + y) % 2 == 0; break;
          case 1: inv = y % 2 == 0; break;
          case 2: inv = x % 3 == 0; break;
          case 3: inv = (x + y) % 3 == 0; break;
          case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
          case 5: inv = x * y % 2 + x * y % 3 == 0; break;
          case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
          default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (inv && !fn_[idx(x, y)]) m_[idx(x, y)] ^= 1;
      }
  }

  void format(int k) {
    const uint32_t bits = format_bits(e_, k);
    auto bit = [bits](int i) { return ((bits >> i) & 1u) != 0; };
    for (int i = 0; i <= 5; ++i) set_fn(8, i, bit(i));
    set_fn(8, 7, bit(6));
    set_fn(8, 8, bit(7));
    set_fn(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set_fn(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) set_fn(n_ - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set_fn(8, n_ - 15 + i, bit(i));
    set_fn(8, n_ - 8, true);  // the dark module
  }

  // The four penalty rules of the standard (3 + run - 5 for runs, 3 per 2x2 block, 40 per finder-like pattern, 10 per 5 % off 50 %).
  int penalty() const {
    int p = 0;
    for (int dir = 0; dir < 2; ++dir)  // rows, then columns
      for (int a = 0; a < n_; ++a) {
        int run = 1;
        for (int b = 1; b < n_; ++b) {
          if (get(dir, a, b) == get(dir, a, b - 1)) {
            ++run;
            if (run == 5) p += 3;
            else if (run > 5) ++p;
          } else {
            run = 1;
          }
        }
      }
    for (int y = 0; y + 1 < n_; ++y)
      for (int x = 0; x + 1 < n_; ++x) {
        const uint8_t c = m_[idx(x, y)];
        if (c == m_[idx(x + 1, y)] && c == m_[idx(x, y + 1)] && c == m_[idx(x + 1, y + 1)]) p += 3;
      }
    static const uint8_t pa[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static const uint8_t pb[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    for (int dir = 0; dir < 2; ++dir)
      for (int a = 0; a < n_; ++a)
        for (int s = -10; s < n_; ++s) {  // outside the symbol counts as light, like the quiet zone
          bool ma = true, mb = true;
          for (int t = 0; t < 11 && (ma || mb); ++t) {
            const int b = s + t;
            const uint8_t c = b >= 0 && b < n_ ? get(dir, a, b) : 0;
            if (c != pa[t]) ma = false;
            if (c != pb[t]) mb = false;
          }
          if (ma) p += 40;
          if (mb) p += 40;
        }
    long dark = 0;
    for (uint8_t c : m_) dark += c;
    const long total = long(n_) * long(n_);
    const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    p += int(k > 0 ? k : 0) * 10;
    return p;
  }

  int size() const { return n_; }
  const std::vector<uint8_t>& modules() const { return m_; }
  uint8_t at(int x, int y) const { return m_[idx(x, y)]; }
  bool is_function(int x, int y) const { return fn_[idx(x, y)] != 0; }

 private:
  size_t idx(int x, int y) const { return size_t(y) * size_t(n_) + size_t(x); }
  uint8_t get(int dir, int a, int b) const { return dir == 0 ? m_[idx(b, a)] : m_[idx(a, b)]; }
  void set_fn(int x, int y, bool dark) {
    m_[idx(x, y)] = dark ? 1 : 0;
    fn_[idx(x, y)] = 1;
  }
  void finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx) {
        const int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
        if (x >= 0 && x < n_ && y >= 0 && y < n_) set_fn(x, y, d != 2 && d != 4);
      }
  }
  void alignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) set_fn(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
  }
  void version() {
    const uint32_t bits = version_bits(v_);
    if (bits == 0) return;
    for (int i = 0; i < 18; ++i) {
      const bool b = ((bits >> i) & 1u) != 0;
      const int a = n_ - 11 + i % 3, c = i / 3;
      set_fn(a, c, b);
      set_fn(c, a, b);
    }
  }

  int v_;
  Ecc e_;
  int n_;
  std::vector<uint8_t> m_, fn_;
};

bool build(std::string_view text, Ecc ecc, int mask, Code& out) {
  int v = kMinVersion;
  while (v <= kMaxVersion && int(text.size()) > capacity_bytes(v, ecc)) ++v;
  if (v > kMaxVersion) return false;
  const std::vector<uint8_t> cw = final_codewords(data_codewords(text, v, ecc), v, ecc);
  Symbol s(v, ecc);
  s.place(cw);
  int best = mask, best_p = 0;
  if (mask < 0) {
    for (int k = 0; k < 8; ++k) {
      s.mask(k);
      s.format(k);
      const int p = s.penalty();
      s.mask(k);  // undo
      if (best < 0 || p < best_p) best = k, best_p = p;
    }
  }
  s.mask(best);
  s.format(best);
  out.version = v;
  out.size = s.size();
  out.ecc = ecc;
  out.mask = best;
  out.dark = s.modules();
  return true;
}

}  // namespace

// ---- public pieces -----------------------------------------------------------------------------------------------------

uint8_t gf_mul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) return 0;
  const Gf& g = gf();
  return g.ex[int(g.lg[a]) + int(g.lg[b])];
}

std::vector<uint8_t> rs_generator(int n) {
  std::vector<uint8_t> r(size_t(n > 0 ? n : 0), 0);
  if (n <= 0) return r;
  r[size_t(n) - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < n; ++i) {
    for (size_t j = 0; j < r.size(); ++j) {
      r[j] = gf_mul(r[j], root);
      if (j + 1 < r.size()) r[j] ^= r[j + 1];
    }
    root = gf_mul(root, 2);
  }
  return r;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, int n) {
  const std::vector<uint8_t> gen = rs_generator(n);
  std::vector<uint8_t> r(gen.size(), 0);
  for (uint8_t b : data) {
    const uint8_t factor = uint8_t(b ^ r[0]);
    r.erase(r.begin());
    r.push_back(0);
    for (size_t i = 0; i < r.size(); ++i) r[i] ^= gf_mul(gen[i], factor);
  }
  return r;
}

int block_count(int version, Ecc ecc) {
  const Blk* b = blk(version, ecc);
  return b ? b->n1 + b->n2 : 0;
}

int ec_per_block(int version, Ecc ecc) {
  const Blk* b = blk(version, ecc);
  return b ? b->ec : 0;
}

int data_codewords_total(int version, Ecc ecc) {
  const Blk* b = blk(version, ecc);
  return b ? b->n1 * b->d1 + b->n2 * b->d2 : 0;
}

int capacity_bytes(int version, Ecc ecc) {
  const Blk* b = blk(version, ecc);
  if (!b) return 0;
  const int bits = data_codewords_total(version, ecc) * 8 - 4 - count_bits(version);
  return bits > 0 ? bits / 8 : 0;
}

std::vector<uint8_t> data_codewords(std::string_view text, int version, Ecc ecc) {
  std::vector<uint8_t> out;
  if (int(text.size()) > capacity_bytes(version, ecc)) return out;
  const size_t cap = size_t(data_codewords_total(version, ecc)) * 8;
  Bits b;
  b.put(0x4, 4);  // byte mode
  b.put(uint32_t(text.size()), count_bits(version));
  for (char c : text) b.put(uint8_t(c), 8);
  b.put(0, int(std::min<size_t>(4, cap - b.b.size())));  // terminator
  while (b.b.size() % 8 != 0) b.b.push_back(0);
  out.assign(b.b.size() / 8, 0);
  for (size_t i = 0; i < b.b.size(); ++i) out[i >> 3] = uint8_t(out[i >> 3] | (b.b[i] << (7 - (i & 7))));
  for (uint8_t pad = 0xEC; out.size() < cap / 8; pad = uint8_t(pad ^ (0xEC ^ 0x11))) out.push_back(pad);
  return out;
}

uint32_t format_bits(Ecc ecc, int mask) {
  static const uint32_t level[4] = {1, 0, 3, 2};  // L, M, Q, H
  const uint32_t data = (level[int(ecc)] << 3) | uint32_t(mask & 7);
  uint32_t rem = data;
  for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
  return ((data << 10) | rem) ^ 0x5412u;
}

uint32_t version_bits(int version) {
  if (version < 7 || version > kMaxVersion) return 0;
  uint32_t rem = uint32_t(version);
  for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25u);
  return (uint32_t(version) << 12) | rem;
}

std::vector<int> alignment_positions(int version) {
  switch (version) {
    case 2: return {6, 18};
    case 3: return {6, 22};
    case 4: return {6, 26};
    case 5: return {6, 30};
    case 6: return {6, 34};
    case 7: return {6, 22, 38};
    case 8: return {6, 24, 42};
    case 9: return {6, 26, 46};
    case 10: return {6, 28, 50};
    default: return {};
  }
}

std::vector<uint8_t> final_codewords(const std::vector<uint8_t>& data, int version, Ecc ecc) {
  std::vector<uint8_t> out;
  const Blk* b = blk(version, ecc);
  if (!b || int(data.size()) != data_codewords_total(version, ecc)) return out;
  std::vector<std::vector<uint8_t>> dat, ecs;
  size_t at = 0;
  for (int i = 0; i < b->n1 + b->n2; ++i) {
    const size_t len = size_t(i < b->n1 ? b->d1 : b->d2);
    dat.emplace_back(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + len));
    ecs.push_back(rs_remainder(dat.back(), b->ec));
    at += len;
  }
  const size_t longest = size_t(b->n2 ? b->d2 : b->d1);
  for (size_t i = 0; i < longest; ++i)
    for (const auto& d : dat)
      if (i < d.size()) out.push_back(d[i]);
  for (int i = 0; i < b->ec; ++i)
    for (const auto& e : ecs) out.push_back(e[size_t(i)]);
  if (int(out.size()) != total_codewords(version)) out.clear();  // the table says what the symbol holds
  return out;
}

bool encode(std::string_view text, Ecc ecc, Code& out) { return build(text, ecc, -1, out); }

bool encode_with_mask(std::string_view text, Ecc ecc, int mask, Code& out) { return mask >= 0 && mask < 8 && build(text, ecc, mask, out); }

}  // namespace qr
}  // namespace zf
