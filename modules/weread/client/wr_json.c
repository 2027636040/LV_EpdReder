#include "wr_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void *wr_alloc(size_t n)
{
    return epd_app_alloc(n ? n : 1, EPD_APP_PSRAM);
}

char *wr_dup_n(const char *s, size_t n)
{
    char *p;
    if (!s || n == SIZE_MAX) return NULL;
    p = wr_alloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

char *wr_dup(const char *s) { return s ? wr_dup_n(s, strlen(s)) : NULL; }

char *wr_format(const char *fmt, ...)
{
    va_list ap, copy;
    int n;
    char *s;
    va_start(ap, fmt);
    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) { va_end(ap); return NULL; }
    s = wr_alloc((size_t)n + 1);
    if (s && vsnprintf(s, (size_t)n + 1, fmt, ap) != n)
    { epd_app_free(s); s = NULL; }
    va_end(ap);
    return s;
}

bool wr_ascii_equal(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *b)
    {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}

char *wr_url_encode(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n, used = 0;
    char *out;
    if (!s || (n = strlen(s)) > (SIZE_MAX - 1) / 3) return NULL;
    out = wr_alloc(n * 3 + 1);
    if (!out) return NULL;
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[used++] = (char)c;
        else { out[used++] = '%'; out[used++] = hex[c >> 4]; out[used++] = hex[c & 15]; }
    }
    out[used] = 0;
    return out;
}

static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
wr_result_t wr_json_parse(const char *text, size_t size, wr_json **out)
{
    const char *end = NULL;
    *out = NULL;
    if (!text || !size || size == SIZE_MAX) return WR_ERR_ARGUMENT;
    *out = wr_cjson_ParseWithLengthOpts(text, size, &end, 0);
    if (!*out) return WR_ERR_PROTOCOL;
    while (end < text + size && (*end == ' ' || *end == '\n' ||
           *end == '\r' || *end == '\t')) ++end;
    if (end != text + size)
    { j_delete(*out); *out = NULL; return WR_ERR_PROTOCOL; }
    return WR_OK;
}
wr_result_t wr_json_print(const wr_json *root, char **out, size_t *size)
{
    *out = NULL; *size = 0;
    if (!root) return WR_ERR_MEMORY;
    *out = wr_cjson_PrintUnformatted(root);
    if (!*out) return WR_ERR_MEMORY;
    *size = strlen(*out);
    return WR_OK;
}
wr_result_t wr_json_int(const wr_json *v, int64_t *out)
{
    if (j_is_number(v))
    {
        double d = v->valuedouble;
        if (!(d >= -9007199254740991.0 && d <= 9007199254740991.0)) return WR_ERR_PROTOCOL;
        *out = (int64_t)d;
        return (double)*out == d ? WR_OK : WR_ERR_PROTOCOL;
    }
    if (j_is_string(v) && v->valuestring[0])
    {
        const char *p = v->valuestring;
        bool negative = *p == '-';
        uint64_t magnitude = 0;
        uint64_t limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
        if (negative) ++p;
        if (!*p) return WR_ERR_PROTOCOL;
        while (*p)
        {
            unsigned d;
            if (!digit((unsigned char)*p)) return WR_ERR_PROTOCOL;
            d = (unsigned)(*p++ - '0');
            if (magnitude > (limit-d)/10) return WR_ERR_PROTOCOL;
            magnitude = magnitude*10 + d;
        }
        if (negative && magnitude == (uint64_t)INT64_MAX+1u) *out = INT64_MIN;
        else *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
        return WR_OK;
    }
    return WR_ERR_PROTOCOL;
}
wr_result_t wr_json_text(const wr_json *v, bool required, char **out)
{
    *out = NULL;
    if (j_is_string(v)) *out = wr_dup(v->valuestring);
    else if (j_is_number(v))
    {
        int64_t n;
        if (wr_json_int(v, &n) != WR_OK) return WR_ERR_PROTOCOL;
        *out = wr_format("%lld", (long long)n);
    }
    else if (!v && !required) *out = wr_dup("");
    else return WR_ERR_PROTOCOL;
    if (!*out) return WR_ERR_MEMORY;
    if (required && !**out) { epd_app_free(*out); *out = NULL; return WR_ERR_PROTOCOL; }
    return WR_OK;
}
bool wr_json_true(const wr_json *v)
{
    return wr_cjson_IsTrue(v) || (j_is_number(v) && v->valuedouble == 1) ||
        (j_is_string(v) && (!strcmp(v->valuestring, "1") || wr_ascii_equal(v->valuestring, "true")));
}
const wr_json *wr_payload(const wr_json *root)
{
    const wr_json *data = j_get(root, "data");
    return j_is_object(data) ? data : root;
}
wr_result_t wr_remote_result(const wr_json *root)
{
    static const char *const keys[] = { "errCode", "errcode", "errorCode", "error", "logicCode" };
    size_t i;
    if (!j_is_object(root)) return WR_OK;
    {
        const wr_json *code = j_get(root,"code");
        int64_t value;
        if (!j_get(root,"vid") && !j_get(root,"skey") && code &&
            wr_json_int(code,&value) == WR_OK && value < 0)
            return value == -2012 ? WR_ERR_AUTH : WR_ERR_REMOTE;
    }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    {
        const wr_json *v = j_get(root, keys[i]);
        if (!v || wr_cjson_IsNull(v) || wr_cjson_IsFalse(v)) continue;
        if (j_is_number(v))
        {
            if (v->valuedouble == 0) continue;
            if (v->valuedouble == -2012 || v->valuedouble == 401) return WR_ERR_AUTH;
            if (v->valuedouble == 403) return WR_ERR_PERMISSION;
        }
        if (j_is_string(v))
        {
            const char *s = v->valuestring;
            if (!*s || !strcmp(s, "0") || wr_ascii_equal(s, "OK")) continue;
            if (!strcmp(s, "-2012") || wr_ascii_equal(s, "LOGIN_EXPIRED")) return WR_ERR_AUTH;
            if (wr_ascii_equal(s, "EXPIRED") || wr_ascii_equal(s, "TIMEOUT") ||
                wr_ascii_equal(s, "QR_EXPIRED")) return WR_EXPIRED;
            if (wr_ascii_equal(s, "PERMISSION_DENIED") || wr_ascii_equal(s, "NO_PERMISSION") ||
                wr_ascii_equal(s, "PAY_REQUIRED")) return WR_ERR_PERMISSION;
        }
        return WR_ERR_REMOTE;
    }
    {
        const wr_json *data = j_get(root, "data");
        if (j_is_object(data)) return wr_remote_result(data);
    }
    return WR_OK;
}
