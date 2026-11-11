// read_file: /proc and debugfs files report 0 bytes but have content (they read as "" before: anything reading /proc through it
// saw nothing); regular files and the size cap as before.
#include <cstdio>
#include <string>

#include "check.hpp"
#include "util.hpp"

using namespace zf;

int main() {
  std::string st;
  CHECK(read_file("/proc/self/status", st));
  CHECK(st.find("VmRSS") != std::string::npos);
  const std::string p = "/tmp/zf-test-read-file.txt";
  if (std::FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fputs("hello", f);
    std::fclose(f);
  }
  std::string s;
  CHECK(read_file(p, s));
  CHECK_EQ(s, std::string("hello"));
  std::remove(p.c_str());
  CHECK(!read_file("/tmp", s));  // a directory is not read
  return zt::done();
}
