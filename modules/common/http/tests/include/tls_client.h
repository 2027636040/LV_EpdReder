#ifndef HTTP_TEST_TLS_CLIENT_H
#define HTTP_TEST_TLS_CLIENT_H
#include <stddef.h>
#include <stdint.h>
#define MBEDTLS_ERR_SSL_WANT_READ (-0x6900)
#define MBEDTLS_ERR_SSL_WANT_WRITE (-0x6880)
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (-0x7880)
#define MBEDTLS_SSL_VERIFY_REQUIRED 2
typedef struct { int authmode; } mbedtls_ssl_config;
typedef struct { mbedtls_ssl_config *conf; } mbedtls_ssl_context;
typedef struct { int fd; } mbedtls_net_context;
typedef struct
{
    char *host;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_net_context server_fd;
} MbedTLSSession;
int mbedtls_client_init(MbedTLSSession *session, void *entropy, size_t size);
int mbedtls_client_context(MbedTLSSession *session);
int mbedtls_client_close(MbedTLSSession *session);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx,
    int (*send)(void *, const unsigned char *, size_t),
    int (*recv)(void *, unsigned char *, size_t), void *timeout);
int mbedtls_net_send(void *ctx, const unsigned char *buffer, size_t size);
int mbedtls_net_recv(void *ctx, unsigned char *buffer, size_t size);
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context *ssl);
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buffer, size_t size);
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buffer, size_t size);
#endif
