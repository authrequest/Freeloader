// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Traffic logger for Plex Media Server.
// LD_PRELOAD library that hooks socket syscalls and logs all network traffic.
//
// Activation:
//   export PLEX_TRAFFIC_LOG=/tmp/pms_traffic.log
//   LD_PRELOAD=.../plexmediaserver_traffic_logger.so ...
//
// Log format:
//   [HH:MM:SS.mmm] DIR fd=N [peer:port] N bytes
//   00000000  48 54 54 50 2f 31 2e 31 20  32 30 30 20 4f 4b     HTTP/1.1 200 OK
//   ...
//
// Limitations:
//   - Connection tracking uses a fixed-size array (CONN_MAX = 4096).
//     FDs beyond that are logged without peer address.
//   - No sendmsg/recvmsg support (fallback to byte-count-only log).

#include "traffic_logger.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void*)(-1))
#endif

// ── Constants ──────────────────────────────────────────────────────────────

/// Maximum bytes to hex-dump per log entry. 0 = dump entire payload.
static constexpr size_t MAX_DUMP = 256;

/// Maximum tracked file descriptors.
static constexpr int CONN_MAX = 4096;

/// Maximum hex dump line width (bytes displayed per line).
static constexpr int HEX_COLS = 16;

/// Format string length buffers.
static constexpr int TS_LEN = 32;
static constexpr int PEER_LEN = 48;

// ── Per-connection state ───────────────────────────────────────────────────

struct ConnInfo
{
    char     peer[PEER_LEN]; // "1.2.3.4:56789" or "[::1]:56789"
    uint64_t start_ns;       // monotonic timestamp of connect/accept
    bool     active;
};

/// Connection table indexed by file descriptor.
static ConnInfo g_conns[CONN_MAX];

/// Guards g_conns.
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

/// Log file descriptor, or -1 if inactive.
static int g_log_fd = -1;

/// Guards g_log_fd writes.
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/// Whether we failed initialisation (suppress further attempts).
static bool g_init_failed = false;

// ── Real function pointers (resolved via dlsym) ────────────────────────────

extern "C"
{
    static ssize_t  (*real_send)(int, const void*, size_t, int) = nullptr;
    static ssize_t  (*real_sendto)(int, const void*, size_t, int,
                                    const struct sockaddr*, socklen_t) = nullptr;
    static ssize_t  (*real_recv)(int, void*, size_t, int) = nullptr;
    static ssize_t  (*real_recvfrom)(int, void*, size_t, int,
                                      struct sockaddr*, socklen_t*) = nullptr;
    static int      (*real_connect)(int, const struct sockaddr*, socklen_t) = nullptr;
    static int      (*real_accept)(int, struct sockaddr*, socklen_t*) = nullptr;
    static int      (*real_accept4)(int, struct sockaddr*, socklen_t*, int) = nullptr;
    static int      (*real_close)(int) = nullptr;
}

// ── Helpers ────────────────────────────────────────────────────────────────

/// Monotonic time in nanoseconds.
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/// Format a sockaddr as "ip:port" into buf.
static void fmt_peer(const struct sockaddr* sa, socklen_t salen, char* buf, size_t bufsize)
{
    if(!sa || salen < sizeof(sa_family_t))
    {
        snprintf(buf, bufsize, "???");
        return;
    }

    if(sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in))
    {
        const auto* sin = (const struct sockaddr_in*)sa;
        inet_ntop(AF_INET, &sin->sin_addr, buf, bufsize);
        size_t n = strlen(buf);
        snprintf(buf + n, bufsize - n, ":%d", (int)ntohs(sin->sin_port));
    }
    else if(sa->sa_family == AF_INET6 && salen >= sizeof(struct sockaddr_in6))
    {
        const auto* sin6 = (const struct sockaddr_in6*)sa;
        buf[0] = '[';
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf + 1, bufsize - 8);
        size_t n = strlen(buf);
        snprintf(buf + n, bufsize - n, "]:%d", (int)ntohs(sin6->sin6_port));
    }
    else
    {
        snprintf(buf, bufsize, "af=%d", sa->sa_family);
    }
}

