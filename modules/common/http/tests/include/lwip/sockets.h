#ifndef HTTP_TEST_LWIP_SOCKETS_H
#define HTTP_TEST_LWIP_SOCKETS_H
#include <stddef.h>
#include <stdint.h>
/* Isolated names avoid collisions with the host C runtime's socket types. */
#define sockaddr http_test_sockaddr
#define sockaddr_in http_test_sockaddr_in
#define socklen_t http_test_socklen_t
#define fd_set http_test_fd_set
#define timeval http_test_timeval
#define socket http_test_socket
#define bind http_test_bind
#define connect http_test_connect
#define select http_test_select
#define getsockopt http_test_getsockopt
#define ioctlsocket http_test_ioctlsocket
#define htons http_test_htons
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define SOL_SOCKET 1
#define SO_ERROR 4
#define FIONBIO 1
#ifdef FD_ZERO
#undef FD_ZERO
#undef FD_SET
#endif
#define FD_ZERO(set) ((set)->fd = -1)
#define FD_SET(value, set) ((set)->fd = (value))
typedef unsigned socklen_t;
typedef struct { int fd; } fd_set;
struct timeval { long tv_sec, tv_usec; };
struct sockaddr { unsigned short family; };
struct sockaddr_in
{
    unsigned short sin_family, sin_port;
    struct { uint32_t s_addr; } sin_addr;
};
int socket(int family, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t size);
int connect(int fd, const struct sockaddr *address, socklen_t size);
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout);
int getsockopt(int fd, int level, int name, void *value, socklen_t *size);
int ioctlsocket(int fd, long command, void *value);
unsigned short htons(unsigned short value);
#endif
