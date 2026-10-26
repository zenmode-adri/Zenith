// Small helpers shared by every module (files, numbers, UTF-8, time). No state.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace zf {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// Whole file into `out`; false if it cannot be opened.
bool read_file(const std::string& path, std::string& out);
/// `size` bytes at `offset` (for gamelist spans); false on short read.
bool read_file_range(const std::string& path, uint64_t offset, size_t size, std::string& out);
bool file_exists(const std::string& path);
bool is_dir(const std::string& path);

/// Locale independent float parse of the whole string (theme numbers: the console may run with LANG=es_*).
bool parse_float(std::string_view s, float& out);
bool parse_int(std::string_view s, long& out);

std::string_view trim(std::string_view s);
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);

/// Next code point of a UTF-8 string (advances `i`); invalid bytes give U+FFFD.
uint32_t utf8_next(std::string_view s, size_t& i);
void utf8_append(std::string& out, uint32_t cp);
/// Upper case (Latin, Greek, Cyrillic basics), like Rust's `to_uppercase` for the cases games use.
std::string utf8_upper(std::string_view s);
/// `s` with its first letter in capitals ("últimos jugados" -> "Últimos jugados").
std::string utf8_cap_first(std::string_view s);
/// Number of code points.
size_t utf8_len(std::string_view s);

/// Preview / dry-run: every file the frontend would WRITE (settings, gamelist, collections, es_input, savestate files)
/// is skipped and reported as done, so a harness pointed at real files never modifies them. Set once at start-up
/// (main.cpp); the single check lives in the write helpers (`write_atomic`, gamelist `splice_file`, savestates).
bool writes_blocked();
void set_writes_blocked(bool blocked);

/// `/proc/uptime` first field, for first-frame logs.
std::string uptime_text();

/// `a/b` joining with exactly one slash.
std::string path_join(std::string_view a, std::string_view b);
std::string path_dir(std::string_view p);
std::string path_filename(std::string_view p);
/// File name up to the last dot (a leading dot is part of the name).
std::string path_stem(std::string_view p);
/// Lower-case extension with its dot (".gb"), empty if none.
std::string path_ext_lower(std::string_view p);

}  // namespace zf
