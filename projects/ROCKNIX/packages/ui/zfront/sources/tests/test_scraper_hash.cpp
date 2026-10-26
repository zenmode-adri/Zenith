// scraper_hash: MD5/CRC32 vectors, streaming over a file, zip central directory, ES rules (arcade zips, size limit, m3u).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "scraper_hash.hpp"
#include "services.hpp"

using namespace zf;
using namespace zf::scraper;
namespace fs = std::filesystem;

static void spit(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}
static std::string md5_of(const std::string& s) {
  Md5 m;
  m.update(s.data(), s.size());
  return m.hex();
}

// a stored (uncompressed) zip with the given entries; `crc` of each entry is the true one
static std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
  auto le32 = [](std::string& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(char((v >> (8 * i)) & 0xff));
  };
  auto le16 = [](std::string& o, uint32_t v) {
    for (int i = 0; i < 2; ++i) o.push_back(char((v >> (8 * i)) & 0xff));
  };
  std::string out, cd;
  for (auto& f : files) {
    uint32_t crc = crc32_update(0, f.second.data(), f.second.size());
    uint32_t off = uint32_t(out.size());
    le32(out, 0x04034b50u), le16(out, 20), le16(out, 0), le16(out, 0), le16(out, 0), le16(out, 0);
    le32(out, crc), le32(out, uint32_t(f.second.size())), le32(out, uint32_t(f.second.size())), le16(out, uint32_t(f.first.size())), le16(out, 0);
    out += f.first + f.second;
    le32(cd, 0x02014b50u), le16(cd, 20), le16(cd, 20), le16(cd, 0), le16(cd, 0), le16(cd, 0), le16(cd, 0);
    le32(cd, crc), le32(cd, uint32_t(f.second.size())), le32(cd, uint32_t(f.second.size())), le16(cd, uint32_t(f.first.size())), le16(cd, 0), le16(cd, 0);
    le16(cd, 0), le16(cd, 0), le32(cd, 0), le32(cd, off);
    cd += f.first;
  }
  uint32_t cdoff = uint32_t(out.size());
  out += cd;
  le32(out, 0x06054b50u), le16(out, 0), le16(out, 0), le16(out, uint32_t(files.size())), le16(out, uint32_t(files.size()));
  le32(out, uint32_t(cd.size())), le32(out, cdoff), le16(out, 0);
  return out;
}

