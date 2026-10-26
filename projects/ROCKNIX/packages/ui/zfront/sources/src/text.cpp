#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "util.hpp"

namespace zf {

Font::~Font() {
  for (auto& f : fb_)
    if (f.face) FT_Done_Face(f.face);
  if (face_) FT_Done_Face(face_);
  if (lib_) FT_Done_FreeType(lib_);
}

bool Font::load(const std::string& path, std::string& err) {
  if (FT_Init_FreeType(&lib_)) {
    err = "FreeType init failed";
    return false;
  }
  if (FT_New_Face(lib_, path.c_str(), 0, &face_)) {
    err = path + ": cannot open font";
    face_ = nullptr;
    return false;
  }
  FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
  cur_px_ = 0;
  // fallback faces: names only now, files are opened on the first code point the main face lacks
  font_dirs_.clear();
  for (auto& f : fb_)
    if (f.face) FT_Done_Face(f.face);
  fb_.clear();
  face_of_.clear();
  font_dirs_.push_back(path_dir(path));
  if (const char* e = std::getenv("ZF_FONT_DIR")) font_dirs_.push_back(e);
  font_dirs_.push_back("/storage/.config/emulationstation/resources");
  font_dirs_.push_back("/usr/config/emulationstation/resources");
  for (const char* n : {"fontawesome-webfont.ttf", "DroidSansFallbackFull.ttf", "PyeojinGothic-Medium.ttf", "Vazirmatn-Regular.ttf"}) {
    Fallback f;
    f.name = n;
    fb_.push_back(f);
  }
  return true;
}

int Font::fallbacks_open() const {
  int n = 0;
  for (const auto& f : fb_) n += f.face != nullptr;
  return n;
}

// FT_Load_Char on a missing code point "succeeds" with glyph 0 (a box), so the check is the char index.
int Font::pick(uint32_t cp) {
  if (cp < 0x80 || fb_.empty()) return 0;
  auto it = face_of_.find(cp);
  if (it != face_of_.end()) return it->second;
  if (face_of_.size() > 8192) face_of_.clear();
  int idx = 0;
  if (!FT_Get_Char_Index(face_, cp)) {
    for (size_t i = 0; i < fb_.size() && !idx; ++i) {
      Fallback& f = fb_[i];
      if (!f.tried) {
        f.tried = true;
        for (const auto& dir : font_dirs_) {
          if (!FT_New_Face(lib_, path_join(dir, f.name).c_str(), 0, &f.face)) {
            FT_Select_Charmap(f.face, FT_ENCODING_UNICODE);
            break;
          }
          f.face = nullptr;
        }
      }
      if (f.face && FT_Get_Char_Index(f.face, cp)) idx = int(i) + 1;
    }
  }
  face_of_[cp] = int8_t(idx);
  return idx;
}

FT_Face Font::face_at(int idx, int px) {
  if (idx == 0) {
    set_px(px);
    return face_;
  }
  Fallback& f = fb_[size_t(idx - 1)];
  if (px < 1) px = 1;
  if (f.px != px) {
    FT_Set_Pixel_Sizes(f.face, 0, static_cast<FT_UInt>(px));
    f.px = px;
  }
  return f.face;
}

void Font::set_px(int px) {
  if (px < 1) px = 1;
  if (px != cur_px_) {
    FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(px));
    cur_px_ = px;
  }
}

float Font::advance(uint32_t cp, int px) {
  uint64_t key = (uint64_t(px) << 32) | cp;
  auto it = adv_.find(key);
  if (it != adv_.end()) return it->second;
  FT_Face ff = face_at(pick(cp), px);
  float a = 0;
  if (!FT_Load_Char(ff, cp, FT_LOAD_DEFAULT)) a = float(ff->glyph->metrics.horiAdvance) / 64.0f;
  adv_[key] = a;
  return a;
}

