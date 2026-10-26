#include "scraper_hash.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util.hpp"

namespace zf::scraper {

namespace {

constexpr uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

uint32_t* crc_table() {
  static uint32_t t[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    init = true;
  }
  return t;
}

}  // namespace

Md5::Md5() : s_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

void Md5::block(const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[i * 4]) | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) | (uint32_t(p[i * 4 + 3]) << 24);
  uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + rol(a + f + K[i] + m[g], R[i]);
    a = t;
  }
  s_[0] += a;
  s_[1] += b;
  s_[2] += c;
  s_[3] += d;
}

void Md5::update(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  len_ += n;
  if (fill_) {
    size_t take = std::min(n, sizeof buf_ - fill_);
    std::memcpy(buf_ + fill_, p, take);
    fill_ += take;
    p += take;
    n -= take;
    if (fill_ == sizeof buf_) {
      block(buf_);
      fill_ = 0;
    }
  }
  while (n >= 64) {
    block(p);
    p += 64;
    n -= 64;
  }
  if (n) {
    std::memcpy(buf_, p, n);
    fill_ = n;
  }
}

std::string Md5::hex() {
  uint64_t bits = len_ * 8;
  uint8_t pad[72] = {0x80};
  size_t padlen = (fill_ < 56) ? 56 - fill_ : 120 - fill_;
  for (int i = 0; i < 8; ++i) pad[padlen + size_t(i)] = uint8_t(bits >> (8 * i));
  update(pad, padlen + 8);
  char out[33];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) std::snprintf(out + i * 8 + k * 2, 3, "%02x", unsigned((s_[i] >> (8 * k)) & 0xff));
  return std::string(out, 32);
}

uint32_t crc32_update(uint32_t crc, const void* data, size_t n) {
  const uint32_t* t = crc_table();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

std::string hex8(uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof b, "%08x", unsigned(v));
  return b;
}

bool hash_file(const std::string& path, uint64_t max_bytes, Ident& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (max_bytes && uint64_t(st.st_size) > max_bytes)) {
    ::close(fd);
    return false;
  }
  (void)!::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  std::vector<uint8_t> buf(64 * 1024);
  Md5 md5;
  uint32_t crc = 0;
  uint64_t total = 0;
  for (;;) {
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    md5.update(buf.data(), size_t(n));
    crc = crc32_update(crc, buf.data(), size_t(n));
    total += uint64_t(n);
  }
  ::close(fd);
  out.size = total;
  out.md5 = md5.hex();
  out.crc = hex8(crc);
  out.hashed = true;
  return true;
}

namespace {
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t rd16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
}  // namespace

bool zip_single_entry(const std::string& path, ZipSingle& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0 || st.st_size < 22) {
    ::close(fd);
    return false;
  }
  uint64_t size = uint64_t(st.st_size);
  size_t tail = size_t(std::min<uint64_t>(size, 22 + 65535));
  std::vector<uint8_t> t(tail);
  bool ok = ::pread(fd, t.data(), tail, off_t(size - tail)) == ssize_t(tail);
  size_t eocd = std::string::npos;
  if (ok)
    for (size_t i = tail - 22 + 1; i-- > 0;)
      if (rd32(&t[i]) == 0x06054b50u) {
        eocd = i;
        break;
      }
  uint32_t cd_size = 0, cd_off = 0, count = 0;
  if (eocd == std::string::npos) ok = false;
  else {
    count = rd16(&t[eocd + 10]);
    cd_size = rd32(&t[eocd + 12]);
    cd_off = rd32(&t[eocd + 16]);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) ok = false;  // zip64
    if (uint64_t(cd_off) + cd_size > size || cd_size > (8u << 20)) ok = false;
  }
  std::vector<uint8_t> cd;
  if (ok) {
    cd.resize(cd_size);
    ok = ::pread(fd, cd.data(), cd_size, off_t(cd_off)) == ssize_t(cd_size);
  }
  ::close(fd);
  if (!ok) return false;
  int found = 0;
  size_t p = 0;
  for (uint32_t i = 0; i < count && ok; ++i) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50u) return false;
    uint32_t crc = rd32(&cd[p + 16]), usize = rd32(&cd[p + 24]);
    size_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
    if (p + 46 + nl + xl + cl > cd.size()) return false;
    std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    p += 46 + nl + xl + cl;
    if (!name.empty() && name.back() == '/') continue;
    if (path_ext_lower(name) == ".txt") continue;
    if (usize == 0xFFFFFFFFu) return false;  // zip64 member
    if (++found > 1) return false;
    out.entry = name;
    out.crc = crc;
    out.size = usize;
  }
  return found == 1;
}

