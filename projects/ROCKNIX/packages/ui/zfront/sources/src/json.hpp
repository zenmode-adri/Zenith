// Minimal JSON reader (RFC 8259 subset enough for the RetroAchievements answers): no exceptions, depth capped at 64, strings
// decoded (\uXXXX including surrogate pairs -> UTF-8), numbers kept as double. Input is untrusted network text: any error
// returns false and leaves `out` null.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zf::json {

struct Value {
  enum class Type : unsigned char { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  bool is_object() const { return type == Type::Object; }
  bool is_array() const { return type == Type::Array; }
  /// Member of an object (first match), or nullptr.
  const Value* find(std::string_view key) const;
  /// Member as text: strings as they are, numbers as integers when they are one, "true"/"false"; "" when missing or not a scalar.
  std::string text(std::string_view key) const;
  /// Member as integer (numbers, or numeric strings as the RA API sends them); `def` when missing.
  long integer(std::string_view key, long def = 0) const;
  /// Member as bool (true only for JSON true).
  bool boolean(std::string_view key) const;
};

/// Parses one document (trailing whitespace allowed, anything else after it is an error).
bool parse(std::string_view text, Value& out);

}  // namespace zf::json
