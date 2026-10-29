#include "core/json.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace pb::json {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : t_(text) {}

  Value parse_value() {
    skip_ws();
    if (pos_ >= t_.size()) fail("unexpected end of input");
    char c = t_[pos_];
    switch (c) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': {
        Value v;
        v.type = Value::String;
        v.str = parse_string();
        return v;
      }
      case 't': expect_literal("true"); return bool_value(true);
      case 'f': expect_literal("false"); return bool_value(false);
      case 'n': expect_literal("null"); return Value{};
      default: return parse_number();
    }
  }

 private:
  static Value bool_value(bool b) {
    Value v;
    v.type = Value::Bool;
    v.b = b;
    return v;
  }

  void fail(const char* why) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "json:%zu: %s", pos_, why);
    throw std::runtime_error(buf);
  }
  void skip_ws() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
  }
  char peek() {
    skip_ws();
    if (pos_ >= t_.size()) fail("unexpected end of input");
    return t_[pos_];
  }
  void expect(char c) {
    if (peek() != c) fail("unexpected character");
    ++pos_;
  }
  void expect_literal(const char* lit) {
    for (const char* p = lit; *p; ++p) {
      if (pos_ >= t_.size() || t_[pos_] != *p) fail("bad literal");
      ++pos_;
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos_ >= t_.size()) fail("unterminated string");
      char c = t_[pos_++];
      if (c == '"') break;
      if (c == '\\') {
        if (pos_ >= t_.size()) fail("unterminated escape");
        char e = t_[pos_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > t_.size()) fail("bad \\u escape");
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i) {
              char h = t_[pos_++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
              else fail("bad hex digit");
            }
            // 简化：BMP 直存 UTF-8；代理对按规范合成
            if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= t_.size() && t_[pos_] == '\\' && t_[pos_ + 1] == 'u') {
              pos_ += 2;
              unsigned lo = 0;
              for (int i = 0; i < 4; ++i) {
                char h = t_[pos_++];
                lo <<= 4;
                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                else fail("bad hex digit");
              }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            append_utf8(out, cp);
            break;
          }
          default: fail("bad escape");
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  static void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back((char)cp);
    } else if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }

  Value parse_number() {
    size_t start = pos_;
    if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
    bool digits = false, dot = false;
    while (pos_ < t_.size()) {
      char c = t_[pos_];
      if (std::isdigit((unsigned char)c)) { digits = true; ++pos_; }
      else if (c == '.' && !dot) { dot = true; ++pos_; }
      else if (c == 'e' || c == 'E' || c == '+' || c == '-') { ++pos_; }
      else break;
    }
    if (!digits) fail("bad number");
    Value v;
    v.type = Value::Number;
    v.num = std::stod(std::string(t_.substr(start, pos_ - start)));
    return v;
  }

  Value parse_object() {
    expect('{');
    Value v;
    v.type = Value::Object;
    if (peek() == '}') { ++pos_; return v; }
    while (true) {
      std::string key = parse_string();
      expect(':');
      v.obj[key] = parse_value();
      char c = peek();
      if (c == ',') { ++pos_; continue; }
      if (c == '}') { ++pos_; break; }
      fail("expected ',' or '}'");
    }
    return v;
  }

  Value parse_array() {
    expect('[');
    Value v;
    v.type = Value::Array;
    if (peek() == ']') { ++pos_; return v; }
    while (true) {
      v.arr.push_back(parse_value());
      char c = peek();
      if (c == ',') { ++pos_; continue; }
      if (c == ']') { ++pos_; break; }
      fail("expected ',' or ']'");
    }
    return v;
  }

  std::string_view t_;
  size_t pos_ = 0;
};

}  // namespace

Value parse(std::string_view text) {
  Parser p(text);
  Value v = p.parse_value();
  return v;
}

}  // namespace pb::json
