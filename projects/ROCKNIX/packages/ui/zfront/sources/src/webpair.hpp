// MODULE `webpair`: phones that the console remembers. A phone that knows the PIN asks `POST /files/pair` (public; the PIN in X-PIN, the
// device name as the body); the console shows "ALLOW THIS DEVICE?" and, on yes, answers with a token (32 random bytes, 64 hex digits)
// that the page keeps in its localStorage and sends as `X-Token` instead of the PIN. `webtransfer::authorize` accepts either.
//
// Safety: the PIN is checked with the SAME lock-out as every other route (five wrong ones lock everything for 30 s); the question
// waits at most 60 s on the transfer worker (one streamed request at a time, so a second device gets 409 meanwhile) and no answer
// means no; tokens are compared in constant time and never logged; the list lives in /storage/.config/zfront/webdevices (mode 0600,
// written to a temp file and renamed), at most ten devices (the oldest paired is dropped); `last seen` is written at most once a
// minute per device. The token travels in a header only, never in a cookie, and no answer carries Access-Control-* headers.
// The console side (the NETWORK rows, the QR code screen, the list of devices) is in webqr_screen.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "webtransfer.hpp"

namespace zf {
namespace webpair {

constexpr size_t kMaxDevices = 10;
constexpr int kAskTimeoutMs = 60000;
constexpr size_t kMaxNameChars = 40;
constexpr long long kSeenGapSeconds = 60;

struct Device {
  std::string id;    // the first 12 hex digits of its token: enough to name it on the console, useless to anyone else
  std::string name;
  long long paired = 0, last_seen = 0;  // unix seconds
};

/// The token (64 hex digits) belongs to a paired device (constant time). Updates `last_seen` at most once a minute.
bool token_valid(std::string_view token);
/// The paired devices, oldest first (no tokens).
std::vector<Device> devices();
/// Forgets one device (by `Device::id`) or all; false when no device has that id.
bool forget(const std::string& id);
void forget_all();
/// Answer to the question `gen` (from the console). False when that question is no longer open (timed out, answered).
bool answer(unsigned gen, bool yes);
/// A device name from the page: printable ASCII only, at most `kMaxNameChars`, "Unknown device" when nothing is left.
std::string sanitize_name(std::string_view raw);
/// Where the list is kept (`ZF_WEBDEVICES`, else /storage/.config/zfront/webdevices).
std::string devices_path();

// ---- tests -------------------------------------------------------------------------------------------------------------
/// How the console is asked: returns false when it cannot be (the answer is then 503). Default: a dialog through `Env::on_main`.
using Asker = std::function<bool(const std::string& name, unsigned gen, const webtransfer::Env& env)>;
void set_asker_for_test(Asker a);
void set_ask_timeout_ms(int ms);
void set_clock_for_test(std::function<long long()> now_seconds);
/// Forgets everything held in memory (not the file) and takes the path from the environment again: the next use reads the file.
void reload_for_test();
/// reload_for_test plus default asker, timeout and clock.
void reset_for_test();

}  // namespace webpair
}  // namespace zf
