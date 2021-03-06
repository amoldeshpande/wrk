#ifndef WRK_H
#define WRK_H

#include "config.h"
#if !_MSC_VER
#include <pthread.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>
#else
#include <wincompat.h>
#endif

#include <inttypes.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <luajit-2.0/lua.h>

#include "stats.h"
#include "ae.h"
#include "http_parser.h"

#define RECVBUF  8192

#define MAX_THREAD_RATE_S   10000000
#define SOCKET_TIMEOUT_MS   2000
#define RECORD_INTERVAL_MS  100

#if !_MSC_VER
extern const char *VERSION;
#endif 

typedef struct {
    pthread_t thread;
	aeEventLoop *loop;
#if _MSC_VER
	HANDLE hCompletionPort;
#endif
    struct addrinfo *addr;
    uint64_t connections;
    uint64_t complete;
    uint64_t requests;
    uint64_t bytes;
    uint64_t start;
    lua_State *L;
    errors errors;
    struct connection *cs;
} thread;

typedef struct {
    char  *buffer;
    size_t length;
    char  *cursor;
} buffer;

typedef struct connection {
    thread *thread;
    http_parser parser;
    enum {
        FIELD, VALUE
    } state;
#if !_MSC_VER
    int fd;
#else
	fd_t fd;
#endif
    SSL *ssl;
    bool delayed;
    uint64_t start;
    char *request;
    size_t length;
    size_t written;
    uint64_t pending;
    buffer headers;
    buffer body;
    char buf[RECVBUF];
} connection;

#endif /* WRK_H */
