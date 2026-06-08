// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Webhook HTTP interceptor for Plex Media Server.
// LD_PRELOAD library component that hooks POSIX socket functions to intercept
// /api/v2/user/webhooks HTTP requests and serve webhook CRUD from a JSON file.
//
// How it works:
//   1. read()/recv() hook: detect incoming requests containing
//      "/api/v2/user/webhooks" in the URL, parse method/path/body, store in
//      per-FD state.
//   2. write()/send() hook: when the PMS tries to write the 404 response for
//      a webhook FD, intercept and serve our own JSON response instead.
//   3. close() hook: clean up per-FD state.
//
// The webhooks JSON file is stored at /var/lib/plexmediaserver/webhooks.json
// (overridable via PLEX_WEBHOOKS_FILE env var).

#include "webhook_handler.hpp"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void*)(-1))
#endif

// ── Constants ──────────────────────────────────────────────────────────────

/// Max tracked file descriptors.
#define MAX_FDS 4096

/// Max webhooks.
#define MAX_WEBHOOKS 64

/// Max request body size.
#define MAX_BODY 8192

/// Max path length.
#define MAX_PATH 512

/// Default webhooks JSON path.
#define DEFAULT_WEBHOOKS_PATH "/var/lib/plexmediaserver/webhooks.json"

// ── State enums ────────────────────────────────────────────────────────────

enum WhFdState {
    WH_FD_NONE = 0,
    WH_FD_DETECTED, // read/recv saw a webhook request
    WH_FD_HANDLED,  // write/send intercepted and replaced
};

// ── Per-FD state structs ───────────────────────────────────────────────────

struct WhRequest {
    char method[8];       // GET, POST, PUT, DELETE
    char path[MAX_PATH];  // /api/v2/user/webhooks or /api/v2/user/webhooks/ID
    char body[MAX_BODY];  // request body (for POST/PUT)
    size_t body_len;      // actual body length
    bool has_body;        // true if body content was captured
};

struct WhFdSlot {
    enum WhFdState state;
    struct WhRequest req;
    /// Accumulates the start of an HTTP request line when the PMS reads it in
    /// multiple recvfrom() calls (first 1 byte, then the rest). This happens
    /// because PMS probes the socket with a 1-byte read before reading the
    /// full request — we need to reassemble to get "GET" not just "ET".
    char preamble[32];
    size_t preamble_len;
};

// ── Webhook entry ──────────────────────────────────────────────────────────

struct WhEntry {
    char id[64];
    char url[2048];
    char encoding[32];
    char format[32];
    bool active;
    bool valid; // true when this slot is occupied
};

struct WhParsedUrl {
    char url[2048];
};

// ── Global state ───────────────────────────────────────────────────────────

/// Real function pointers for socket calls.
/// NOTE: write() and send() are NOT hooked — PMS uses sendmsg() for HTTP
/// output and recvfrom() for HTTP input. Hooking write() caused SEGV during
/// shutdown because PMS calls write() on many FDs and our hook could falsely
/// intercept log/file writes with reused FD numbers.
static ssize_t (*real_read)(int, void*, size_t) = NULL;
static ssize_t (*real_recv)(int, void*, size_t, int) = NULL;
static ssize_t (*real_recvfrom)(int, void*, size_t, int, struct sockaddr*, socklen_t*) = NULL;
static ssize_t (*real_sendmsg)(int, const struct msghdr*, int) = NULL;
static int (*real_close)(int) = NULL;
static bool g_handler_ready = false;

/// Per-FD tracking table (protected by g_fd_lock).
static struct WhFdSlot g_fds[MAX_FDS];
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;

/// Webhook list cache (protected by g_wh_lock).
static struct WhEntry g_webhooks[MAX_WEBHOOKS];
static int g_webhook_count = 0;
static pthread_mutex_t g_wh_lock = PTHREAD_MUTEX_INITIALIZER;

/// Path to webhooks JSON file (resolved once from env or default).
static char g_wh_path[1024] = "";
static bool g_wh_path_resolved = false;
static pthread_mutex_t g_wh_path_lock = PTHREAD_MUTEX_INITIALIZER;

/// Resolved path — mutable buffer returned for convenience (caller does not own).
static const char* wh_path(void)
{
    if(!g_wh_path_resolved)
    {
        pthread_mutex_lock(&g_wh_path_lock);
        if(!g_wh_path_resolved)
        {
            const char* env = getenv("PLEX_WEBHOOKS_FILE");
            size_t n;
            if(env && env[0])
            {
                n = strlen(env);
                if(n >= sizeof(g_wh_path)) n = sizeof(g_wh_path) - 1;
                memcpy(g_wh_path, env, n);
            }
            else
            {
                n = strlen(DEFAULT_WEBHOOKS_PATH);
                memcpy(g_wh_path, DEFAULT_WEBHOOKS_PATH, n);
            }
            g_wh_path[n] = '\0';
            g_wh_path_resolved = true;
        }
        pthread_mutex_unlock(&g_wh_path_lock);
    }
    return g_wh_path;
}

// ── File I/O helpers ───────────────────────────────────────────────────────

