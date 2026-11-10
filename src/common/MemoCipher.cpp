#include "common/MemoCipher.h"

#include <common/Logger.h>
#include <infra/Sha256.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <utility>

namespace tg::common {
namespace {

// ---------------------------------------------------------------------------
// Python regex compatibility helpers.
//
// Clean() implements, character by character:
//   re.sub(r'https?://\S+|t\.me/?\S+|[\s*]+|[\[\]\*_-]+', '', memo)
// Python's `\s` for str patterns is the full Unicode whitespace set
// (str.isspace()-true characters), not just ASCII — memo captions can carry
// U+00A0 / U+3000 etc., so the scanner decodes UTF-8 codepoints.
// ---------------------------------------------------------------------------

struct Codepoint {
  std::uint32_t value;
  std::size_t length;  // bytes in UTF-8
};

// Decodes one UTF-8 codepoint. Invalid/overlong/truncated sequences yield the
// single lead byte as a Latin-1 style codepoint (>= 0x80, can never match a
// removal class), which keeps the scan byte-aligned and lossless.
Codepoint DecodeCp(const std::string& s, size_t i) {
  const auto byte = static_cast<std::uint8_t>(s[i]);
  if (byte < 0x80) return {byte, 1};
  std::uint32_t cp = 0;
  std::size_t len = 0;
  if ((byte & 0xE0) == 0xC0) {
    cp = byte & 0x1F;
    len = 2;
  } else if ((byte & 0xF0) == 0xE0) {
    cp = byte & 0x0F;
    len = 3;
  } else if ((byte & 0xF8) == 0xF0) {
    cp = byte & 0x07;
    len = 4;
  } else {
    return {byte, 1};  // stray continuation byte
  }
  if (i + len > s.size()) return {byte, 1};  // truncated sequence
  for (std::size_t k = 1; k < len; ++k) {
    const auto cont = static_cast<std::uint8_t>(s[i + k]);
    if ((cont & 0xC0) != 0x80) return {byte, 1};
    cp = (cp << 6) | (cont & 0x3F);
  }
  return {cp, len};
}

// Python 3 `\s` (str pattern): exactly the characters for which
// str.isspace() is true.
bool IsPySpace(std::uint32_t cp) {
  if (cp <= 0x20) {
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C ||
           cp == 0x0D || cp == 0x1C || cp == 0x1D || cp == 0x1E ||
           cp == 0x1F || cp == 0x20;
  }
  switch (cp) {
    case 0x85:    // NEL
    case 0xA0:    // NBSP
    case 0x1680:  // OGHAM SPACE MARK
    case 0x2028:  // LINE SEPARATOR
    case 0x2029:  // PARAGRAPH SEPARATOR
    case 0x202F:  // NARROW NO-BREAK SPACE
    case 0x205F:  // MEDIUM MATHEMATICAL SPACE
    case 0x3000:  // IDEOGRAPHIC SPACE
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

bool IsAsciiNonSpace(std::uint32_t cp) {
  // \S for our purposes: not Unicode whitespace. (Captions are valid UTF-8;
  // every byte of a decoded codepoint shares its whitespace verdict.)
  return !IsPySpace(cp);
}

// Maximal run of bytes whose codepoints satisfy `pred`, starting at i.
size_t RunWhile(const std::string& s, size_t i, bool (*pred)(std::uint32_t)) {
  while (i < s.size()) {
    const Codepoint cp = DecodeCp(s, i);
    if (!pred(cp.value)) break;
    i += cp.length;
  }
  return i;
}

// Bytes the run occupies when it starts at `start` and ends at `end`.
struct RunMatch {
  bool matched = false;
  size_t end = 0;  // byte offset just past the match
};

// Alt 1: https?://\S+   (try `s` first, as the regex does).
RunMatch MatchUrl(const std::string& s, size_t i) {
  RunMatch m;
  static const char kHttp[] = "http";
  if (s.compare(i, 4, kHttp) != 0) return m;
  for (const int has_s : {1, 0}) {
    size_t p = i + 4;
    if (has_s != 0) {
      if (p >= s.size() || s[p] != 's') continue;
      ++p;
    }
    static const char kSep[] = "://";
    if (s.compare(p, 3, kSep) != 0) continue;
    p += 3;
    const size_t run_end = RunWhile(s, p, IsAsciiNonSpace);
    if (run_end == p) continue;  // \S+ needs at least one char
    m.matched = true;
    m.end = run_end;
    return m;
  }
  return m;
}

// Alt 2: t\.me/?\S+. After the literal "t.me", Python tries the slash first,
// then backtracks to an empty `/?` — both consume the same maximal non-space
// run starting at the byte right after "t.me" (the slash itself is non-space),
// so a single greedy run reproduces the backtracking exactly.
RunMatch MatchTme(const std::string& s, size_t i) {
  RunMatch m;
  static const char kTme[] = "t.me";
  if (s.compare(i, 4, kTme) != 0) return m;
  const size_t p = i + 4;
  if (p >= s.size()) return m;  // \S+ needs at least one char
  const size_t run_end = RunWhile(s, p, IsAsciiNonSpace);
  if (run_end == p) return m;
  m.matched = true;
  m.end = run_end;
  return m;
}

// Alt 3: [\s*]+
RunMatch MatchSpaceStar(const std::string& s, size_t i) {
  RunMatch m;
  size_t p = i;
  while (p < s.size()) {
    const Codepoint cp = DecodeCp(s, p);
    if (!IsPySpace(cp.value) && cp.value != static_cast<std::uint32_t>('*')) break;
    p += cp.length;
  }
  if (p == i) return m;
  m.matched = true;
  m.end = p;
  return m;
}

// Alt 4: [\[\]\*_-]+
RunMatch MatchBracketRun(const std::string& s, size_t i) {
  RunMatch m;
  size_t p = i;
  while (p < s.size() && (s[p] == '[' || s[p] == ']' || s[p] == '*' ||
                          s[p] == '_' || s[p] == '-')) {
    ++p;
  }
  if (p == i) return m;
  m.matched = true;
  m.end = p;
  return m;
}

// ---------------------------------------------------------------------------
// Base64 (standard alphabet, '=' padding — matches Python base64.b64encode).
// ---------------------------------------------------------------------------

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& raw) {
  std::string out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= raw.size()) {
    const auto b0 = static_cast<std::uint8_t>(raw[i]);
    const auto b1 = static_cast<std::uint8_t>(raw[i + 1]);
    const auto b2 = static_cast<std::uint8_t>(raw[i + 2]);
    out.push_back(kBase64Chars[b0 >> 2]);
    out.push_back(kBase64Chars[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(kBase64Chars[((b1 & 0x0F) << 2) | (b2 >> 6)]);
    out.push_back(kBase64Chars[b2 & 0x3F]);
    i += 3;
  }
  const size_t rem = raw.size() - i;
  if (rem == 1) {
    const auto b0 = static_cast<std::uint8_t>(raw[i]);
    out.push_back(kBase64Chars[b0 >> 2]);
    out.push_back(kBase64Chars[(b0 & 0x03) << 4]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const auto b0 = static_cast<std::uint8_t>(raw[i]);
    const auto b1 = static_cast<std::uint8_t>(raw[i + 1]);
    out.push_back(kBase64Chars[b0 >> 2]);
    out.push_back(kBase64Chars[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(kBase64Chars[(b1 & 0x0F) << 2]);
    out.push_back('=');
  }
  return out;
}

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool Base64Decode(const std::string& text, std::string* out) {
  if (text.size() % 4 != 0 || text.empty()) return false;
  size_t pad = 0;
  if (text.back() == '=') {
    pad = 1;
    if (text[text.size() - 2] == '=') pad = 2;
    if (pad == 2 && text[text.size() - 3] == '=') return false;
  }
  out->clear();
  out->reserve(text.size() / 4 * 3);
  const size_t body = text.size() - pad;
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    for (int k = 0; k < 4; ++k) {
      v[k] = i + static_cast<size_t>(k) < body ? Base64Value(text[i + k])
                                               : 0;  // padding decodes as 0
      if (i + static_cast<size_t>(k) < body && v[k] < 0) return false;
    }
    // b0 needs c1 real — always true, the last group has 2..4 real chars.
    out->push_back(static_cast<char>((v[0] << 2) | (v[1] >> 4)));
    // b1 needs c2 real, b2 needs c3 real (4 chars -> 3 bytes, 3 -> 2,
    // 2 -> 1): the conditions against `body` decide per group.
    if (i + 2 < body) {
      out->push_back(static_cast<char>(((v[1] & 0x0F) << 4) | (v[2] >> 2)));
    }
    if (i + 3 < body) {
      out->push_back(static_cast<char>(((v[2] & 0x03) << 6) | v[3]));
    }
  }
  return true;
}

// --- OpenSSL EVP AES-256-GCM ------------------------------------------------

class CipherCtx {
 public:
  CipherCtx() : ctx_(EVP_CIPHER_CTX_new()) {}
  ~CipherCtx() { EVP_CIPHER_CTX_free(ctx_); }
  CipherCtx(const CipherCtx&) = delete;
  CipherCtx& operator=(const CipherCtx&) = delete;
  EVP_CIPHER_CTX* get() const { return ctx_; }

 private:
  EVP_CIPHER_CTX* ctx_;
};

// key = SHA256(KEYME.utf8) — the Python derivation (app_client.py:1132-1133).
std::array<std::uint8_t, 32> DeriveKey(const std::string& keyme) {
  infra::Sha256 hash;
  hash.Update(keyme.data(), keyme.size());
  return hash.Finish();
}

}  // namespace

// --- Clean ------------------------------------------------------------------

std::string MemoCipher::Clean(const std::string& memo) {
  std::string out;
  out.reserve(memo.size());
  size_t i = 0;
  while (i < memo.size()) {
    // Python alternation, tried left to right at every position.
    if (auto m = MatchUrl(memo, i); m.matched) {
      i = m.end;
      continue;
    }
    if (auto m = MatchTme(memo, i); m.matched) {
      i = m.end;
      continue;
    }
    if (auto m = MatchSpaceStar(memo, i); m.matched) {
      i = m.end;
      continue;
    }
    if (auto m = MatchBracketRun(memo, i); m.matched) {
      i = m.end;
      continue;
    }
    const Codepoint cp = DecodeCp(memo, i);
    out.append(memo, i, cp.length);
    i += cp.length;
  }
  return out;
}

// --- Cipher -----------------------------------------------------------------

MemoCipher::MemoCipher(std::string keyme) : keyme_(std::move(keyme)) {}

std::optional<std::string> MemoCipher::Encrypt(const std::string& memo) const {
  MemoNonce nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    spdlog::error("memo: RAND_bytes failed for nonce");
    return std::nullopt;
  }
  return EncryptWithNonce(memo, nonce);
}

std::optional<std::string> MemoCipher::EncryptWithNonce(
    const std::string& memo, const MemoNonce& nonce) const {
  if (!IsConfigured()) {
    spdlog::error("memo: KEYME is not configured — cannot encrypt");
    return std::nullopt;
  }
  const std::string plaintext = Clean(memo);
  const auto key = DeriveKey(keyme_);

  CipherCtx ctx;
  if (ctx.get() == nullptr ||
      EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(),
                         nonce.data()) != 1) {
    spdlog::error("memo: EVP encrypt init failed");
    return std::nullopt;
  }
  std::string ciphertext(plaintext.size(), '\0');
  int out_len = 0;
  if (EVP_EncryptUpdate(ctx.get(),
                        reinterpret_cast<std::uint8_t*>(ciphertext.data()),
                        &out_len,
                        reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1) {
    spdlog::error("memo: EVP encrypt update failed");
    return std::nullopt;
  }
  int final_len = 0;
  if (EVP_EncryptFinal_ex(
          ctx.get(), reinterpret_cast<std::uint8_t*>(&ciphertext[out_len]),
          &final_len) != 1) {
    spdlog::error("memo: EVP encrypt final failed");
    return std::nullopt;
  }
  ciphertext.resize(static_cast<size_t>(out_len) + static_cast<size_t>(final_len));
  std::array<std::uint8_t, kMemoTagSize> tag{};
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kMemoTagSize,
                          tag.data()) != 1) {
    spdlog::error("memo: EVP get tag failed");
    return std::nullopt;
  }

  // base64(nonce || ciphertext || tag) — the Python wire format.
  std::string raw;
  raw.reserve(nonce.size() + ciphertext.size() + tag.size());
  raw.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
  raw += ciphertext;
  raw.append(reinterpret_cast<const char*>(tag.data()), tag.size());
  return Base64Encode(raw);
}

std::optional<std::string> MemoCipher::Decrypt(const std::string& token) const {
  if (!IsConfigured()) {
    spdlog::error("memo: KEYME is not configured — cannot decrypt");
    return std::nullopt;
  }
  std::string raw;
  if (!Base64Decode(token, &raw)) return std::nullopt;
  // nonce[12] || ciphertext[>=0] || tag[16]
  if (raw.size() < kMemoNonceSize + kMemoTagSize) return std::nullopt;
  const auto* data = reinterpret_cast<const std::uint8_t*>(raw.data());
  MemoNonce nonce{};
  std::memcpy(nonce.data(), data, kMemoNonceSize);
  std::array<std::uint8_t, kMemoTagSize> tag{};
  std::memcpy(tag.data(), data + raw.size() - kMemoTagSize, kMemoTagSize);
  const std::size_t ct_size = raw.size() - kMemoNonceSize - kMemoTagSize;

  const auto key = DeriveKey(keyme_);
  CipherCtx ctx;
  if (ctx.get() == nullptr ||
      EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(),
                         nonce.data()) != 1) {
    return std::nullopt;
  }
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kMemoTagSize,
                          tag.data()) != 1) {
    return std::nullopt;
  }
  std::string plaintext(ct_size, '\0');
  int out_len = 0;
  if (EVP_DecryptUpdate(ctx.get(),
                        reinterpret_cast<std::uint8_t*>(plaintext.data()),
                        &out_len, data + kMemoNonceSize,
                        static_cast<int>(ct_size)) != 1) {
    return std::nullopt;
  }
  int final_len = 0;
  // Finalize verifies the tag; anything but success is an auth failure.
  if (EVP_DecryptFinal_ex(
          ctx.get(), reinterpret_cast<std::uint8_t*>(&plaintext[out_len]),
          &final_len) != 1) {
    return std::nullopt;
  }
  plaintext.resize(static_cast<size_t>(out_len) + static_cast<size_t>(final_len));
  return plaintext;
}

}  // namespace tg::common