bool extract_archives_for(std::string_view platforms) {
  size_t i = 0;
  while (i < platforms.size()) {
    while (i < platforms.size() && (platforms[i] == ' ' || platforms[i] == '\t' || platforms[i] == '\n' || platforms[i] == '\r')) ++i;
    size_t j = i;
    while (j < platforms.size() && !(platforms[j] == ' ' || platforms[j] == '\t' || platforms[j] == '\n' || platforms[j] == '\r')) ++j;
    std::string_view w = platforms.substr(i, j - i);
    for (const char* s : {"arcade", "neogeo", "daphne", "lutro", "dreamcast", "atomiswave", "naomi"})
      if (w == s) return false;
    i = j;
  }
  return true;
}

namespace {

// `7za l -slt FILE`: "Size = N" / "CRC = XXXXXXXX" of the only file (the archive header block has no CRC line).
bool sevenzip_single(CmdRunner& cmd, const std::string& path, Ident& out) {
  std::string q = shell_quote(path);
  CmdResult r = cmd.run("(7za l -slt " + q + " || 7z l -slt " + q + ") 2>/dev/null", 20000);
  if (r.dry || r.exit_code != 0 || r.out.empty()) return false;
  int files = 0;
  std::string crc;
  uint64_t size = 0;
  bool in_files = false;
  size_t pos = 0;
  while (pos < r.out.size()) {
    size_t e = r.out.find('\n', pos);
    if (e == std::string::npos) e = r.out.size();
    std::string_view line = trim(std::string_view(r.out).substr(pos, e - pos));
    pos = e + 1;
    if (line == "----------") in_files = true;
    else if (in_files && starts_with(line, "Path = ")) ++files;
    else if (in_files && starts_with(line, "Size = ")) {
      long v = 0;
      if (parse_int(line.substr(7), v)) size = uint64_t(v);
    } else if (in_files && starts_with(line, "CRC = ")) crc = std::string(trim(line.substr(6)));
    else if (in_files && starts_with(line, "Folder = +")) --files;  // directories do not count
  }
  if (files != 1 || crc.size() != 8) return false;
  for (char& c : crc) c = char(std::tolower(static_cast<unsigned char>(c)));
  out.crc = crc;
  out.size = size;
  out.hashed = true;
  return true;
}

bool first_m3u_target(const std::string& path, std::string& target) {
  std::string t;
  if (!read_file(path, t)) return false;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t e = t.find('\n', pos);
    if (e == std::string::npos) e = t.size();
    std::string_view line = trim(std::string_view(t).substr(pos, e - pos));
    pos = e + 1;
    if (line.empty() || line[0] == '#') continue;
    target = (line[0] == '/') ? std::string(line) : path_join(path_dir(path), line);
    return true;
  }
  return false;
}

}  // namespace

bool identify(CmdRunner& cmd, const std::string& path, bool extract_archives, Ident& out) {
  out = Ident();
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  if (S_ISDIR(st.st_mode)) return true;  // a folder game: nothing to hash
  std::string file = path;
  std::string ext = path_ext_lower(file);
  if (ext == ".m3u") {
    std::string target;
    if (first_m3u_target(file, target) && ::stat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      file = target;
      ext = path_ext_lower(file);
    }
  }
  out.size = uint64_t(st.st_size);
  if (extract_archives && ext == ".zip") {
    ZipSingle z;
    if (zip_single_entry(file, z)) {
      out.crc = hex8(z.crc);
      out.size = z.size;
      out.hashed = true;
      return true;
    }
  } else if (extract_archives && ext == ".7z") {
    if (sevenzip_single(cmd, file, out)) return true;
  }
  if (out.size > kMaxHashBytes) return true;  // too big (ES: no hash above 128 MB): the name alone is searched
  Ident h;
  if (!hash_file(file, kMaxHashBytes, h)) return true;
  out = h;
  return true;
}

}  // namespace zf::scraper
