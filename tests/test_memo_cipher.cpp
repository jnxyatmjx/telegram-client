// MemoCipher: Python-compatible AES-256-GCM memo encryption (Telethon
// migration Chunk 1). The two cross-language vectors below were COMPUTED,
// not guessed:
//  - VECTOR_PY is the output of app_client.py's aes_gcm_encrypt (regex clean
//    + SHA256(KEYME) key + 12-byte random nonce + base64(nonce||ct||tag)),
//    generated 2026-11-10 with cryptography 50.0.2 / Python 3.12.
//  - VECTOR_CPP_FIXED_NONCE is what the same scheme must produce for the
//    fixed nonce 000102...0b — C++ reproduces it byte-exactly, and the
//    ciphertext was verified to decrypt, with the very same Python routine,
//    back to the cleaned plaintext (see mig-chunk1.log for the live run).
#include <common/MemoCipher.h>

#include <gtest/gtest.h>

#include <iostream>
#include <string>

namespace {

constexpr const char* kKeyme = "keyme-mig-chunk1-test-seed";

// Exercises every part of the clean regex: URL, t.me link, whitespace
// (incl. U+3000 spelled as its UTF-8 bytes), '*', '[', ']', '_', '-'.
const std::string kMemo =
    "Hot deal https://shop.example.com/item?ref=x  join t.me/dealchannel "
    "now!! [50%*off] code_SALE-99  \xE3\x80\x80" "end";

const std::string kCleaned = "Hotdealjoinnow!!50%offcodeSALE99end";

const std::string kVectorPy =
    "SkNglqSRTv/8XImkada4jVoYdP3sy/PXZiMFaItqjZ1l5Rd0VgBV2DMJ1NCqZ85x/qjl6dm8HBQoAuy4JE8o";

const std::string kVectorCppFixedNonce =
    "AAECAwQFBgcICQoL8CGMz52yORXv76dr7DIeypG8gRJ+31VQspbIltITy8gYoiRysq+FIS9KrHBQs/Ca7ZG5";

tg::common::MemoNonce FixedNonce() {
  tg::common::MemoNonce nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i) {
    nonce[i] = static_cast<std::uint8_t>(i);
  }
  return nonce;
}

}  // namespace

namespace memo = tg::common;

// --- Clean: the Python regex, case by case ----------------------------------

TEST(MemoClean, RemovesUrlsTmeWhitespaceAndMarkup) {
  EXPECT_EQ(memo::MemoCipher::Clean(kMemo), kCleaned);
}

TEST(MemoClean, UrlForms) {
  EXPECT_EQ(memo::MemoCipher::Clean("see https://a.b/c?d=e!"), "see");  // \S+ eats "!"
  EXPECT_EQ(memo::MemoCipher::Clean("http://x.y"), "");
  EXPECT_EQ(memo::MemoCipher::Clean("https://"), "https://");  // \S+ needs 1 char
  EXPECT_EQ(memo::MemoCipher::Clean("Http://a.b"), "Http://a.b");  // case-sensitive
}

TEST(MemoClean, TmeForms) {
  EXPECT_EQ(memo::MemoCipher::Clean("j t.me/deal now"), "jnow");
  EXPECT_EQ(memo::MemoCipher::Clean("t.me"), "t.me");      // nothing after
  EXPECT_EQ(memo::MemoCipher::Clean("t.me/"), "");         // slash consumed via backtrack
  EXPECT_EQ(memo::MemoCipher::Clean("t.me/ "), "");        // then [\s*]+ eats the space
  EXPECT_EQ(memo::MemoCipher::Clean("t.me/ x"), "x");      // verified against re.sub
  EXPECT_EQ(memo::MemoCipher::Clean("t.mefoo"), "");       // \S+ eats foo
  EXPECT_EQ(memo::MemoCipher::Clean("T.me/x"), "T.me/x");  // case-sensitive
}

TEST(MemoClean, MarkupAndWhitespaceRuns) {
  EXPECT_EQ(memo::MemoCipher::Clean("[a] *b* _c_-"), "abc");
  EXPECT_EQ(memo::MemoCipher::Clean("a  \t\n b"), "ab");
  EXPECT_EQ(memo::MemoCipher::Clean("a\xE3\x80\x80" "b"), "ab");  // U+3000
  EXPECT_EQ(memo::MemoCipher::Clean("a\xC2\xA0" "b"), "ab");      // U+00A0 NBSP
  EXPECT_EQ(memo::MemoCipher::Clean("50%"), "50%");  // '%' kept
  EXPECT_EQ(memo::MemoCipher::Clean(""), "");
}

// --- Cross-language vectors --------------------------------------------------

