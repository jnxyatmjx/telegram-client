#ifndef TG_INFRA_SHA256_H_
#define TG_INFRA_SHA256_H_

// Standalone SHA-256 (FIPS 180-4). Used only for logging / dedup bookkeeping
// — the hash is deliberately NOT part of the proto (v5 §2.2 step [4]).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tg::infra {

class Sha256 {
 public:
  static constexpr size_t kDigestSize = 32;

  Sha256();

  void Update(const void* data, size_t length);
  void Update(const std::string& data) { Update(data.data(), data.size()); }

  std::array<std::uint8_t, kDigestSize> Finish();

  std::string FinishHex();

  // One-shot helpers.
  static std::string Hex(const void* data, size_t length);
  static std::string Hex(const std::string& data) {
    return Hex(data.data(), data.size());
  }
  static std::string HexDigest(const std::array<std::uint8_t, kDigestSize>& digest);

 private:
  void Transform(const std::uint8_t* block);

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::uint8_t buffer_[64];
  size_t buffer_length_;
};

}  // namespace tg::infra

#endif  // TG_INFRA_SHA256_H_
