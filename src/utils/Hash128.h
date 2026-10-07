#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

struct Hash128 {
  uint64_t a{0};
  uint64_t b{0};
  bool operator==(const Hash128& o) const { return a == o.a && b == o.b; }
  bool operator!=(const Hash128& o) const { return !(*this == o); }
  bool operator<(const Hash128& o) const { return a < o.a || (a == o.a && b < o.b); }
  [[nodiscard]] std::string hex() const;
};

struct Hash128Hash {
  size_t operator()(const Hash128& h) const noexcept { return static_cast<size_t>(h.a ^ (h.b << 1)); }
};

// Order-sensitive, non-cryptographic 128-bit hash: two lanes of murmur3's finalizer.
class Hasher128
{
public:
  void u64(uint64_t w)
  {
    s0 = fmix(s0 ^ w) + 0x9E3779B97F4A7C15ULL;
    s1 = fmix(s1 + (w ^ 0xC2B2AE3D27D4EB4FULL)) ^ (s0 >> 17);
    ++n;
  }
  void f64(double d)
  {
    uint64_t w;
    std::memcpy(&w, &d, sizeof w);
    u64(w);
  }
  void bytes(const void *data, size_t size)
  {
    const auto *p = static_cast<const unsigned char *>(data);
    while (size >= 8) {
      uint64_t w;
      std::memcpy(&w, p, 8);
      u64(w);
      p += 8;
      size -= 8;
    }
    if (size) {
      uint64_t w = 0;
      std::memcpy(&w, p, size);
      u64(w ^ (uint64_t(size) << 56));
    }
  }
  void str(std::string_view s)
  {
    u64(s.size());
    bytes(s.data(), s.size());
  }
  void h(const Hash128& x)
  {
    u64(x.a);
    u64(x.b);
  }
  [[nodiscard]] Hash128 finish() const
  {
    Hash128 r;
    r.a = fmix(s0 ^ fmix(n + 0x632BE59BD9B4E019ULL));
    r.b = fmix(s1 ^ (n * 0x94D049BB133111EBULL) ^ r.a);
    return r;
  }

private:
  static uint64_t fmix(uint64_t k)
  {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }
  uint64_t s0{0x243F6A8885A308D3ULL};
  uint64_t s1{0x13198A2E03707344ULL};
  uint64_t n{0};
};

inline std::string Hash128::hex() const
{
  static const char digits[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int i = 0; i < 16; ++i) {
    out[15 - i] = digits[(a >> (4 * i)) & 0xf];
    out[31 - i] = digits[(b >> (4 * i)) & 0xf];
  }
  return out;
}
