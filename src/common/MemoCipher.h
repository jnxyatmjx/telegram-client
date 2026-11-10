#ifndef TG_COMMON_MEMOCIPHER_H_
#define TG_COMMON_MEMOCIPHER_H_

// MemoCipher — AES-256-GCM memo encryption, byte-level compatible with the
// Python client (app_client.py aes_gcm_encrypt / aes_gcm_decrypt 1129-1162;
// Telethon migration Chunk 1, Hong decision 1 of 2026-11-10).
//
// Wire format: base64( nonce[12] || ciphertext || tag[16] ), AES-256-GCM,
// key = SHA256(KEYME.utf8) (32 bytes), nonce = 12 fresh random bytes per
// message. The server must be able to decrypt with the same KEYME.
//
// Cleaning (identical to the Python regex, applied before encryption):
//   re.sub(r'https?://\S+|t\.me/?\S+|[\s*]+|[\[\]\*_-]+', '', memo)
// i.e. URLs, t.me links, whitespace (Unicode, incl. U+3000), '*',
// '[', ']', '_' and '-' are all removed. Leftmost alternative wins, greedy
// runs — implemented as a codepoint scanner, see MemoCipher.cpp.
//
// KEYME is a secret: it comes from config (`features.media_thumbnail.keyme`,
// normally `${KEYME}` from the environment) and must never be committed.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace tg::common {

// GCM nonce size (Python: os.urandom(12)) and auth tag size (16).
inline constexpr std::size_t kMemoNonceSize = 12;
inline constexpr std::size_t kMemoTagSize = 16;
using MemoNonce = std::array<std::uint8_t, kMemoNonceSize>;

class MemoCipher {
 public:
  // keyme: the seed secret; SHA256(keyme) is the AES key.
  explicit MemoCipher(std::string keyme);

  MemoCipher(const MemoCipher&) = default;
  MemoCipher& operator=(const MemoCipher&) = default;
  MemoCipher(MemoCipher&&) = default;
  MemoCipher& operator=(MemoCipher&&) = default;

  const std::string& keyme() const { return keyme_; }
  bool IsConfigured() const { return !keyme_.empty(); }

  // Python-compatible: clean, then encrypt with a fresh random nonce.
  // Returns base64(nonce || ciphertext || tag). nullopt when the cipher is
  // not configured (empty KEYME) or OpenSSL fails (logged).
  std::optional<std::string> Encrypt(const std::string& memo) const;

  // Deterministic variant with a caller-supplied nonce. Production code must
  // use Encrypt(); the fixed-nonce form exists so tests can pin cross-language
  // vectors (same key + plaintext + nonce -> same bytes as Python would
  // produce). Never reuse a nonce for two different messages with one key.
  std::optional<std::string> EncryptWithNonce(const std::string& memo,
                                              const MemoNonce& nonce) const;

  // Inverse of Encrypt: base64 -> split (nonce | ct | tag) -> verify + decrypt.
  // Returns the CLEANED plaintext (the wire format carries no original
  // whitespace/URLs — they were removed before encryption). nullopt on
  // invalid base64, short input, or tag mismatch (wrong key / tampering).
  std::optional<std::string> Decrypt(const std::string& token) const;

  // The Python-compatible regex clean, exposed for tests.
  static std::string Clean(const std::string& memo);

 private:
  std::string keyme_;  // seed; key = SHA256(keyme)
};

}  // namespace tg::common

#endif  // TG_COMMON_MEMOCIPHER_H_
