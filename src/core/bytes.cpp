#include "core/bytes.h"

#include <windows.h>

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

}  // namespace pb
