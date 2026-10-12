/* WWE SmackDown vs. Raw 2011 - Mbed TLS settings for the port's https
 * (src/net/tls.cpp): a TLS client over the port's own sockets, randomness
 * from the platform (mbedtls_hardware_poll), certificates from memory. */
#undef MBEDTLS_NET_C          /* (our sockets) */
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO          /* (the CA list is built in) */
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SSL_SRV_C      /* (a client only) */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
/* PSA (TLS 1.3) and our DRBG take the platform's randomness directly
 * (tls.cpp): the entropy pool isn't used - on the PS5 it called a source
 * that wasn't there. */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_PLATFORM_ZEROIZE_ALT  /* (no explicit_bzero on the PS5) */
