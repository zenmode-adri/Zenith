// MODULE `webcloud`: the Cloud tab of the web file transfer. It connects the console to a cloud drive FROM THE PHONE, with no terminal:
// the page asks for what the service needs, this module writes the rclone configuration (/storage/.config/rclone/rclone.conf) and the
// console's CLOUD BACKUP (cloud_backup / cloud_restore) works from then on.
//
//   GET  /files/cloud            JSON: is rclone there, is a cloud connected (name, type, host/url, user; NEVER a password or token)
//   POST /files/cloud/set        JSON body, one of
//                                  {"type":"webdav","url":"https://...","user":"..","pass":"..","vendor":"nextcloud"|"owncloud"|"other"}
//                                  {"type":"sftp","host":"..","port":"22","user":"..","pass":".."}
//                                  {"type":"ftp","host":"..","port":"21","user":"..","pass":"..","tls":true}
//                                  {"type":"drive"|"dropbox","token":"{\"access_token\":...}"}   (what `rclone authorize` prints)
//   POST /files/cloud/test       starts a connection test in the background (answers 200 at once); 409 while one runs
//   GET  /files/cloud/test       JSON: {"state":"idle"|"running"|"ok"|"fail","message":".."}
//   POST /files/cloud/remove     forgets the cloud (the other remotes of the file, if any, stay)
//
// Safety: the same PIN / paired-phone check as every other route. The remote is always called `zenith` (cloud_backup, cloud_restore and
// rclonectl take the first remote; the scripts prefer `zenith`). rclone is run WITHOUT a shell (fork + execv, arguments one by one); a
// password goes to `rclone obscure -` through its standard input, so it never shows in an argument list or a log, and the page never gets
// a secret back. The file is rewritten whole to a temporary name (mode 0600) and renamed; sections of other remotes are kept as they are.
// Paths come from ZF_RCLONE and ZF_RCLONE_CONF in tests, never /storage.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace zf {
namespace webcloud {

constexpr const char* kRemote = "zenith";

/// `ZF_RCLONE` or /usr/bin/rclone.
std::string rclone_path();
/// `ZF_RCLONE_CONF` or /storage/.config/rclone/rclone.conf.
std::string conf_path();

/// What a configuration says about the `zenith` remote (no secrets).
struct Remote {
  bool present = false;
  std::string type, where, user;  // where: url for webdav, host[:port] for sftp / ftp, "" for drive / dropbox
};
Remote read_remote(const std::string& ini);

/// A field from the page: no control characters (CR, LF, NUL...), at most `max` bytes, not empty.
bool valid_field(std::string_view v, size_t max);
/// A port: 1 to 65535.
bool valid_port(std::string_view v);
/// http:// or https:// followed by something without spaces.
bool valid_url(std::string_view v);
/// What `rclone authorize` prints (JSON with an access_token): the single line to keep, "" when it is not that.
std::string clean_token(std::string_view raw);

/// `ini` with the `[name]` section replaced by `body` (the lines of the section, without the header), added at the end when it was not
/// there. An empty `body` removes the section. Other sections are untouched.
std::string replace_section(const std::string& ini, const std::string& name, const std::string& body);
/// True when the file has no section left.
bool has_no_section(const std::string& ini);

struct SetResult {
  int status = 200;
  std::string error;  // for status != 200
};
/// Turns the page's JSON into the section body and writes it. `obscure` turns a password into rclone's form (tests may replace it).
SetResult set_remote(const std::string& json_body);
/// Forgets the remote; the file is deleted when nothing else is left in it.
SetResult remove_remote();

/// Runs a program without a shell: stdin text in, stdout and stderr out (capped at 64 KB). -1 when it could not start or took longer than
/// `timeout_ms` (it is killed).
int run_argv(const std::vector<std::string>& argv, const std::string& in, int timeout_ms, std::string& out);

// ---- tests ---------------------------------------------------------------------------------------------------------------
/// Waits (up to `ms`) until no test connection is running.
bool wait_idle(int ms);

}  // namespace webcloud
}  // namespace zf