int main() {
  // --- vectors
  CHECK_EQ(md5_of(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
  CHECK_EQ(md5_of("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
  CHECK_EQ(md5_of("The quick brown fox jumps over the lazy dog"), std::string("9e107d9d372bb6826bd81d3542a419d6"));
  CHECK_EQ(md5_of(std::string(1000, 'a')), std::string("cabe45dcc9ae5b66ba86600cca6b8ba8"));
  {  // incremental updates in odd pieces equal one update (block boundaries)
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + (i * 7) % 26);
    Md5 a, b;
    a.update(big.data(), big.size());
    for (size_t i = 0; i < big.size();) {
      size_t n = std::min<size_t>(1 + (i % 97), big.size() - i);
      b.update(big.data() + i, n);
      i += n;
    }
    CHECK_EQ(a.hex(), b.hex());
  }
  CHECK_EQ(hex8(crc32_update(0, "123456789", 9)), std::string("cbf43926"));
  CHECK_EQ(hex8(crc32_update(crc32_update(0, "1234", 4), "56789", 5)), std::string("cbf43926"));

  char dir[] = "/tmp/zf-hash-XXXXXX";
  std::string root = ::mkdtemp(dir);
  CmdRunner cmd;
  cmd.dry_run = true;

  // --- a plain file: both digests, size
  spit(root + "/a.bin", "abc");
  Ident id;
  CHECK(identify(cmd, root + "/a.bin", true, id));
  CHECK(id.hashed);
  CHECK_EQ(id.size, 3u);
  CHECK_EQ(id.md5, std::string("900150983cd24fb0d6963f7d28e17f72"));
  CHECK_EQ(id.crc, std::string("352441c2"));

  // --- a bigger file (several 64 KB blocks) against md5sum
  {
    std::string big(300000, 'q');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char(i * 31 + (i >> 8));
    spit(root + "/big.bin", big);
    CHECK(identify(cmd, root + "/big.bin", true, id));
    CHECK_EQ(id.size, 300000u);
    CHECK_EQ(id.md5, md5_of(big));
    CHECK_EQ(id.crc, hex8(crc32_update(0, big.data(), big.size())));
  }

  // --- zip with one ROM (and a readme): CRC and size come from the directory, nothing is read
  std::string rom = "ROMDATA-0123456789";
  spit(root + "/one.zip", make_zip({{"game.sfc", rom}, {"readme.txt", "hi"}, {"dir/", ""}}));
  ZipSingle z;
  CHECK(zip_single_entry(root + "/one.zip", z));
  CHECK_EQ(z.entry, std::string("game.sfc"));
  CHECK_EQ(z.size, uint64_t(rom.size()));
  CHECK_EQ(hex8(z.crc), hex8(crc32_update(0, rom.data(), rom.size())));
  CHECK(identify(cmd, root + "/one.zip", true, id));
  CHECK(id.hashed);
  CHECK_EQ(id.size, uint64_t(rom.size()));
  CHECK_EQ(id.crc, hex8(crc32_update(0, rom.data(), rom.size())));
  CHECK(id.md5.empty());
  // arcade-like systems: the zip IS the ROM
  CHECK(!extract_archives_for("arcade"));
  CHECK(!extract_archives_for("atomiswave arcade"));
  CHECK(!extract_archives_for("neogeo"));
  CHECK(extract_archives_for("snes"));
  CHECK(identify(cmd, root + "/one.zip", extract_archives_for("arcade"), id));
  CHECK(id.hashed);
  struct stat st {};
  CHECK(::stat((root + "/one.zip").c_str(), &st) == 0);
  CHECK_EQ(id.size, uint64_t(st.st_size));
  CHECK(!id.md5.empty());
  // several ROMs, or a damaged zip: the archive itself is hashed
  spit(root + "/two.zip", make_zip({{"a.sfc", "AAA"}, {"b.sfc", "BBB"}}));
  CHECK(!zip_single_entry(root + "/two.zip", z));
  CHECK(identify(cmd, root + "/two.zip", true, id));
  CHECK(!id.md5.empty());
  spit(root + "/bad.zip", "not a zip at all, just text that is long enough to have a tail....");
  CHECK(!zip_single_entry(root + "/bad.zip", z));
  CHECK(identify(cmd, root + "/bad.zip", true, id));
  CHECK(id.hashed && !id.md5.empty());

  // --- .7z goes through `7za l -slt`: in dry-run (or without 7z) it falls back to the file itself
  spit(root + "/x.7z", "7z\xBC\xAF\x27\x1C-fake");
  CHECK(identify(cmd, root + "/x.7z", true, id));
  CHECK(!cmd.log.empty() && cmd.log.back().find("7za l -slt") != std::string::npos);
  CHECK(id.hashed && !id.md5.empty());

  // --- folders, missing files, m3u
  fs::create_directories(root + "/gamedir");
  CHECK(identify(cmd, root + "/gamedir", true, id));
  CHECK(!id.hashed && id.size == 0);
  CHECK(!identify(cmd, root + "/nope.bin", true, id));
  spit(root + "/disc1.bin", "DISC-ONE");
  spit(root + "/multi.m3u", "# comment\n\ndisc1.bin\ndisc2.bin\n");
  CHECK(identify(cmd, root + "/multi.m3u", true, id));
  CHECK_EQ(id.md5, md5_of("DISC-ONE"));

  // --- over the size limit: only the size is known (no hashing, no reading)
  {
    int fd = ::open((root + "/huge.bin").c_str(), O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, off_t(kMaxHashBytes + 1)) == 0);  // sparse
    ::close(fd);
    CHECK(identify(cmd, root + "/huge.bin", true, id));
    CHECK(!id.hashed && id.md5.empty());
    CHECK_EQ(id.size, kMaxHashBytes + 1);
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