/// Return the peer string for an fd, or "?" if unknown.
static const char* peer_for_fd(int fd, char* fallback_buf)
{
    if(fd >= 0 && fd < CONN_MAX)
    {
        pthread_mutex_lock(&g_conn_lock);
        if(g_conns[fd].active)
        {
            strcpy(fallback_buf, g_conns[fd].peer);
            pthread_mutex_unlock(&g_conn_lock);
            return fallback_buf;
        }
        pthread_mutex_unlock(&g_conn_lock);
    }
    fallback_buf[0] = '?';
    fallback_buf[1] = '\0';
    return fallback_buf;
}

// ── Logging ────────────────────────────────────────────────────────────────

/// Write a hex+ASCII dump of `len` bytes starting at `data`.
static void log_hexdump(const unsigned char* data, size_t len)
{
    if(len == 0 || g_log_fd < 0) return;

    if(len > MAX_DUMP) len = MAX_DUMP;

    // Local line buffer to minimise syscall count.
    char line[128];
    size_t off = 0;

    for(size_t i = 0; i < len; i += HEX_COLS)
    {
        size_t remain = len - i;
        size_t row    = remain < HEX_COLS ? remain : HEX_COLS;

        // Address.
        off = (size_t)snprintf(line, sizeof(line), "  %08zx  ", i);

        // Hex bytes (left half, right half).
        size_t h;
        for(h = 0; h < row && h < 8; h++)
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %02x", data[i + h]);
        if(row <= 8)
            off += (size_t)snprintf(line + off, sizeof(line) - off, "  "); // spacer
        for(; h < row; h++)
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %02x", data[i + h]);
        // Pad to column 60.
        for(size_t p = row; p < HEX_COLS; p++)
        {
            off += (size_t)snprintf(line + off, sizeof(line) - off, "   ");
            if(p == 7) off += (size_t)snprintf(line + off, sizeof(line) - off, " ");
        }

        // ASCII view.
        off += (size_t)snprintf(line + off, sizeof(line) - off, "  |");
        for(size_t j = 0; j < row; j++)
        {
            unsigned char c = data[i + j];
            line[off++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[off++] = '|';
        line[off++] = '\n';
        write(g_log_fd, line, off);
    }

    if(len < MAX_DUMP && len > 0)
    {
        // No truncation needed.
    }
}

/// Write a timestamped log line with metadata and optional hex dump.
static void log_traffic(char dir, int fd, const char* peer,
                         const unsigned char* data, size_t len,
                         const char* suffix)
{
    if(g_log_fd < 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char timestamp[TS_LEN];
    strftime(timestamp, sizeof(timestamp), "%T", &tm_buf);

    char line[512];
    int n = snprintf(line, sizeof(line),
                     "[%s.%03ld] %c %s fd=%d [%s]%s%zu byte%s\n",
                     timestamp, (long)(ts.tv_nsec / 1000000),
                     dir,
                     (dir == 'S' || dir == 's') ? "SEND" :
                     (dir == 'R' || dir == 'r') ? "RECV" :
                     (dir == 'C')               ? "CONN" :
                     (dir == 'A')               ? "ACPT" :
                     (dir == 'X')               ? "CLSE" :
                                                  "????",
                     fd, peer,
                     suffix ? suffix : "",
                     len, len == 1 ? "" : "s");

    pthread_mutex_lock(&g_log_lock);
    write(g_log_fd, line, (size_t)n);
    if(data && len > 0)
    {
        log_hexdump(data, len);
    }
    pthread_mutex_unlock(&g_log_lock);
}

// ── Hooked functions ───────────────────────────────────────────────────────

extern "C" ssize_t send(int sockfd, const void* buf, size_t len, int flags)
{
    if(!real_send) return (ssize_t)-1;
    ssize_t ret = real_send(sockfd, buf, len, flags);

    char fallback[PEER_LEN];
    log_traffic('S', sockfd, peer_for_fd(sockfd, fallback),
                 (const unsigned char*)buf, (size_t)(ret > 0 ? ret : 0), nullptr);
    return ret;
}

extern "C" ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
                           const struct sockaddr* dest_addr, socklen_t addrlen)
{
    if(!real_sendto) return (ssize_t)-1;
    ssize_t ret = real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);

    if(ret > 0)
    {
        char peer_buf[PEER_LEN];
        if(dest_addr)
        {
            fmt_peer(dest_addr, addrlen, peer_buf, sizeof(peer_buf));
        }
        else
        {
            char fallback[PEER_LEN];
            strcpy(peer_buf, peer_for_fd(sockfd, fallback));
        }
        log_traffic('s', sockfd, peer_buf,
                     (const unsigned char*)buf, (size_t)ret, nullptr);
    }
    return ret;
}

extern "C" ssize_t recv(int sockfd, void* buf, size_t len, int flags)
{
    if(!real_recv) return (ssize_t)-1;
    ssize_t ret = real_recv(sockfd, buf, len, flags);

    if(ret > 0)
    {
        char fallback[PEER_LEN];
        log_traffic('R', sockfd, peer_for_fd(sockfd, fallback),
                     (const unsigned char*)buf, (size_t)ret, nullptr);
    }
    return ret;
}

extern "C" ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                             struct sockaddr* src_addr, socklen_t* addrlen)
{
    if(!real_recvfrom) return (ssize_t)-1;
    ssize_t ret = real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);

    if(ret > 0)
    {
        char peer_buf[PEER_LEN];
        if(src_addr && addrlen)
        {
            fmt_peer(src_addr, *addrlen, peer_buf, sizeof(peer_buf));
        }
        else
        {
            char fallback[PEER_LEN];
            strcpy(peer_buf, peer_for_fd(sockfd, fallback));
        }
        log_traffic('r', sockfd, peer_buf,
                     (const unsigned char*)buf, (size_t)ret, nullptr);
    }
    return ret;
}

