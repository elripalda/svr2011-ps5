// WWE SmackDown vs. Raw 2011 - https over Mbed TLS (see tls.h).
#include "tls.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <rex/logging.h>

#if defined(__PROSPERO__)
#include <sys/sysctl.h>
// (weak: an app may not get libSceRandom - its import was a call to 0)
extern "C" int sceRandomGetRandomNumber(void* buf, size_t size) __attribute__((weak));
#else
#include <sys/random.h>
#endif

// The Mozilla CA list (PEM, ending in a NUL), built into the program.
#define SVR_INCBIN(name, file)                                                        \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
SVR_INCBIN(svr_cacert, SVR2011_CERTS_DIR "/cacert.pem")

// Randomness for Mbed TLS (MBEDTLS_ENTROPY_HARDWARE_ALT): the platform's.
extern "C" int mbedtls_hardware_poll(void*, unsigned char* out, size_t len, size_t* olen) {
  size_t done = 0;
#if defined(__PROSPERO__)
  // the kernel's random numbers (kern.arandom), at most 256 bytes a call
  while (done < len) {
    int mib[2] = {CTL_KERN, KERN_ARND};
    size_t n = std::min<size_t>(len - done, 256);
    if (sysctl(mib, 2, out + done, &n, nullptr, 0) != 0 || n == 0) break;
    done += n;
  }
  // else the system library's (when the app has it)
  while (done < len && sceRandomGetRandomNumber) {
    const size_t n = std::min<size_t>(len - done, 64);  // (at most 64 bytes a call)
    if (sceRandomGetRandomNumber(out + done, n) != 0) break;
    done += n;
  }
  static bool said = false;
  if (!said) {
    said = true;
    if (done != len) REXLOG_ERROR("online: no random numbers for https on this console");
  }
#else
  while (done < len) {
    const ssize_t n = getrandom(out + done, len - done, 0);
    if (n <= 0) break;
    done += size_t(n);
  }
#endif
  *olen = done;
  return done == len ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

// PSA's randomness (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG): the platform's.
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t*, uint8_t* out,
                                                         size_t size, size_t* got) {
  size_t n = 0;
  const int rc = mbedtls_hardware_poll(nullptr, out, size, &n);
  *got = n;
  return rc == 0 && n == size ? PSA_SUCCESS : PSA_ERROR_INSUFFICIENT_ENTROPY;
}

// A memset that the compiler keeps (MBEDTLS_PLATFORM_ZEROIZE_ALT).
extern "C" void mbedtls_platform_zeroize(void* buf, size_t len) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(buf);
  while (len--) *p++ = 0;
}

