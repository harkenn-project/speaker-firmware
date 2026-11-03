#include "relay.h"

#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace relay {
namespace {

uint8_t key[32];
bool have_key = false;
uint8_t harkenn_nonce[16], speaker_nonce[16];
bool keys_ready = false;
mbedtls_gcm_context send_ctx, recv_ctx;
bool ctx_init = false;
uint64_t sent = 0, received = 0;

void mac(const char* label, uint8_t out[32]) {
  const mbedtls_md_info_t* sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, sha, 1);
  mbedtls_md_hmac_starts(&c, key, sizeof(key));
  mbedtls_md_hmac_update(&c, reinterpret_cast<const uint8_t*>(label), strlen(label));
  mbedtls_md_hmac_update(&c, harkenn_nonce, sizeof(harkenn_nonce));
  mbedtls_md_hmac_update(&c, speaker_nonce, sizeof(speaker_nonce));
  mbedtls_md_hmac_finish(&c, out);
  mbedtls_md_free(&c);
}

bool same(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

void free_ctx() {
  if (!ctx_init) return;
  mbedtls_gcm_free(&send_ctx);
  mbedtls_gcm_free(&recv_ctx);
  ctx_init = false;
}

void nonce(uint64_t counter, uint8_t out[12]) {
  memset(out, 0, 4);
  for (int i = 0; i < 8; i++) out[4 + i] = counter >> (56 - 8 * i);
}

}  // namespace

bool begin(const String& key_hex) {
  end();
  if (key_hex.length() != 64) return false;
  for (int i = 0; i < 32; i++) {
    char byte[3] = {key_hex[2 * i], key_hex[2 * i + 1], 0};
    char* rest;
    key[i] = strtoul(byte, &rest, 16);
    if (*rest) return false;
  }
  have_key = true;
  return true;
}

bool answer(const uint8_t* hello, size_t len, uint8_t out[33]) {
  if (!have_key || len != 17 || hello[0] != HELLO) return false;
  memcpy(harkenn_nonce, hello + 1, 16);
  esp_fill_random(speaker_nonce, sizeof(speaker_nonce));
  uint8_t proof[32];
  mac("harkenn-relay speaker", proof);
  out[0] = ANSWER;
  memcpy(out + 1, speaker_nonce, 16);
  memcpy(out + 17, proof, 16);
  return true;
}

bool confirm(const uint8_t* ok, size_t len) {
  if (!have_key || len != 17 || ok[0] != OK) return false;
  uint8_t proof[32];
  mac("harkenn-relay harkenn", proof);
  if (!same(proof, ok + 1, 16)) return false;
  uint8_t s2h[32], h2s[32];
  mac("harkenn-relay s2h", s2h);
  mac("harkenn-relay h2s", h2s);
  free_ctx();
  mbedtls_gcm_init(&send_ctx);
  mbedtls_gcm_init(&recv_ctx);
  ctx_init = true;
  if (mbedtls_gcm_setkey(&send_ctx, MBEDTLS_CIPHER_ID_AES, s2h, 256) ||
      mbedtls_gcm_setkey(&recv_ctx, MBEDTLS_CIPHER_ID_AES, h2s, 256)) {
    free_ctx();
    return false;
  }
  sent = received = 0;
  keys_ready = true;
  return true;
}

bool ready() { return keys_ready; }

void end() {
  keys_ready = false;
  free_ctx();
}

size_t seal(char kind, const uint8_t* data, size_t len, uint8_t* out) {
  if (!keys_ready) return 0;
  sent++;
  out[0] = DATA;
  uint8_t iv[12];
  nonce(sent, iv);
  memcpy(out + 1, iv + 4, 8);
  uint8_t* plain = out + 9;  // kind + data, encrypted in place
  plain[0] = kind;
  memcpy(plain + 1, data, len);
  if (mbedtls_gcm_crypt_and_tag(&send_ctx, MBEDTLS_GCM_ENCRYPT, len + 1, iv, sizeof(iv), nullptr, 0, plain, plain,
                                16, plain + len + 1)) {
    return 0;
  }
  return len + OVERHEAD;
}

bool open(uint8_t* frame, size_t len, char* kind, uint8_t** data, size_t* data_len) {
  if (!keys_ready || len < OVERHEAD || frame[0] != DATA) return false;
  uint64_t counter = 0;
  for (int i = 0; i < 8; i++) counter = (counter << 8) | frame[1 + i];
  if (counter <= received) return false;
  uint8_t iv[12];
  nonce(counter, iv);
  const size_t n = len - 9 - 16;
  uint8_t* body = frame + 9;
  if (mbedtls_gcm_auth_decrypt(&recv_ctx, n, iv, sizeof(iv), nullptr, 0, body + n, 16, body, body)) return false;
  received = counter;
  *kind = static_cast<char>(body[0]);
  *data = body + 1;
  *data_len = n - 1;
  return true;
}

}  // namespace relay
