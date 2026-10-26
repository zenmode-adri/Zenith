// ROM identification for the scraper: MD5 + CRC32 in one streaming pass (64 KB blocks, the ROM is never loaded whole),
// the CRC and size of the single file inside a .zip (read from its central directory: nothing is decompressed) or a .7z
// (`7za l -slt`), and the rules of ES (`ApiSystem::getMD5/getCRC32`): archives are looked into except for arcade-like
// systems, whose ROM IS the zip; nothing is hashed above 128 MB; a .m3u is identified by its first disc.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "services.hpp"

namespace zf::scraper {

/// RFC 1321.
class Md5 {
 public:
  Md5();
  void update(const void* data, size_t n);
  /// 32 lower-case hex digits; the object is spent afterwards.
  std::string hex();

 private:
  void block(const uint8_t* p);
  uint32_t s_[4];
  uint64_t len_ = 0;
  uint8_t buf_[64];
  size_t fill_ = 0;
};

uint32_t crc32_update(uint32_t crc, const void* data, size_t n);  // start with 0
std::string hex8(uint32_t v);                                      // lower-case, 8 digits

struct Ident {
  uint64_t size = 0;          // size of the ROM content (for a zip/7z with one file: the file's uncompressed size)
  std::string md5, crc;       // lower-case hex; empty when unknown
  bool hashed = false;        // md5/crc/size describe the content (false: only `size` of the file itself, or nothing)
};

/// Streams `path` once. False if it cannot be read. `max_bytes` 0 = no limit (bigger files return false).
bool hash_file(const std::string& path, uint64_t max_bytes, Ident& out);

struct ZipSingle {
  std::string entry;
  uint32_t crc = 0;
  uint64_t size = 0;
};
/// The only non-directory, non-.txt entry of the zip (central directory only). False for zero or several entries,
/// zip64 sizes or a damaged file.
bool zip_single_entry(const std::string& path, ZipSingle& out);

/// ES `shouldExtractHashesFromArchives`: false for arcade, neogeo, daphne, lutro, dreamcast, atomiswave, naomi (their ROM
/// is the archive itself). `platforms` is the space separated `<platform>` text of es_systems.cfg.
bool extract_archives_for(std::string_view platforms);

/// Identification as ES does it. `cmd` runs `7za l -slt` for .7z. Nothing for directories, files above 128 MB (only the
/// size is filled) or unreadable files (`ok` false: size 0).
bool identify(CmdRunner& cmd, const std::string& path, bool extract_archives, Ident& out);

constexpr uint64_t kMaxHashBytes = 128ull * 1024 * 1024;

}  // namespace zf::scraper