namespace svr2011::net {

namespace {

// Seeds the DRBG straight from the platform (no entropy pool).
int PlatformEntropy(void*, unsigned char* out, size_t len) {
  size_t got = 0;
  return mbedtls_hardware_poll(nullptr, out, len, &got);
}

// What every connection shares: randomness and the CA list (made once).
struct Shared {
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt cas;
  bool ok = false;
};

Shared* GetShared() {
  static Shared* shared = [] {
    auto* s = new Shared;
    REXLOG_INFO("online: https: starting the crypto library");
    rex::FlushLogging();
    if (const psa_status_t ps = psa_crypto_init(); ps != PSA_SUCCESS) {
      REXLOG_ERROR("online: the crypto library didn't start ({})", int(ps));
      return s;
    }
    REXLOG_INFO("online: https: seeding");
    rex::FlushLogging();
    mbedtls_ctr_drbg_init(&s->drbg);
    mbedtls_x509_crt_init(&s->cas);
    static const char kPers[] = "svr2011 tls";
    int rc = mbedtls_ctr_drbg_seed(&s->drbg, PlatformEntropy, nullptr,
                                   reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers));
    if (rc) {
      REXLOG_ERROR("online: no randomness for https (-{:#x})", unsigned(-rc));
      return s;
    }
    REXLOG_INFO("online: https: reading the certificate list");
    rex::FlushLogging();
    rc = mbedtls_x509_crt_parse(&s->cas, svr_cacert, size_t(svr_cacert_end - svr_cacert) + 1);
    if (rc < 0) {
      REXLOG_ERROR("online: the built-in certificate list couldn't be read (-{:#x})", unsigned(-rc));
      return s;
    }
    s->ok = true;
    REXLOG_INFO("online: https ready ({} certificates)", [&] {
      int n = 0;
      for (const mbedtls_x509_crt* c = &s->cas; c && c->raw.len; c = c->next) ++n;
      return n;
    }());
    return s;
  }();
  return shared;
}
std::mutex g_drbg_mutex;  // (ctr_drbg is shared between connections)

int Random(void* p, unsigned char* out, size_t len) {
  std::lock_guard lock(g_drbg_mutex);
  return mbedtls_ctr_drbg_random(p, out, len);
}

int SendCb(void* ctx, const unsigned char* buf, size_t len) {
  const ssize_t n = send(*static_cast<int*>(ctx), buf, len, 0);
  if (n >= 0) return int(n);
  return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x004E;  // (MBEDTLS_ERR_NET_SEND_FAILED)
}

int RecvCb(void* ctx, unsigned char* buf, size_t len) {
  const ssize_t n = recv(*static_cast<int*>(ctx), buf, len, 0);
  if (n >= 0) return int(n);
  return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : -0x004C;  // (MBEDTLS_ERR_NET_RECV_FAILED; a timeout too)
}

std::string Error(int rc) {
  char buf[160];
  mbedtls_strerror(rc, buf, sizeof(buf));
  return buf;
}

}  // namespace

struct Tls::State {
  int socket = -1;
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  State() {
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
  }
  ~State() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
  }
};

std::unique_ptr<Tls> Tls::Start(int socket, const std::string& host) {
  Shared* shared = GetShared();
  if (!shared->ok) return nullptr;
  std::unique_ptr<Tls> t(new Tls);
  t->s_ = std::make_unique<State>();
  State& s = *t->s_;
  s.socket = socket;
  int rc = mbedtls_ssl_config_defaults(&s.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc) {
    REXLOG_ERROR("online: https setup failed: {}", Error(rc));
    return nullptr;
  }
  mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&s.conf, &shared->cas, nullptr);
  mbedtls_ssl_conf_rng(&s.conf, Random, &shared->drbg);
  if ((rc = mbedtls_ssl_setup(&s.ssl, &s.conf)) || (rc = mbedtls_ssl_set_hostname(&s.ssl, host.c_str()))) {
    REXLOG_ERROR("online: https setup failed: {}", Error(rc));
    return nullptr;
  }
  mbedtls_ssl_set_bio(&s.ssl, &s.socket, SendCb, RecvCb, nullptr);
  REXLOG_INFO("online: https: handshake with {}", host);
  while ((rc = mbedtls_ssl_handshake(&s.ssl)) != 0) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    const uint32_t flags = mbedtls_ssl_get_verify_result(&s.ssl);
    if (flags && flags != uint32_t(-1)) {
      char why[256];
      mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
      REXLOG_ERROR("online: {}'s certificate wasn't accepted: {}", host, why);
    } else {
      REXLOG_ERROR("online: https with {} failed: {}", host, Error(rc));
    }
    return nullptr;
  }
  REXLOG_INFO("online: https with {}: {}", host, mbedtls_ssl_get_version(&s.ssl));
  return t;
}

Tls::~Tls() {
  if (s_) mbedtls_ssl_close_notify(&s_->ssl);
}

bool Tls::SendAll(const void* data, size_t size) {
  const auto* p = static_cast<const unsigned char*>(data);
  while (size) {
    const int n = mbedtls_ssl_write(&s_->ssl, p, size);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (n <= 0) return false;
    p += n, size -= size_t(n);
  }
  return true;
}

int Tls::Recv(void* buf, size_t size) {
  for (;;) {
    const int n = mbedtls_ssl_read(&s_->ssl, static_cast<unsigned char*>(buf), size);
    if (n >= 0) return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    // (TLS 1.3: a session ticket after the handshake - not data, read on)
    if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return -1;
  }
}

}  // namespace svr2011::net
