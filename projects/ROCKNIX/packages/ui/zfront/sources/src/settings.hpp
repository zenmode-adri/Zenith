// The two settings files shared with the rest of the OS: `system.cfg` (`key=value`, read by get_setting in shell) and
// ES's `es_settings.cfg` (XML, `<bool|string|int|float name=".." value=".."/>`). Both are written atomically (temp
// file + rename) because other programs (tuner, OTA scripts, runemu.sh) read them while we run.
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zf {

/// Write `text` to `path` through a temp file in the same directory, then `rename` (atomic on the same filesystem).
bool write_atomic(const std::string& path, const std::string& text, std::string* err = nullptr);

/// Bumped by every successful save of system.cfg or es_settings.cfg made by this process (any thread). Whoever caches
/// work derived from those files (the idle-time launch preparation) compares it with the value it saw.
unsigned settings_generation();

/// `system.cfg`: plain `key=value`, `#` and `;` are comments. Unknown lines are kept on write.
class SystemConf {
 public:
  SystemConf() = default;
  static SystemConf load(const std::string& path);
  /// Parse text (tests).
  static SystemConf from_text(const std::string& text, const std::string& path = std::string());

  const std::string* get(const std::string& key) const;
  std::string get_or(const std::string& key, const std::string& def) const;
  bool get_bool(const std::string& key, bool def) const;
  void set(const std::string& key, const std::string& value);
  void remove(const std::string& key);
  bool save(std::string* err = nullptr) const;
  std::string text() const;
  /// Locked read-modify-write of ONE key: takes the same lock as the shell scripts (/tmp/.system.cfg.lock), re-reads
  /// the file, applies the change and writes atomically, so concurrent `sed -i` edits of other keys are never lost.
  /// Use these (not set()+save()) from menus.
  bool persist(const std::string& key, const std::string& value, std::string* err = nullptr);
  bool persist_remove(const std::string& key, std::string* err = nullptr);
  /// Lock file shared with 001-functions (tests point it elsewhere).
  static std::string& lock_path();
  /// The plain map, the shape `systems::resolve_emu` and `launch::build_command` take.
  const std::unordered_map<std::string, std::string>& map() const { return map_; }
  /// File this conf was loaded from (empty for from_text without a path): lets a menu re-read it fresh.
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::vector<std::string> lines_;
  std::unordered_map<std::string, std::string> map_;
};

/// `es_settings.cfg`: every value kept as text with its type tag.
class EsSettings {
 public:
  EsSettings() = default;
  static EsSettings load(const std::string& path);
  static EsSettings from_text(const std::string& xml, const std::string& path = std::string());

  const std::string* get(const std::string& name) const;
  bool get_bool(const std::string& name, bool def) const;
  long get_int(const std::string& name, long def) const;
  void set_bool(const std::string& name, bool v);
  void set_string(const std::string& name, const std::string& v);
  void set_int(const std::string& name, long v);
  bool save(std::string* err = nullptr) const;
  std::string text() const;

 private:
  std::string path_;
  std::map<std::string, std::pair<std::string, std::string>> map_;  // name -> (type tag, value)
};

}  // namespace zf
