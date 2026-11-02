#include "core/bytes.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pb {

std::string to_hex(const uint8_t* p, size_t n) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kDigits[p[i] >> 4]);
    out.push_back(kDigits[p[i] & 0xF]);
  }
  return out;
}

std::vector<uint8_t> from_hex(std::string_view s) {
  if (s.size() % 2 != 0) throw std::runtime_error("from_hex: odd length");
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) throw std::runtime_error("from_hex: invalid digit");
    out.push_back((hi << 4) | lo);
  }
  return out;
}

#ifdef _WIN32
std::wstring to_w(std::string_view utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring w(n > 0 ? n : 0, 0);
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
  return w;
}

std::string from_w(std::wstring_view utf16) {
  int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), nullptr, 0, nullptr,
                              nullptr);
  std::string s(n > 0 ? n : 0, 0);
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), (int)utf16.size(), s.data(), n, nullptr,
                        nullptr);
  return s;
}
#else
namespace {
void append_utf16(std::wstring& w, unsigned cp) {
  if (cp < 0x10000) {
    w.push_back((wchar_t)cp);
  } else {
    cp -= 0x10000;
    w.push_back((wchar_t)(0xD800 + ((cp >> 10) & 0x3FF)));
    w.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
  }
}
unsigned decode_utf8(std::string_view s, size_t& i) {
  unsigned c = (unsigned char)s[i];
  if (c < 0x80) {
    ++i;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
    i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    unsigned cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) |
                  ((unsigned char)s[i + 2] & 0x3F);
    i += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    unsigned cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
                  (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
    i += 4;
    return cp;
  }
  ++i;
  return c;
}
}  // namespace

std::wstring to_w(std::string_view utf8) {
  std::wstring w;
  for (size_t i = 0; i < utf8.size();) append_utf16(w, decode_utf8(utf8, i));
  return w;
}

std::string from_w(std::wstring_view utf16) {
  std::string s;
  for (size_t i = 0; i < utf16.size(); ++i) {
    unsigned cp = (unsigned)utf16[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
      unsigned lo = (unsigned)utf16[++i];
      cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
    }
    if (cp < 0x80) {
      s.push_back((char)cp);
    } else if (cp < 0x800) {
      s.push_back((char)(0xC0 | (cp >> 6)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back((char)(0xE0 | (cp >> 12)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      s.push_back((char)(0xF0 | (cp >> 18)));
      s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return s;
}
#endif

}  // namespace pb