extern "C" int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen)
{
    int ret = real_connect ? real_connect(sockfd, addr, addrlen) : -1;

    // Track the connection even if it fails (EINPROGRESS is normal for non-blocking).
    if(addr)
    {
        char peer_buf[PEER_LEN];
        fmt_peer(addr, addrlen, peer_buf, sizeof(peer_buf));

        if(sockfd >= 0 && sockfd < CONN_MAX)
        {
            pthread_mutex_lock(&g_conn_lock);
            g_conns[sockfd].active   = true;
            g_conns[sockfd].start_ns = now_ns();
            strcpy(g_conns[sockfd].peer, peer_buf);
            pthread_mutex_unlock(&g_conn_lock);
        }

        log_traffic('C', sockfd, peer_buf, nullptr, 0,
                     ret == 0 ? nullptr : (errno == EINPROGRESS ? " (EINPROGRESS)" : " (FAIL)"));
    }
    return ret;
}

extern "C" int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen)
{
    int client = real_accept ? real_accept(sockfd, addr, addrlen) : -1;

    if(client >= 0 && addr && addrlen)
    {
        char peer_buf[PEER_LEN];
        fmt_peer(addr, *addrlen, peer_buf, sizeof(peer_buf));

        if(client < CONN_MAX)
        {
            pthread_mutex_lock(&g_conn_lock);
            g_conns[client].active   = true;
            g_conns[client].start_ns = now_ns();
            strcpy(g_conns[client].peer, peer_buf);
            pthread_mutex_unlock(&g_conn_lock);
        }

        log_traffic('A', client, peer_buf, nullptr, 0, nullptr);

        // Also log the accept call from the listening socket perspective.
        char local[PEER_LEN];
        snprintf(local, sizeof(local), "LISTEN");
        log_traffic('a', sockfd, local, nullptr, 0, nullptr);
    }
    return client;
}