/// Read entire file into a malloc'd buffer. Caller must free(). Returns NULL on
/// failure (file missing, empty, or error).
static char* read_file(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;

    struct stat st;
    if(fstat(fileno(f), &st) != 0 || st.st_size == 0)
    {
        fclose(f);
        return NULL;
    }

    long fsize = st.st_size;
    if(fsize > 1024 * 1024)
    {
        // Sanity cap at 1 MB.
        fclose(f);
        return NULL;
    }

    char* buf = (char*)malloc((size_t)fsize + 1);
    if(!buf)
    {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    buf[nread] = '\0';
    if(out_len) *out_len = nread;
    return buf;
}

/// Atomically write buffer to file (temp + rename).
static bool write_file(const char* path, const char* data, size_t len)
{
    // Build temp path.
    size_t plen = strlen(path);
    char* tmp = (char*)malloc(plen + 8);
    if(!tmp) return false;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    tmp[plen + 4] = '\0';

    FILE* f = fopen(tmp, "wb");
    if(!f)
    {
        free(tmp);
        return false;
    }

    bool ok = true;
    if(len > 0 && fwrite(data, 1, len, f) != len) ok = false;
    fclose(f);

    if(ok)
    {
        if(rename(tmp, path) != 0) ok = false;
    }

    if(!ok) remove(tmp);
    free(tmp);
    return ok;
}

// ── Minimal JSON parser (webhook format only) ──────────────────────────────
//
// Parses: [{"id":"...","url":"...","encoding":"...","format":"...","active":true/false}]
// Handles quoted strings with basic escaping, booleans, and ignores unknown keys.

/// Skip whitespace characters.
static void skip_ws(const char** p)
{
    while(**p && (unsigned char)**p <= ' ') (*p)++;
}

/// Parse a JSON string value. Advances *p past closing ".
/// Returns pointer to the parsed string (static buffer, overwritten each call).
/// Returns NULL on syntax error.
static const char* parse_string(const char** p)
{
    skip_ws(p);
    if(**p != '"') return NULL;
    (*p)++; // skip opening "

    static char buf[2048];
    size_t i = 0;
    while(**p && **p != '"' && i < sizeof(buf) - 1)
    {
        if(**p == '\\')
        {
            (*p)++; // skip backslash
            if(**p) buf[i++] = *(*p)++;
            continue;
        }
        buf[i++] = *(*p)++;
    }
    buf[i] = '\0';

    if(**p != '"') return NULL;
    (*p)++; // skip closing "
    return buf;
}

/// Parse a JSON value that is either a string or a boolean literal.
/// Returns true on success, false on syntax error.
static bool parse_value(const char** p, char* str_out, size_t str_size, bool* bool_out)
{
    skip_ws(p);
    if(**p == '"')
    {
        const char* s = parse_string(p);
        if(!s) return false;
        strncpy(str_out, s, str_size - 1);
        str_out[str_size - 1] = '\0';
        return true;
    }
    if(strncmp(*p, "true", 4) == 0)
    {
        *bool_out = true;
        *p += 4;
        return true;
    }
    if(strncmp(*p, "false", 5) == 0)
    {
        *bool_out = false;
        *p += 5;
        return true;
    }
    return false;
}

/// Parse one JSON webhook object: {"id":"...","url":"...",...}
static bool parse_one_webhook(const char** p, struct WhEntry* entry)
{
    memset(entry, 0, sizeof(*entry));

    skip_ws(p);
    if(**p != '{') return false;
    (*p)++; // skip {

    while(**p)
    {
        skip_ws(p);
        if(**p == '}') { (*p)++; entry->valid = true; return true; }
        if(**p == ',') { (*p)++; continue; }

        // Parse key
        const char* key = parse_string(p);
        if(!key) return false;

        skip_ws(p);
        if(**p != ':') return false;
        (*p)++; // skip :
        skip_ws(p);

        // Branch on key
        if(strcmp(key, "id") == 0)
        {
            const char* v = parse_string(p);
            if(v) { strncpy(entry->id, v, sizeof(entry->id) - 1); }
        }
        else if(strcmp(key, "url") == 0)
        {
            const char* v = parse_string(p);
            if(v) { strncpy(entry->url, v, sizeof(entry->url) - 1); }
        }
        else if(strcmp(key, "encoding") == 0)
        {
            const char* v = parse_string(p);
            if(v) { strncpy(entry->encoding, v, sizeof(entry->encoding) - 1); }
        }
        else if(strcmp(key, "format") == 0)
        {
            const char* v = parse_string(p);
            if(v) { strncpy(entry->format, v, sizeof(entry->format) - 1); }
        }
        else if(strcmp(key, "active") == 0)
        {
            parse_value(p, NULL, 0, &entry->active);
        }
        else
        {
            // Unknown key — skip value
            if(**p == '"') { const char* _skip = parse_string(p); (void)_skip; }
            else if(strncmp(*p, "true", 4) == 0) *p += 4;
            else if(strncmp(*p, "false", 5) == 0) *p += 5;
            else { /* skip unexpected */ while(**p && **p != ',' && **p != '}') (*p)++; }
        }
    }
    return false;
}

/// Parse JSON array of webhook objects.
static bool parse_webhooks(const char* json, size_t len)
{
    (void)len;
    g_webhook_count = 0;

    const char* p = json;
    skip_ws(&p);
    if(*p != '[') return false;
    p++; // skip [

    while(*p)
    {
        skip_ws(&p);
        if(*p == ']') { p++; return true; }
        if(*p == ',') { p++; continue; }

        if(g_webhook_count >= MAX_WEBHOOKS) break;
        if(parse_one_webhook(&p, &g_webhooks[g_webhook_count]))
        {
            g_webhook_count++;
        }
        else
        {
            // Skip malformed object
            while(*p && *p != ',' && *p != ']') p++;
            if(*p == ',') p++;
        }
    }
    return true;
}

/// Load webhooks from the JSON file into g_webhooks[].
static void load_webhooks(void)
{
    const char* path = wh_path();
    size_t len = 0;
    char* data = read_file(path, &len);
    if(!data || len == 0)
    {
        g_webhook_count = 0;
        free(data);
        return;
    }

    parse_webhooks(data, len);
    free(data);
}

/// Serialize g_webhooks[] to a JSON string (caller must free).
static char* serialize_webhooks(void)
{
    // Estimate buffer size: each webhook ~256 bytes + overhead
    size_t cap = 256 + (size_t)g_webhook_count * 256;
    char* buf = (char*)malloc(cap);
    if(!buf) return NULL;

    size_t off = 0;
    off += snprintf(buf + off, cap - off, "[");

    for(int i = 0; i < g_webhook_count; i++)
    {
        if(!g_webhooks[i].valid) continue;

        // We need to escape url for JSON (only " and \ require escaping).
        // Build a simple escaped copy on the stack.
        char escaped_url[2048 * 2]; // worst case: every char is escaped
        size_t ej = 0;
        for(size_t si = 0; g_webhooks[i].url[si] && ej < sizeof(escaped_url) - 2; si++)
        {
            if(g_webhooks[i].url[si] == '"' || g_webhooks[i].url[si] == '\\')
                escaped_url[ej++] = '\\';
            escaped_url[ej++] = g_webhooks[i].url[si];
        }
        escaped_url[ej] = '\0';

        if(i > 0) off += snprintf(buf + off, cap - off, ",");
        off += snprintf(buf + off, cap - off,
            "{\"id\":\"%s\",\"url\":\"%s\",\"encoding\":\"%s\",\"format\":\"%s\",\"active\":%s}",
            g_webhooks[i].id,
            escaped_url,
            g_webhooks[i].encoding,
            g_webhooks[i].format,
            g_webhooks[i].active ? "true" : "false");
    }

    off += snprintf(buf + off, cap - off, "]");
    return buf;
}

/// Save g_webhooks[] to the JSON file.
static bool save_webhooks(void)
{
	char* json = serialize_webhooks();
	if(!json) return false;
	bool ok = write_file(wh_path(), json, strlen(json));
	free(json);
	return ok;
}

static void refresh_webhook_manager(void);

static void copy_string(char* dst, size_t dst_size, const char* src)
{
    if(dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int hex_value(char ch)
{
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void url_decode_copy(char* dst, size_t dst_size, const char* src, size_t src_len)
{
    if(dst_size == 0) return;

    size_t out = 0;
    for(size_t i = 0; i < src_len && out + 1 < dst_size; i++)
    {
        if(src[i] == '+' )
        {
            dst[out++] = ' ';
        }
        else if(src[i] == '%' && i + 2 < src_len)
        {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if(hi >= 0 && lo >= 0)
            {
                dst[out++] = static_cast<char>((hi << 4) | lo);
                i += 2;
            }
            else
            {
                dst[out++] = src[i];
            }
        }
        else
        {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool parse_bool_text(const char* s, bool* out)
{
    if(strcmp(s, "true") == 0 || strcmp(s, "1") == 0 || strcmp(s, "on") == 0)
    {
        *out = true;
        return true;
    }
    if(strcmp(s, "false") == 0 || strcmp(s, "0") == 0 || strcmp(s, "off") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

static void normalize_webhook_url(char* url, size_t url_size)
{
    if(url[0] == '\0' || strstr(url, "://")) return;

    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "http://%s", url);
    strncpy(url, tmp, url_size - 1);
    url[url_size - 1] = '\0';
}

static bool parse_webhook_form(const char* body, size_t body_len, struct WhEntry* entry)
{
    const char* p = body;
    const char* end = body + body_len;
    bool found_url = false;

    while(p < end && *p)
    {
        const char* key_start = p;
        while(p < end && *p && *p != '=' && *p != '&') p++;
        const char* key_end = p;

        const char* val_start = p;
        const char* val_end = p;
        if(p < end && *p == '=')
        {
            p++;
            val_start = p;
            while(p < end && *p && *p != '&') p++;
            val_end = p;
        }

        char key[64];
        char val[2048];
        url_decode_copy(key, sizeof(key), key_start, static_cast<size_t>(key_end - key_start));
        url_decode_copy(val, sizeof(val), val_start, static_cast<size_t>(val_end - val_start));

        if((strcmp(key, "urls[]") == 0 || strcmp(key, "url") == 0 || strcmp(key, "urls") == 0) && val[0])
        {
            strncpy(entry->url, val, sizeof(entry->url) - 1);
            entry->url[sizeof(entry->url) - 1] = '\0';
            normalize_webhook_url(entry->url, sizeof(entry->url));
            found_url = true;
        }
        else if(strcmp(key, "encoding") == 0 && val[0])
        {
            strncpy(entry->encoding, val, sizeof(entry->encoding) - 1);
            entry->encoding[sizeof(entry->encoding) - 1] = '\0';
        }
        else if(strcmp(key, "format") == 0 && val[0])
        {
            strncpy(entry->format, val, sizeof(entry->format) - 1);
            entry->format[sizeof(entry->format) - 1] = '\0';
        }
        else if(strcmp(key, "active") == 0)
        {
            parse_bool_text(val, &entry->active);
        }

        if(p < end && *p == '&') p++;
    }

    return found_url;
}

static bool parse_webhook_form_urls(const char* body, size_t body_len,
                                    WhParsedUrl* urls, int* url_count,
                                    bool* saw_urls_key)
{
    const char* p = body;
    const char* end = body + body_len;
    *url_count = 0;
    *saw_urls_key = false;

    while(p < end && *p)
    {
        const char* key_start = p;
        while(p < end && *p && *p != '=' && *p != '&') p++;
        const char* key_end = p;

        const char* val_start = p;
        const char* val_end = p;
        if(p < end && *p == '=')
        {
            p++;
            val_start = p;
            while(p < end && *p && *p != '&') p++;
            val_end = p;
        }

        char key[64];
        char val[2048];
        url_decode_copy(key, sizeof(key), key_start, static_cast<size_t>(key_end - key_start));
        url_decode_copy(val, sizeof(val), val_start, static_cast<size_t>(val_end - val_start));

        if(strcmp(key, "urls[]") == 0 || strcmp(key, "url") == 0 || strcmp(key, "urls") == 0)
        {
            *saw_urls_key = true;
            if(val[0] && *url_count < MAX_WEBHOOKS)
            {
                normalize_webhook_url(val, sizeof(val));

                bool duplicate = false;
                for(int i = 0; i < *url_count; i++)
                {
                    if(strcmp(urls[i].url, val) == 0)
                    {
                        duplicate = true;
                        break;
                    }
                }

                if(!duplicate)
                {
                    copy_string(urls[*url_count].url, sizeof(urls[*url_count].url), val);
                    (*url_count)++;
                }
            }
        }

        if(p < end && *p == '&') p++;
    }

    return *saw_urls_key;
}

// ── Webhook CRUD operations ────────────────────────────────────────────────

/// Find a webhook by ID. Returns index or -1.
static int find_webhook(const char* id)
{
    for(int i = 0; i < g_webhook_count; i++)
    {
        if(g_webhooks[i].valid && strcmp(g_webhooks[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int find_webhook_by_url(const char* url)
{
    for(int i = 0; i < g_webhook_count; i++)
    {
        if(g_webhooks[i].valid && strcmp(g_webhooks[i].url, url) == 0)
            return i;
    }
    return -1;
}

static int next_numeric_id(void)
{
    int next = 1;
    for(int i = 0; i < g_webhook_count; i++)
    {
        if(!g_webhooks[i].valid) continue;
        int id = atoi(g_webhooks[i].id);
        if(id >= next) next = id + 1;
    }
    return next;
}

/// Generate a unique ID (seconds-since-epoch based).
static void gen_id(char* buf, size_t size)
{
    // Use PID + counter for uniqueness within this process lifetime.
    static int s_counter = 0;

    do
    {
        s_counter++;
        snprintf(buf, size, "%d", s_counter);
    }
    while(find_webhook(buf) >= 0);
}

/// Handle GET /api/v2/user/webhooks (list all webhooks).
/// Returns a malloc'd string that the caller must free (never returns a static buffer).
static char* handle_get(size_t* resp_len)
{
    pthread_mutex_lock(&g_wh_lock);
    load_webhooks(); // refresh from file

    char* json = serialize_webhooks();
    pthread_mutex_unlock(&g_wh_lock);

    if(!json)
    {
        json = (char*)malloc(3);
        if(json) { memcpy(json, "[]", 3); *resp_len = 2; }
        else     { *resp_len = 0; }
        return json;
    }
    *resp_len = strlen(json);
    return json;
}

/// Handle GET /api/v2/user/webhooks/ID (single webhook).
/// Returns a malloc'd string that the caller must free (never returns a static buffer).
static char* handle_get_one(const char* id, size_t* resp_len)
{
    pthread_mutex_lock(&g_wh_lock);
    load_webhooks();

    int idx = find_webhook(id);
    char* json = NULL;

    if(idx >= 0)
    {
        // Serialize single webhook
        char buf[4096];
        size_t off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
            "{\"id\":\"%s\",\"url\":\"%s\",\"encoding\":\"%s\",\"format\":\"%s\",\"active\":%s}",
            g_webhooks[idx].id, g_webhooks[idx].url,
            g_webhooks[idx].encoding, g_webhooks[idx].format,
            g_webhooks[idx].active ? "true" : "false");
        json = (char*)malloc(off + 1);
        if(json) { memcpy(json, buf, off + 1); *resp_len = off; }
    }

    pthread_mutex_unlock(&g_wh_lock);

    if(!json)
    {
        json = (char*)malloc(3);
        if(json) { memcpy(json, "{}", 3); *resp_len = 2; }
        else     { *resp_len = 0; }
    }
    return json;
}

/// Handle POST /api/v2/user/webhooks (create webhook from body).
static bool handle_post(const char* body, size_t body_len, char** out_json, size_t* out_len)
{
    WhParsedUrl urls[MAX_WEBHOOKS];
    int url_count = 0;
    bool saw_urls_key = false;
    if(parse_webhook_form_urls(body, body_len, urls, &url_count, &saw_urls_key) && saw_urls_key)
    {
        pthread_mutex_lock(&g_wh_lock);
        load_webhooks();

        struct WhEntry previous[MAX_WEBHOOKS];
        const int previous_count = g_webhook_count;
        memcpy(previous, g_webhooks, sizeof(previous));
        const int first_new_id = next_numeric_id();

        memset(g_webhooks, 0, sizeof(g_webhooks));
        g_webhook_count = 0;
        int next_id = first_new_id;

        for(int i = 0; i < url_count; i++)
        {
            struct WhEntry* entry = &g_webhooks[g_webhook_count];

            int old_idx = -1;
            for(int x = 0; x < previous_count; x++)
            {
                if(previous[x].valid && strcmp(previous[x].url, urls[i].url) == 0)
                {
                    old_idx = x;
                    break;
                }
            }

            if(old_idx >= 0)
            {
                copy_string(entry->id, sizeof(entry->id), previous[old_idx].id);
                copy_string(entry->encoding, sizeof(entry->encoding), previous[old_idx].encoding[0] ? previous[old_idx].encoding : "json");
                copy_string(entry->format, sizeof(entry->format), previous[old_idx].format[0] ? previous[old_idx].format : "basic");
            }
            else
            {
                snprintf(entry->id, sizeof(entry->id), "%d", next_id++);
                copy_string(entry->encoding, sizeof(entry->encoding), "json");
                copy_string(entry->format, sizeof(entry->format), "basic");
            }

            copy_string(entry->url, sizeof(entry->url), urls[i].url);
            entry->active = true;
            entry->valid = true;
            g_webhook_count++;
        }

        save_webhooks();
        char* json = serialize_webhooks();
        pthread_mutex_unlock(&g_wh_lock);

        refresh_webhook_manager();

        if(!json)
        {
            static const char empty[] = "[]";
            *out_json = (char*)empty;
            *out_len = strlen(empty);
            return true;
        }

        *out_json = json;
        *out_len = strlen(json);
        return true;
    }

    // Parse body as partial webhook object
    struct WhEntry tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.active = true; // default

    const char* p = body;
    parse_one_webhook(&p, &tmp);
    if(tmp.url[0] == '\0')
    {
        memset(&tmp, 0, sizeof(tmp));
        tmp.active = true;
        parse_webhook_form(body, body_len, &tmp);
    }

    // Sanity: url is required
    if(tmp.url[0] == '\0')
    {
        static const char err[] = "{\"error\":\"url is required\"}";
        *out_json = (char*)err;
        *out_len = strlen(err);
        return false;
    }

    pthread_mutex_lock(&g_wh_lock);
    load_webhooks();

    int existing_idx = find_webhook_by_url(tmp.url);
    if(existing_idx >= 0)
    {
        struct WhEntry* entry = &g_webhooks[existing_idx];
        if(tmp.encoding[0]) copy_string(entry->encoding, sizeof(entry->encoding), tmp.encoding);
        if(tmp.format[0]) copy_string(entry->format, sizeof(entry->format), tmp.format);
        entry->active = tmp.active;

        save_webhooks();
        pthread_mutex_unlock(&g_wh_lock);

        refresh_webhook_manager();

        char buf[4096];
        size_t off = snprintf(buf, sizeof(buf),
            "{\"id\":\"%s\",\"url\":\"%s\",\"encoding\":\"%s\",\"format\":\"%s\",\"active\":%s}",
            entry->id, entry->url, entry->encoding, entry->format,
            entry->active ? "true" : "false");
        *out_json = (char*)malloc(off + 1);
        if(*out_json) memcpy(*out_json, buf, off + 1);
        *out_len = off;
        return true;
    }

    // Ensure we don't exceed limit
    if(g_webhook_count >= MAX_WEBHOOKS)
    {
        pthread_mutex_unlock(&g_wh_lock);
        static const char err[] = "{\"error\":\"max webhooks reached\"}";
        *out_json = (char*)err;
        *out_len = strlen(err);
        return false;
    }

    // Generate ID and copy fields
    struct WhEntry* entry = &g_webhooks[g_webhook_count];
    gen_id(entry->id, sizeof(entry->id));
    entry->valid = true;
    copy_string(entry->url, sizeof(entry->url), tmp.url);
    copy_string(entry->encoding, sizeof(entry->encoding), tmp.encoding[0] ? tmp.encoding : "json");
    copy_string(entry->format, sizeof(entry->format), tmp.format[0] ? tmp.format : "basic");
    entry->active = tmp.active;
    g_webhook_count++;

    save_webhooks();
    pthread_mutex_unlock(&g_wh_lock);

    refresh_webhook_manager();

    // Build response
    char buf[4096];
    size_t off = snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"url\":\"%s\",\"encoding\":\"%s\",\"format\":\"%s\",\"active\":%s}",
        entry->id, entry->url, entry->encoding, entry->format,
        entry->active ? "true" : "false");
    *out_json = (char*)malloc(off + 1);
    if(*out_json) memcpy(*out_json, buf, off + 1);
    *out_len = off;
    return true;
}

/// Handle PUT /api/v2/user/webhooks/ID (update webhook).
static bool handle_put(const char* id, const char* body, size_t body_len,
                       char** out_json, size_t* out_len)
{
    // Parse body as partial webhook object
    struct WhEntry tmp;
    memset(&tmp, 0, sizeof(tmp));

    const char* p = body;
    parse_one_webhook(&p, &tmp);
    if(tmp.url[0] == '\0')
        parse_webhook_form(body, body_len, &tmp);

    pthread_mutex_lock(&g_wh_lock);
    load_webhooks();

    int idx = find_webhook(id);
    if(idx < 0)
    {
        pthread_mutex_unlock(&g_wh_lock);
        static const char err[] = "{\"error\":\"webhook not found\"}";
        *out_json = (char*)err;
        *out_len = strlen(err);
        return false;
    }

    // Update fields (only non-empty values)
    struct WhEntry* entry = &g_webhooks[idx];
    if(tmp.url[0])       strncpy(entry->url, tmp.url, sizeof(entry->url) - 1);
    if(tmp.encoding[0])  strncpy(entry->encoding, tmp.encoding, sizeof(entry->encoding) - 1);
    if(tmp.format[0])    strncpy(entry->format, tmp.format, sizeof(entry->format) - 1);
    // Always set active (since it's a boolean, it defaults to false)
    entry->active = tmp.active;

    save_webhooks();
    pthread_mutex_unlock(&g_wh_lock);

    refresh_webhook_manager();

    // Build response
    char buf[4096];
    size_t off = snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"url\":\"%s\",\"encoding\":\"%s\",\"format\":\"%s\",\"active\":%s}",
        entry->id, entry->url, entry->encoding, entry->format,
        entry->active ? "true" : "false");
    *out_json = (char*)malloc(off + 1);
    if(*out_json) memcpy(*out_json, buf, off + 1);
    *out_len = off;
    return true;
}

/// Handle DELETE /api/v2/user/webhooks/ID.
static bool handle_delete(const char* id)
{
    pthread_mutex_lock(&g_wh_lock);
    load_webhooks();

    int idx = find_webhook(id);
    if(idx < 0)
    {
        pthread_mutex_unlock(&g_wh_lock);
        return false;
    }

    // Remove by swapping with last
    g_webhooks[idx] = g_webhooks[g_webhook_count - 1];
    memset(&g_webhooks[g_webhook_count - 1], 0, sizeof(struct WhEntry));
    g_webhook_count--;

    save_webhooks();
    pthread_mutex_unlock(&g_wh_lock);

    refresh_webhook_manager();
    return true;
}

// ── HTTP response builder ──────────────────────────────────────────────────

/// Build a complete HTTP response string in a malloc'd buffer.
/// Caller must free().
static char* build_http_response(int status_code, const char* status_text,
                                 const char* content_type,
                                 const char* body, size_t body_len,
                                 size_t* out_len)
{
    // Status line + headers
    char header_buf[512];
    int header_len = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "\r\n",
        status_code, status_text,
        content_type,
        body_len);

    size_t total = (size_t)(header_len > 0 ? header_len : 0) + body_len;
    char* resp = (char*)malloc(total + 1);
    if(!resp) return NULL;

    size_t off = 0;
    if(header_len > 0)
    {
        memcpy(resp, header_buf, (size_t)header_len);
        off += (size_t)header_len;
    }
    if(body_len > 0)
    {
        memcpy(resp + off, body, body_len);
        off += body_len;
    }
    resp[off] = '\0';
    *out_len = off;
    return resp;
}

// ── Request parsing ────────────────────────────────────────────────────────

/// Extract the path component from an HTTP request line.
/// "GET /api/v2/user/webhooks HTTP/1.1" → "/api/v2/user/webhooks"
static void parse_http_path(const char* buf, size_t len, char* path_out, size_t path_size)
{
    path_out[0] = '\0';

    // Find method (skip to first space after verb)
    const char* p = buf;
    while(p < buf + len && *p && *p != ' ') p++;
    if(*p != ' ') return;
    p++; // skip space

    // Read path (up to next space or ?)
    size_t i = 0;
    while(p < buf + len && *p && *p != ' ' && *p != '?' && i < path_size - 1)
    {
        path_out[i++] = *p++;
    }
    path_out[i] = '\0';
}

/// Extract the HTTP method. "POST /path HTTP/1.1" → "POST"
static void parse_http_method(const char* buf, size_t len, char* method_out, size_t method_size)
{
    size_t i = 0;
    while(i < len && buf[i] && buf[i] != ' ' && i < method_size - 1)
    {
        method_out[i] = buf[i];
        i++;
    }
    method_out[i] = '\0';
}

/// PMS reads HTTP requests in two concurrent recvfrom() calls on different
/// threads: 1 byte first (the method's first character), then the rest of
/// the request line (e.g. "ET /api/v2/..." for GET). Since the two threads
/// race on our FD lock, the full-request thread may parse "ET" as the method
/// before the 1-byte thread stores its preamble.
///
/// This function checks whether the extracted method could be the *tail* of
/// a known HTTP method (i.e. a method split by the 1-byte probe). If so, it
/// prepends the missing character to reconstruct the real method.
static void fix_split_method(char* method)
{
    // Sorted longest-first so "OPTIONS" matches before "POST/PUT/PATCH",
    // and "DELETE" before "GET".
    static const char* known[] = {
        "CONNECT", "OPTIONS", "DELETE", "PATCH",
        "POST", "PUT", "HEAD", "GET", "TRACE"
    };
    size_t mlen = strlen(method);
    if(mlen == 0) return;

    for(int i = 0; i < 9; i++)
    {
        const char* km = known[i];
        size_t klen = strlen(km);
        // The buffer starts with the method tail — the 1-byte probe consumed
        // the first character.  E.g. "ET" (mlen=2) + 1 = "GET" (klen=3).
        if(klen == mlen + 1 && strcmp(km + 1, method) == 0)
        {
            memmove(method + 1, method, mlen + 1); // shift right by 1
            method[0] = km[0];                      // prepend missing char
            return;
        }
    }
}

/// Find Content-Length header value. Returns 0 if not found.
static size_t parse_content_length(const char* buf, size_t len)
{
    const char* p = buf;
    const char* end = buf + len;

    while(p < end)
    {
        // Look for "Content-Length:" or "content-length:"
        const char* cl = NULL;
        if(end - p >= 16)
        {
            if((p[0]=='C'||p[0]=='c') && (p[1]=='o'||p[1]=='O') &&
               (p[2]=='n'||p[2]=='N') && (p[3]=='t'||p[3]=='T') &&
               (p[4]=='e'||p[4]=='E') && (p[5]=='n'||p[5]=='N') &&
               (p[6]=='t'||p[6]=='T') &&
               p[7]=='-' &&
               (p[8]=='L'||p[8]=='l') &&
               (p[9]=='e'||p[9]=='E') &&
               (p[10]=='n'||p[10]=='N') &&
               (p[11]=='g'||p[11]=='G') &&
               (p[12]=='t'||p[12]=='T') &&
               (p[13]=='h'||p[13]=='H') &&
               p[14]==':')
            {
                cl = p + 15;
            }
        }
        if(!cl)
        {
            // Skip to next line
            while(p < end && *p != '\n') p++;
            if(p < end) p++;
            continue;
        }

        // Skip whitespace
        while(cl < end && *cl == ' ') cl++;
        size_t val = 0;
        while(cl < end && *cl >= '0' && *cl <= '9')
        {
            val = val * 10 + (size_t)(*cl - '0');
            cl++;
        }
        return val;
    }
    return 0;
}

/// Find the double CRLF that separates headers from body.
/// Returns pointer to start of body, or NULL if not found.
static const char* find_body_start(const char* buf, size_t len)
{
    for(size_t i = 0; i + 3 < len; i++)
    {
        if(buf[i] == '\r' && buf[i+1] == '\n' &&
           buf[i+2] == '\r' && buf[i+3] == '\n')
        {
            return buf + i + 4;
        }
    }
    return NULL;
}

/// Parse an incoming HTTP request buffer and fill req_out.
/// Returns true if it looks like a valid HTTP request.
static bool parse_request(const char* buf, size_t len, struct WhRequest* req_out)
{
    memset(req_out, 0, sizeof(*req_out));

    parse_http_method(buf, len, req_out->method, sizeof(req_out->method));
    if(req_out->method[0] == '\0') return false;

    parse_http_path(buf, len, req_out->path, sizeof(req_out->path));
    if(req_out->path[0] == '\0') return false;

    // Check if it's a webhook path
    if(strstr(req_out->path, "/api/v2/user/webhooks") == NULL)
        return false;

    // Fix split method — PMS reads 1 byte on one thread and the rest on
    // another, so the method may be missing its first character ("ET"→"GET").
    fix_split_method(req_out->method);

    // Extract body for POST/PUT
    size_t cl = parse_content_length(buf, len);
    if(cl > 0)
    {
        const char* body_start = find_body_start(buf, len);
        if(body_start)
        {
            size_t remaining = len - (size_t)(body_start - buf);
            size_t to_copy = cl < remaining ? cl : remaining;
            if(to_copy > MAX_BODY - 1) to_copy = MAX_BODY - 1;
            memcpy(req_out->body, body_start, to_copy);
            req_out->body[to_copy] = '\0';
            req_out->body_len = to_copy;
            req_out->has_body = to_copy > 0;
        }
    }

    return true;
}

/// Extract the trailing ID from a path like /api/v2/user/webhooks/123.
/// Returns pointer to ID string, or NULL if no ID segment.
static const char* extract_webhook_id(const char* path)
{
    // NOTE: strlen("/api/v2/user/webhooks") == 21, NOT 22.
    static const size_t PREFIX_LEN = 21;
    const char* p = strstr(path, "/api/v2/user/webhooks");
    if(!p) return NULL;
    p += PREFIX_LEN;
    if(*p == '/')
    {
        p++;
        return (*p) ? p : NULL;
    }
    return NULL;
}

// ── Common read-data processing (used by read/recv/recvfrom hooks) ─────────

/// Returns true if ch is the first character of a known HTTP method.
/// Handles the 1-byte probe that PMS reads before the full HTTP request line.
static inline bool is_http_method_start(char ch)
{
    return ch == 'G' || ch == 'P' || ch == 'D' || ch == 'O'
        || ch == 'H' || ch == 'T' || ch == 'C';
}

/// Process incoming data on an FD: try to detect a webhook request, handling
/// the PMS 1-byte probe by buffering preambles between recvfrom() calls.
/// Must be called under g_fd_lock.
static void on_fd_data(int fd, const char* data, size_t len)
{
    // If this FD was previously handled (keep-alive), reset for next request.
    if(g_fds[fd].state == WH_FD_HANDLED)
        g_fds[fd].state = WH_FD_NONE;

    if(g_fds[fd].state != WH_FD_NONE)
        return;

    struct WhRequest req;
    bool matched = false;

    if(g_fds[fd].preamble_len > 0)
    {
        // Earlier read left a 1-byte preamble (e.g. "G" from the PMS probe).
        // Combine with current data and try to parse the full request line.
        char combined[8192];
        size_t combined_len = g_fds[fd].preamble_len + len;
        if(combined_len <= sizeof(combined))
        {
            memcpy(combined, g_fds[fd].preamble, g_fds[fd].preamble_len);
            memcpy(combined + g_fds[fd].preamble_len, data, len);
            matched = parse_request(combined, combined_len, &req);
        }
        // Always clear the preamble — it is either consumed or stale.
        g_fds[fd].preamble_len = 0;
    }
    else
    {
        matched = parse_request(data, len, &req);
    }

    if(matched)
    {
        g_fds[fd].state = WH_FD_DETECTED;
        g_fds[fd].req = req;
    }
    else if(len == 1 && is_http_method_start(data[0]))
    {
        // Single byte that looks like the start of an HTTP method — PMS is
        // probing the socket. Buffer it so the next recvfrom combines into
        // the full request line (e.g. "G" + "ET /api/v2/..." -> "GET /...").
        g_fds[fd].preamble[0] = data[0];
        g_fds[fd].preamble_len = 1;
    }
}

/// Common error/close handling for read/recv/recvfrom hooks.
static void on_fd_close_or_error(int fd)
{
    g_fds[fd].state = WH_FD_NONE;
    g_fds[fd].preamble_len = 0;
}

// ── Hook functions ─────────────────────────────────────────────────────────

extern "C" ssize_t read(int fd, void* buf, size_t count)
{
    if(!real_read) return syscall(SYS_read, fd, buf, count);
    ssize_t ret = real_read(fd, buf, count);

    if(!g_handler_ready) return ret;

    if(ret > 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_data(fd, (const char*)buf, (size_t)ret);
        pthread_mutex_unlock(&g_fd_lock);
    }
    else if(ret <= 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_close_or_error(fd);
        pthread_mutex_unlock(&g_fd_lock);
    }

    return ret;
}

extern "C" ssize_t recv(int fd, void* buf, size_t len, int flags)
{
    if(!real_recv) return syscall(SYS_recvfrom, fd, buf, len, flags, nullptr, nullptr);
    ssize_t ret = real_recv(fd, buf, len, flags);

    if(!g_handler_ready) return ret;

    if(ret > 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_data(fd, (const char*)buf, (size_t)ret);
        pthread_mutex_unlock(&g_fd_lock);
    }
    else if(ret <= 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_close_or_error(fd);
        pthread_mutex_unlock(&g_fd_lock);
    }

    return ret;
}

extern "C" ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                            struct sockaddr* src_addr, socklen_t* addrlen)
{
    if(!real_recvfrom) return syscall(SYS_recvfrom, fd, buf, len, flags, src_addr, addrlen);
    ssize_t ret = real_recvfrom(fd, buf, len, flags, src_addr, addrlen);

    if(!g_handler_ready) return ret;

    if(ret > 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_data(fd, (const char*)buf, (size_t)ret);
        pthread_mutex_unlock(&g_fd_lock);
    }
    else if(ret <= 0 && fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        on_fd_close_or_error(fd);
        pthread_mutex_unlock(&g_fd_lock);
    }

    return ret;
}

/// Forward declaration for serve_webhook_response (called from sendmsg hook).
static ssize_t serve_webhook_response(int fd, ssize_t requested_len);

/// Hooked sendmsg() intercepts PMS HTTP responses. PMS uses sendmsg() to write
/// HTTP responses to client sockets. When a FD is in WH_FD_DETECTED state,
/// sendmsg() serves our webhook JSON response instead of the original payload.
/// Returns the length to return to the caller (the original requested size).
static ssize_t serve_webhook_response(int fd, ssize_t requested_len)
{
    struct WhRequest* req = &g_fds[fd].req;
    const char* id = extract_webhook_id(req->path);

    char* json = NULL;
    size_t json_len = 0;
    int status = 200;
    const char* status_text = "OK";
    bool should_free = false;

    if(strcmp(req->method, "GET") == 0)
    {
        if(id) { json = handle_get_one(id, &json_len); should_free = true; }
        else   { json = handle_get(&json_len); should_free = true; }
    }
    else if(strcmp(req->method, "POST") == 0)
    {
        status = 201; status_text = "Created";
        char* rj = NULL; size_t rl = 0;
        if(handle_post(req->body, req->body_len, &rj, &rl))
            { json = rj; json_len = rl; should_free = true; }
        else
            { json = rj ? rj : (char*)"{}"; json_len = rl ? rl : 2; }
    }
    else if(strcmp(req->method, "PUT") == 0)
    {
        char* rj = NULL; size_t rl = 0;
        if(id && handle_put(id, req->body, req->body_len, &rj, &rl))
            { json = rj; json_len = rl; should_free = true; }
        else
            { status = 404; status_text = "Not Found"; json = (char*)"{}"; json_len = 2; }
    }
    else if(strcmp(req->method, "DELETE") == 0)
    {
        if(id && handle_delete(id))
            { json = (char*)"{}"; json_len = 2; }
        else
            { status = 404; status_text = "Not Found"; json = (char*)"{}"; json_len = 2; }
    }
    else if(strcmp(req->method, "OPTIONS") == 0)
    {
        json = (char*)""; json_len = 0;
    }
    else
    {
        status = 405; status_text = "Method Not Allowed";
        json = (char*)"{}"; json_len = 2;
    }

    size_t resp_total = 0;
    char* http_resp = build_http_response(status, status_text,
                                           "application/json",
                                           json, json_len, &resp_total);
    if(should_free && json) free(json);

    if(http_resp)
    {
        write(fd, http_resp, resp_total);
        free(http_resp);
    }

    return (ssize_t)requested_len;
}

extern "C" ssize_t sendmsg(int fd, const struct msghdr* msg, int flags)
{
    if(!real_sendmsg) return syscall(SYS_sendmsg, fd, msg, flags);

    if(!g_handler_ready) return real_sendmsg(fd, msg, flags);

    if(fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);

        if(g_fds[fd].state == WH_FD_DETECTED)
        {
            g_fds[fd].state = WH_FD_HANDLED;
            ssize_t total = 0;
            if(msg) for(int i = 0; i < (int)msg->msg_iovlen; i++)
                total += (ssize_t)msg->msg_iov[i].iov_len;

            ssize_t ret = serve_webhook_response(fd, total);
            pthread_mutex_unlock(&g_fd_lock);
            return ret;
        }

        if(g_fds[fd].state == WH_FD_HANDLED)
        {
            pthread_mutex_unlock(&g_fd_lock);
            ssize_t total = 0;
            if(msg) for(int i = 0; i < (int)msg->msg_iovlen; i++)
                total += (ssize_t)msg->msg_iov[i].iov_len;
            return total;
        }

        pthread_mutex_unlock(&g_fd_lock);
    }

    return real_sendmsg(fd, msg, flags);
}

extern "C" int close(int fd)
{
    if(!real_close) return syscall(SYS_close, fd);

    if(!g_handler_ready) return real_close(fd);

    if(fd >= 0 && fd < MAX_FDS)
    {
        pthread_mutex_lock(&g_fd_lock);
        g_fds[fd].state = WH_FD_NONE;
        g_fds[fd].preamble_len = 0;
        memset(&g_fds[fd].req, 0, sizeof(g_fds[fd].req));
        pthread_mutex_unlock(&g_fd_lock);
    }

    return real_close(fd);
}

// ── WebhookManager injection ──────────────────────────────────────────────
//
// Hooks sub_125ACA6 (WebhookManager::init or similar). After the original
// runs (fetching webhooks from plex.tv — likely empty for non-Pass users),
// injects our webhooks from webhooks.json directly into the Manager's
// internal per-user vector.
//
// The WebhookManager layout (from IDA of sub_125C156 / sub_125E524):
//   +0x04: pthread_mutex_t
//   +0x78: std::map<int, PerUserNode>   (container start)
//   +0x80: tree root pointer
//
// PerUserNode (map value type, sizeof=0x40):
//   +0x00: __tree_node_base (parent, left, right, color) — 32 bytes
//   +0x20 (32): user ID (int) — 4 bytes
//   +0x24: padding — 4 bytes
//   +0x28 (40): vector begin (char**) — 8 bytes
//   +0x30 (48): vector end (char**) — 8 bytes
//   +0x38 (56): vector capacity (char**) — 8 bytes
//
// Each vector entry is a libc++ std::string (24 bytes).
//   Short string (<=22 chars): bytes 0-22 inline, byte 23 = 22 - length
//   Long string  (>=23 chars): bytes 0-7 = heap ptr, 8-15 = size,
//                               16-23 = capacity|0x80_MSB

/// Manually construct a libc++ std::string (24 bytes) in an output buffer.
/// Avoids including C++ <string> header and keeps build deps minimal.
static void construct_stdstring_24(void* slot, const char* s)
{
    size_t len = strlen(s);
    auto* buf = static_cast<unsigned char*>(slot);

    if(len <= 22)
    {
        // Short string: inline data
        memcpy(buf, s, len);
        buf[len] = '\0';
        buf[23] = static_cast<unsigned char>(22 - len);
    }
    else
    {
        // Long string: heap-allocated data
        char* heap = static_cast<char*>(malloc(len + 1));
        memcpy(heap, s, len + 1);

        memcpy(buf,      &heap, 8);  // __long.__data_
        memcpy(buf + 8,  &len,  8);  // __long.__size_
        size_t cap = len | (1ULL << 63); // __long.__cap_ + MSB long marker
        memcpy(buf + 16, &cap,  8);  // __long.__cap_
    }
}

/// Resolved at startup via sig_scan.
/// void* sub_125E524(void* map, int* key, int** key_ptr_ptr)
///   Finds or creates a per-user node in the WebhookManager's std::map.
static void* (*_webhook_map_find)(void*, int*, int**) = nullptr;
static void* g_webhook_manager = nullptr;

void webhook_set_sub_125E524(void* addr)
{
	_webhook_map_find = reinterpret_cast<decltype(_webhook_map_find)>(addr);
}

void webhook_set_manager(void* manager)
{
    g_webhook_manager = manager;
}

void webhook_inject_into_manager(void* manager)
{
    if(!manager || !_webhook_map_find) return;

    // Re-read webhooks from JSON file
    pthread_mutex_lock(&g_wh_lock);
    load_webhooks();

    // Count active local webhooks
    size_t new_count = 0;
    for(int i = 0; i < g_webhook_count; i++)
        if(g_webhooks[i].valid && g_webhooks[i].active) new_count++;

    // The map container is at manager + 0x78.
    void* map = static_cast<char*>(manager) + 0x78;
    int user_id = 1;  // local admin
    int* p_user_id = &user_id;
    auto* node = static_cast<char*>((*_webhook_map_find)(map, &user_id, &p_user_id));
    if(!node)
    {
        pthread_mutex_unlock(&g_wh_lock);
        return;
    }

    // ← At this point the g_webhooks array is valid so we hold the lock.
    // Vector pointers at node + 40, +48, +56.
    auto** v_begin = reinterpret_cast<char**>(node + 40);
    auto** v_end   = reinterpret_cast<char**>(node + 48);
    auto** v_cap   = reinterpret_cast<char**>(node + 56);

    size_t existing = *v_begin ? (size_t)(*v_end - *v_begin) / 24 : 0;
    if(*v_begin)
    {
        // Drop the previous in-memory dispatch list.  We zero entries before
        // raw deallocation to avoid stale std::string ownership if PMS later
        // observes the old buffer during shutdown.
        memset(*v_begin, 0, existing * 24);
        operator delete(*v_begin);
    }

    if(new_count == 0)
    {
        *v_begin = nullptr;
        *v_end = nullptr;
        *v_cap = nullptr;
        pthread_mutex_unlock(&g_wh_lock);
        return;
    }

    // Allocate a new contiguous buffer for the expanded vector.
    // Using musl operator new (same allocator PMS uses).
    auto* new_buf = static_cast<char*>(operator new(24 * new_count));

    // Construct new std::string entries for each valid webhook.
    size_t idx = 0;
    for(int i = 0; i < g_webhook_count; i++)
    {
        if(!g_webhooks[i].valid || !g_webhooks[i].active) continue;
        construct_stdstring_24(new_buf + 24 * idx, g_webhooks[i].url);
        idx++;
    }

    pthread_mutex_unlock(&g_wh_lock);

    // Update vector bookkeeping.
    *v_begin = static_cast<char*>(new_buf);
    *v_end   = static_cast<char*>(new_buf + 24 * new_count);
    *v_cap   = static_cast<char*>(new_buf + 24 * new_count);
}

static void refresh_webhook_manager(void)
{
    if(g_webhook_manager && _webhook_map_find)
        webhook_inject_into_manager(g_webhook_manager);
}

// ── Initialisation ─────────────────────────────────────────────────────────

void webhook_handler_init(void)
{
    if(g_handler_ready) return;

    // Resolve real function pointers via dlsym(RTLD_NEXT, ...).
    // RTLD_NEXT finds the "next" definition of the symbol — i.e. the libc
    // implementation — so our LD_PRELOAD hooks can call through to the real
    // implementation after interception.
    bool ok = true;

#define RESOLVE(name_, var_) do { \
    var_ = reinterpret_cast<decltype(var_)>(dlsym(RTLD_NEXT, name_)); \
    if(!var_) { ok = false; } \
} while(0)

    RESOLVE("read",     real_read);
    RESOLVE("recv",     real_recv);
    RESOLVE("recvfrom", real_recvfrom);
    // NOT hooking write()/send() — PMS uses sendmsg() for HTTP output.
    // write() hooks caused SEGV during shutdown (false intercepts on reused FDs).
    RESOLVE("sendmsg",  real_sendmsg);
    RESOLVE("close",    real_close);

#undef RESOLVE

    if(!ok)
    {
        // If any dlsym failed, crash early rather than silently misbehave.
        // This is a fatal error because we can't safely operate without the
        // real libc socket functions.
        static const char msg[] = "plexmediaserver_crack: webhook_handler_init: "
                                  "dlsym(RTLD_NEXT, ...) failed — aborting\n";
        write(STDERR_FILENO, msg, strlen(msg));
        _exit(1);
    }

    // Zero out FD tracking table
    memset(g_fds, 0, sizeof(g_fds));

    g_handler_ready = true;

    // Preload initial webhooks from file (just to validate the path works).
    // Not critical — the file is read on every GET request anyway.
    const char* path = wh_path();
    FILE* test = fopen(path, "r");
    if(test)
    {
        fclose(test);
    }
    else
    {
        // File doesn't exist yet — create an empty array
        write_file(path, "[]", 2);
    }
}