// Python aes_gcm_encrypt(KMEMO) -> C++ decrypts to the cleaned plaintext.
TEST(MemoCipherInterop, DecryptsPythonCiphertext) {
  const memo::MemoCipher cipher(kKeyme);
  const auto plain = cipher.Decrypt(kVectorPy);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(*plain, kCleaned);
}

// C++ encrypt (deterministic fixed nonce) -> byte-identical to what the
// Python routine computes for the same key/nonce/plaintext (verified there).
TEST(MemoCipherInterop, EncryptMatchesPythonForFixedNonce) {
  const memo::MemoCipher cipher(kKeyme);
  const auto token = cipher.EncryptWithNonce(kMemo, FixedNonce());
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(*token, kVectorCppFixedNonce);
}

// Live interop tool, DISABLED so the suite never runs it: invoke with
//   ./test_memo_cipher --gtest_also_run_disabled_tests \
//       --gtest_filter='*DumpTokenForPythonDecrypt*'
// and pipe the printed token into app_client.py's aes_gcm_decrypt (same
// KEYME). Used 2026-11-10 for the C++-encrypts/Python-decrypts check in
// mig-chunk1.log; kept for re-verification after any cipher change.
TEST(MemoCipherInterop, DISABLED_DumpTokenForPythonDecrypt) {
  const memo::MemoCipher cipher(kKeyme);
  const auto token = cipher.Encrypt(kMemo);
  ASSERT_TRUE(token.has_value());
  std::cout << "CPP_TOKEN_FOR_PYTHON:\n" << *token << std::endl;
}

// --- Behaviour ---------------------------------------------------------------

TEST(MemoCipher, RoundTripThroughEncryptDecrypt) {
  const memo::MemoCipher cipher(kKeyme);
  const auto token = cipher.Encrypt(kMemo);
  ASSERT_TRUE(token.has_value());
  const auto plain = cipher.Decrypt(*token);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(*plain, kCleaned);
}

// Regression (2026-11-10): base64 '=' padding — the decoder emitted one byte
// too many for 2- and 3-char tail groups, shifting the tag and failing auth.
// Every cleaned length 1..40 hits all padding residues.
TEST(MemoCipher, RoundTripHandlesPaddedBase64Lengths) {
  const memo::MemoCipher cipher(kKeyme);
  for (int n = 1; n <= 40; ++n) {
    const std::string memo(n, 'x');  // nothing to clean: stays n bytes
    const auto token = cipher.Encrypt(memo);
    ASSERT_TRUE(token.has_value()) << "n=" << n;
    const auto plain = cipher.Decrypt(*token);
    ASSERT_TRUE(plain.has_value()) << "n=" << n;
    EXPECT_EQ(*plain, memo) << "n=" << n;
  }
}

TEST(MemoCipher, FreshNoncePerMessage) {
  const memo::MemoCipher cipher(kKeyme);
  const auto a = cipher.Encrypt("same memo");
  const auto b = cipher.Encrypt("same memo");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(*a, *b);  // random nonces -> different bytes
}

TEST(MemoCipher, DecryptRejectsGarbageAndTampering) {
  const memo::MemoCipher cipher(kKeyme);
  EXPECT_FALSE(cipher.Decrypt("").has_value());
  EXPECT_FALSE(cipher.Decrypt("not-base64!!!").has_value());
  EXPECT_FALSE(cipher.Decrypt("AAAA").has_value());          // too short
  // Extra base64 group still decodes (and stays 4-aligned), but the tag no
  // longer matches: authentication failure, not garbage output.
  EXPECT_FALSE(cipher.Decrypt(kVectorPy + "AAAA").has_value());

  // Flip one byte inside the tag: authentication must fail.
  std::string tampered = kVectorPy;
  ASSERT_FALSE(tampered.empty());
  tampered[tampered.size() - 2] =
      tampered[tampered.size() - 2] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(cipher.Decrypt(tampered).has_value());

  // Wrong key: auth failure, not garbage output.
  const memo::MemoCipher other("a-different-seed");
  EXPECT_FALSE(other.Decrypt(kVectorPy).has_value());
}

TEST(MemoCipher, UnconfiguredCipherRefuses) {
  const memo::MemoCipher cipher("");
  EXPECT_FALSE(cipher.IsConfigured());
  EXPECT_FALSE(cipher.Encrypt("x").has_value());
  EXPECT_FALSE(cipher.Decrypt(kVectorPy).has_value());
}

TEST(MemoCipher, ValueSemantics) {
  const memo::MemoCipher original(kKeyme);
  const memo::MemoCipher copy = original;  // copies are self-contained
  const auto plain = copy.Decrypt(kVectorPy);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(*plain, kCleaned);
}