extern "C" int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags)
{
    int client = real_accept4 ? real_accept4(sockfd, addr, addrlen, flags) : -1;

    if(client >= 0 && addr && addrlen)
    {
        char peer_buf[PEER_LEN];
        fmt_peer(addr, *addrlen, peer_buf, sizeof(peer_buf));

        if(client < CONN_MAX)
        {
            pthread_mutex_lock(&g_conn_lock);
            g_conns[client].active   = true;
            g_conns[client].start_ns = now_ns();
            strcpy(g_conns[client].peer, peer_buf);
            pthread_mutex_unlock(&g_conn_lock);
        }

        log_traffic('A', client, peer_buf, nullptr, 0, nullptr);
    }
    return client;
}

extern "C" int close(int fd)
{
    if(fd >= 0 && fd < CONN_MAX)
    {
        pthread_mutex_lock(&g_conn_lock);
        bool was_active = g_conns[fd].active;
        if(was_active)
        {
            uint64_t age_ns = now_ns() - g_conns[fd].start_ns;
            char info[64];
            if(g_conns[fd].start_ns > 0)
            {
                unsigned long secs = (unsigned long)(age_ns / 1000000000ULL);
                snprintf(info, sizeof(info), " (lifetime %lus)", secs);
            }
            else
            {
                info[0] = '\0';
            }
            log_traffic('X', fd, g_conns[fd].peer, nullptr, 0, info);
            g_conns[fd].active = false;
        }
        pthread_mutex_unlock(&g_conn_lock);
    }

    return real_close ? real_close(fd) : -1;
}

// ── Initialisation ─────────────────────────────────────────────────────────

__attribute__((constructor)) void traffic_logger_init(void)
{
    if(g_init_failed) return;

    const char* log_path = getenv("PLEX_TRAFFIC_LOG");

    // Always resolve real function pointers so hooks work correctly even when
    // logging is disabled. Otherwise every hooked call returns -1 (the nullptr
    // guard), which causes PMS to spin at 100 % CPU retrying failed I/O.
    bool ok = true;
    bool log_active = false;

#define RESOLVE(name_, var_) do { \
    var_ = reinterpret_cast<decltype(var_)>(dlsym(RTLD_NEXT, name_)); \
    if(!var_) { ok = false; } \
} while(0)

    RESOLVE("send",     real_send);
    RESOLVE("sendto",   real_sendto);
    RESOLVE("recv",     real_recv);
    RESOLVE("recvfrom", real_recvfrom);
    RESOLVE("connect",  real_connect);
    RESOLVE("accept",   real_accept);
    RESOLVE("accept4",  real_accept4);
    RESOLVE("close",    real_close);

#undef RESOLVE

    // Open log file if env var is set.
    if(log_path && log_path[0] != '\0')
    {
        g_log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if(g_log_fd >= 0)
        {
            log_active = true;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm tm_buf;
            localtime_r(&ts.tv_sec, &tm_buf);
            char ts_buf[64];
            strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %T", &tm_buf);

            char hdr[256];
            int n = snprintf(hdr, sizeof(hdr),
                             "# Plex Media Server traffic log — started %s\n"
                             "# PID=%d  LOG_PATH=%s  MAX_DUMP=%zu\n"
                             "## [time] DIR fd [peer] N bytes [hex dump]\n",
                             ts_buf, (int)getpid(), log_path, MAX_DUMP);
            write(g_log_fd, hdr, (size_t)(n > 0 ? n : 0));

            char msg[128];
            n = snprintf(msg, sizeof(msg),
                         "# traffic logger initialized — %d hooks installed\n",
                         ok ? 8 : 0);
            write(g_log_fd, msg, (size_t)(n > 0 ? n : 0));
        }
    }

    // When env var is unset: pointers are resolved, g_log_fd stays -1, hooks
    // call through to libc with no logging overhead (no-op fast path).
    if(!ok)
    {
        g_init_failed = true;
        if(g_log_fd >= 0)
        {
            const char* warn = "# WARNING: some dlsym(RTLD_NEXT, ...) calls failed — hooks degraded\n";
            write(g_log_fd, warn, strlen(warn));
        }
    }
}