const Font::Glyph& Font::glyph(uint32_t cp, int px) {
  uint64_t key = (uint64_t(px) << 32) | cp;
  auto it = glyphs_.find(key);
  if (it != glyphs_.end()) return it->second;
  if (glyphs_.size() > 6000) glyphs_.clear();  // CJK-heavy lists: start over rather than grow without bound
  FT_Face ff = face_at(pick(cp), px);
  Glyph g;
  if (!FT_Load_Char(ff, cp, FT_LOAD_RENDER)) {
    FT_GlyphSlot s = ff->glyph;
    const FT_Bitmap& bm = s->bitmap;
    g.ok = true;
    g.bearing_x = float(s->metrics.horiBearingX) / 64.0f;
    g.bearing_y = float(s->metrics.horiBearingY) / 64.0f;
    g.adv = float(s->metrics.horiAdvance) / 64.0f;
    g.w = bm.width, g.h = bm.rows;
    g.cov.resize(size_t(g.w) * g.h);
    for (unsigned row = 0; row < g.h; ++row)
      for (unsigned col = 0; col < g.w; ++col) g.cov[size_t(row) * g.w + col] = bm.buffer[row * size_t(bm.pitch) + col];
  }
  return glyphs_.emplace(key, std::move(g)).first->second;
}

float Font::kerning(uint32_t left, uint32_t right, int px) {
  int fi = pick(left);
  if (fi != pick(right)) return 0;  // kerning only inside one face
  FT_Face ff = face_at(fi, px);
  if (!FT_HAS_KERNING(ff)) return 0;
  FT_Vector d;
  FT_UInt li = FT_Get_Char_Index(ff, left), ri = FT_Get_Char_Index(ff, right);
  if (FT_Get_Kerning(ff, li, ri, FT_KERNING_DEFAULT, &d)) return 0;
  return float(d.x) / 64.0f;
}

float Font::measure(std::string_view s, int px) {
  float x = 0;
  uint32_t prev = 0;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = utf8_next(s, i);
    if (prev) x += kerning(prev, c, px);
    x += advance(c, px);
    prev = c;
  }
  return x;
}

// ES `Font`: every ASCII glyph is rendered at start; the line box is as tall as the tallest one (mMaxGlyphHeight)
// and the baseline sits at (box height + 'S' bearing) / 2, so capital letters are centred in the box.
const Font::SizeInfo& Font::size_info(int px) {
  auto it = info_.find(px);
  if (it != info_.end()) return it->second;
  set_px(px);
  SizeInfo si;
  for (uint32_t c = 32; c < 128; ++c)
    if (!FT_Load_Char(face_, c, FT_LOAD_RENDER)) si.max_h = std::max(si.max_h, int(face_->glyph->bitmap.rows));
  if (!FT_Load_Char(face_, 'S', FT_LOAD_RENDER)) {
    si.cap_top = float(face_->glyph->metrics.horiBearingY) / 64.0f;
    si.letter_h = int(face_->glyph->bitmap.rows);
  } else {
    si.cap_top = 35;
    si.letter_h = px;
  }
  si.max_h = std::max(si.max_h, 1);
  return info_.emplace(px, si).first->second;
}

int Font::line_height(int px) { return size_info(px).max_h; }
int Font::letter_height(int px) { return size_info(px).letter_h; }

Img Font::render(std::string_view s, int px, Color color) {
  const SizeInfo si = size_info(px);
  const float baseline = (float(si.max_h) + si.cap_top) / 2.0f;
  const float width = measure(s, px);
  Img img;
  img.w = uint32_t(std::max(int(std::ceil(width)) + 1, 1));
  img.h = uint32_t(si.max_h);
  img.rgba.assign(size_t(img.w) * img.h * 4, 0);
  float pen = 0;
  uint32_t prev = 0;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = utf8_next(s, i);
    if (prev) pen += kerning(prev, c, px);
    const Glyph& g = glyph(c, px);
    if (g.ok) {
      int gx = int(std::lround(pen + g.bearing_x));
      int gy = int(std::lround(baseline - g.bearing_y));
      for (unsigned row = 0; row < g.h; ++row) {
        for (unsigned col = 0; col < g.w; ++col) {
          int x = gx + int(col), y = gy + int(row);
          if (x < 0 || y < 0 || x >= int(img.w) || y >= int(img.h)) continue;
          uint32_t cov = g.cov[size_t(row) * g.w + col];
          size_t o = (size_t(y) * img.w + size_t(x)) * 4;
          uint32_t a = std::max<uint32_t>(cov * color[3] / 255, img.rgba[o + 3]);
          img.rgba[o] = color[0];
          img.rgba[o + 1] = color[1];
          img.rgba[o + 2] = color[2];
          img.rgba[o + 3] = uint8_t(a);
        }
      }
      pen += g.adv;
    } else {
      pen += advance(c, px);
    }
    prev = c;
  }
  return img;
}

}  // namespace zf
