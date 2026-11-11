// MODULE `qr`: a QR Code Model 2 encoder (ISO/IEC 18004), our own code, no library. Byte mode only (the text goes in as its UTF-8 bytes),
// versions 1..10 (up to 271 bytes at level L), error correction L / M / Q / H, the smallest version that fits, all eight masks scored by
// the four penalty rules. The output is the matrix of the symbol, dark = true, WITHOUT the quiet zone (the screen adds its four light modules).
// Pure code: no I/O, no globals, nothing allocated that outlives the call. The small helpers at the end are public for the tests.
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace zf {
namespace qr {

enum class Ecc : uint8_t { L = 0, M = 1, Q = 2, H = 3 };

constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 10;

struct Code {
  int version = 0;  // 1..10; 0: empty
  int size = 0;     // modules per side: 17 + 4 * version
  Ecc ecc = Ecc::M;
  int mask = 0;     // the mask pattern chosen (0..7)
  std::vector<uint8_t> dark;  // size * size, row by row, 1 = dark
  bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[size_t(y) * size_t(size) + size_t(x)] != 0; }
};

/// Encodes `text` (bytes) at level `ecc`; the smallest version 1..10 that holds it. False when it does not fit in version 10.
bool encode(std::string_view text, Ecc ecc, Code& out);
/// The same with a mask picked by the caller (0..7) instead of the best one (tests).
bool encode_with_mask(std::string_view text, Ecc ecc, int mask, Code& out);

// ---- pieces (tests) ----------------------------------------------------------------------------------------------------
/// Most bytes version `v` holds in byte mode at level `ecc`; 0 for a version out of range.
int capacity_bytes(int version, Ecc ecc);
/// Data codewords (all blocks) of version `v` at `ecc`, and error correction codewords per block.
int data_codewords_total(int version, Ecc ecc);
int ec_per_block(int version, Ecc ecc);
int block_count(int version, Ecc ecc);
/// The data codewords of `text` (mode, count, bytes, terminator, padding), exactly `data_codewords_total` of them. Empty when it does not fit.
std::vector<uint8_t> data_codewords(std::string_view text, int version, Ecc ecc);
/// The generator polynomial of degree `n` over GF(256) (0x11D) without its leading 1: coefficients of x^(n-1) .. x^0.
std::vector<uint8_t> rs_generator(int n);
/// The `n` error correction codewords of `data` (remainder of data * x^n by the generator).
std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, int n);
/// GF(256) product with the polynomial 0x11D.
uint8_t gf_mul(uint8_t a, uint8_t b);
/// The 15 format bits (after the 0x5412 mask) of (level, mask), the 18 version bits of a version 7..10 (0 below 7).
uint32_t format_bits(Ecc ecc, int mask);
uint32_t version_bits(int version);
/// Centres of the alignment patterns on one axis (version 1: none).
std::vector<int> alignment_positions(int version);
/// Data and error correction codewords interleaved the way they are placed in the symbol.
std::vector<uint8_t> final_codewords(const std::vector<uint8_t>& data, int version, Ecc ecc);

}  // namespace qr
}  // namespace zf
