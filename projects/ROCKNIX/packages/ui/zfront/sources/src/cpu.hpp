// Software compositor: executes the draw operations into an RGBA buffer. Used by the headless harness (`--preview`,
// PNG for review and pixel diffs) and by tests; the device uses the SDL renderer (sdlr.hpp) with the same operations.
#pragma once
#include <string>
#include <vector>

#include "gfx.hpp"

namespace zf {

struct Canvas {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> px;  // straight RGBA, opaque after the first background rect
};

/// How images are scaled: `CatmullRom` is what the Rust prototype used (default, pixel diffs against it);
/// `Triangle` is closer to the GPU's linear filter (`--cpu-filter triangle`).
enum class Sampling { CatmullRom, Triangle };

Canvas cpu_render(const std::vector<Op>& ops, const Assets& a, uint32_t w, uint32_t h, Sampling sampling = Sampling::CatmullRom);
/// Uncompressed (stored-deflate) PNG: no zlib dependency, fine for test artifacts.
bool save_png(const Canvas& cv, const std::string& path, std::string& err);

}  // namespace zf
