#pragma once
// 字节流读写与 hex 工具
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pb {

class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16le(uint16_t v) { buf_.push_back(v & 0xFF); buf_.push_back(v >> 8); }
  void u16be(uint16_t v) { buf_.push_back(v >> 8); buf_.push_back(v & 0xFF); }
  void u32le(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8 * i)) & 0xFF);
  }
  void u32be(uint32_t v) {
    for (int i = 3; i >= 0; --i) buf_.push_back((v >> (8 * i)) & 0xFF);
  }
  void u64be(uint64_t v) {
    for (int i = 7; i >= 0; --i) buf_.push_back((v >> (8 * i)) & 0xFF);
  }
  void bytes(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
  void zeros(size_t n) { buf_.insert(buf_.end(), n, 0); }
  const std::vector<uint8_t>& buf() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }
  size_t size() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

  uint8_t u8() {
    if (!ensure(1)) return 0;
    return p_[pos_++];
  }
  uint16_t u16le() {
    if (!ensure(2)) return 0;
    uint16_t v = p_[pos_] | (p_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
  }
  uint16_t u16be() {
    if (!ensure(2)) return 0;
    uint16_t v = (p_[pos_] << 8) | p_[pos_ + 1];
    pos_ += 2;
    return v;
  }
  uint32_t u32le() {
    if (!ensure(4)) return 0;
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p_[pos_ + i];
    pos_ += 4;
    return v;
  }
  uint32_t u32be() {
    if (!ensure(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | p_[pos_ + i];
    pos_ += 4;
    return v;
  }
  uint64_t u64be() {
    if (!ensure(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p_[pos_ + i];
    pos_ += 8;
    return v;
  }
  std::vector<uint8_t> bytes(size_t n) {
    if (!ensure(n)) return {};
    std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + n);
    pos_ += n;
    return v;
  }
  void skip(size_t n) {
    if (!ensure(n)) return;
    pos_ += n;
  }
  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? n_ - pos_ : 0; }
  const uint8_t* cur() const { return p_ + pos_; }

 private:
  bool ensure(size_t n) {
    if (!ok_ || pos_ + n > n_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

std::string to_hex(const uint8_t* p, size_t n);
inline std::string to_hex(const std::vector<uint8_t>& v) { return to_hex(v.data(), v.size()); }
std::vector<uint8_t> from_hex(std::string_view s);

// UTF-8 <-> UTF-16 conversions (single shared copy for GUI and core callers;
// Windows-only project, so windows.h macros are fine here).
std::wstring to_w(std::string_view utf8);
std::string from_w(std::wstring_view utf16);

}  // namespace pb
