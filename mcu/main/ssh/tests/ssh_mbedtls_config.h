/* Host transport tests use the SDK's exact mbedTLS sources and cipher backend. */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_PLATFORM_C
