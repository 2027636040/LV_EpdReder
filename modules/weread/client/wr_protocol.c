/* Content ID/signature/shuffle adapted from epdiy-epub src/weread.
 * See ../LICENSE.reference. Crypto primitives use private SDK mbedTLS copies. */
#include "wr_internal.h"
#include "private/wr_md5.h"
#include "private/wr_base64.h"
#include <stdio.h>
#include <stdlib.h>

void wr_md5_hex(const void *data, size_t size, char out[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[16];
    size_t i;
    wr_mbedtls_md5_context ctx;
    const unsigned char *p = data;
    wr_mbedtls_md5_init(&ctx); wr_mbedtls_md5_starts(&ctx);
    while (size)
    {
        size_t take = size > UINT32_MAX ? UINT32_MAX : size;
        wr_mbedtls_md5_update(&ctx,p,take); p += take; size -= take;
    }
    wr_mbedtls_md5_finish(&ctx,digest); wr_mbedtls_md5_free(&ctx);
    for (i = 0; i < 16; ++i)
    { out[2*i] = hex[digest[i] >> 4]; out[2*i+1] = hex[digest[i] & 15]; }
    out[32] = 0;
}
wr_result_t wr_encode_id(const char *s, char **out)
{
    char digest[33], tail[33], *p;
    size_t n, i, used = 7;
    bool numeric = true;
    *out = NULL;
    if (!s || !(n = strlen(s))) return WR_ERR_ARGUMENT;
    if (n > (SIZE_MAX - 100) / 3) return WR_ERR_MEMORY;
    wr_md5_hex(s, n, digest);
    for (i = 0; i < n; ++i) if (s[i] < '0' || s[i] > '9') numeric = false;
    p = wr_alloc(n * 3 + 100);
    if (!p) return WR_ERR_MEMORY;
    memcpy(p, digest, 3); memcpy(p+3, numeric ? "32" : "42", 2); memcpy(p+5, digest+30, 2);
    if (numeric)
    {
        for (i = 0; i < n;)
        {
            size_t take = n - i, j;
            unsigned value = 0;
            char hex[16];
            int length;
            if (take > 9) take = 9;
            for (j = 0; j < take; ++j) value = value * 10 + (unsigned)(s[i+j] - '0');
            length = snprintf(hex, sizeof(hex), "%x", value);
            used += (size_t)sprintf(p + used, "%02x%s", (unsigned)length, hex);
            i += take;
            if (i < n) p[used++] = 'g';
        }
    }
    else
    {
        used += (size_t)sprintf(p + used, "%02zx", n * 2);
        for (i = 0; i < n; ++i) used += (size_t)sprintf(p + used, "%02x", (unsigned char)s[i]);
    }
    if (used < 20) { memcpy(p + used, digest, 20 - used); used = 20; }
    wr_md5_hex(p, used, tail); memcpy(p + used, tail, 3); p[used+3] = 0;
    *out = p;
    return WR_OK;
}
uint32_t wr_sign_query(const char *query)
{
    uint32_t a = 0x15051505u, b = a;
    size_t n = strlen(query), i = n;
    while (i > 1)
    {
        a = (a ^ ((uint32_t)(unsigned char)query[i-1] << ((n-i+1)%30))) & 0x7fffffffu;
        b = (b ^ ((uint32_t)(unsigned char)query[i-2] << ((i-1)%30))) & 0x7fffffffu;
        i -= 2;
    }
    return a + b;
}
wr_result_t wr_content_request(const char *book, const char *chapter,
                                uint32_t now, uint32_t nonce, char **body)
{
    char *b = NULL, *c = NULL, *ps = NULL, *pc = NULL, *query = NULL;
    char stamp[16], r[16], ct[16], sign[16];
    wr_json *j = NULL;
    wr_result_t result;
    size_t size;
    *body = NULL;
    result = wr_encode_id(book, &b); if (result != WR_OK) goto done;
    result = wr_encode_id(chapter, &c); if (result != WR_OK) goto done;
    snprintf(stamp, sizeof(stamp), "%u", (unsigned)(now > 2 ? now-2 : 0));
    result = wr_encode_id(stamp, &ps); if (result != WR_OK) goto done;
    snprintf(stamp, sizeof(stamp), "%u", (unsigned)(now > 1 ? now-1 : 0));
    result = wr_encode_id(stamp, &pc); if (result != WR_OK) goto done;
    nonce %= 10000;
    snprintf(r, sizeof(r), "%u", (unsigned)(nonce * nonce));
    snprintf(ct, sizeof(ct), "%u", (unsigned)now);
    result = WR_ERR_MEMORY;
    query = wr_format("b=%s&c=%s&ct=%s&pc=%s&prevChapter=false&ps=%s&r=%s&sc=0&st=0", b,c,ct,pc,ps,r);
    if (!query) goto done;
    snprintf(sign, sizeof(sign), "%x", (unsigned)wr_sign_query(query));
    j = j_object();
    if (!j || !j_string(j,"b",b) || !j_string(j,"c",c) || !j_string(j,"r",r) ||
        !j_string(j,"ct",ct) || !j_string(j,"ps",ps) || !j_string(j,"pc",pc) ||
        !j_number(j,"sc",0) || !j_bool(j,"prevChapter",false) || !j_number(j,"st",0) ||
        !j_string(j,"s",sign)) goto done;
    result = wr_json_print(j, body, &size);
done:
    epd_app_free(b); epd_app_free(c); epd_app_free(ps); epd_app_free(pc); epd_app_free(query);
    j_delete(j); return result;
}
wr_result_t wr_fragment_check(const char *data, size_t size, const char **out, size_t *n)
{
    char digest[33], prefix[33];
    *out = NULL; *n = 0;
    if (!data || size <= 32) return WR_ERR_INTEGRITY;
    wr_md5_hex(data + 32, size - 32, digest);
    memcpy(prefix, data, 32); prefix[32] = 0;
    if (!wr_ascii_equal(prefix, digest)) return WR_ERR_INTEGRITY;
    *out = data + 32; *n = size - 32;
    return WR_OK;
}
static size_t swap_positions(const unsigned char *data, size_t n, size_t positions[10])
{
    char digits[40], mtext[32];
    size_t tail, i, used = 0, m, step, count = 0;
    if (n < 4) return 0;
    if (n < 11) { positions[0] = 0; positions[1] = 2; return 2; }
    tail = n / 10 + (n % 10 != 0);
    if (tail > 4) tail = 4;
    for (i = 0; i < tail; ++i)
    {
        unsigned v = 0, bit;
        for (bit = 8; bit > 0; --bit) v = v * 4 + ((data[n-1-i] >> (bit-1)) & 1);
        used += (size_t)sprintf(digits + used, "%u", v);
    }
    m = n - tail - 2;
    step = (size_t)snprintf(mtext, sizeof(mtext), "%zu", m);
    for (i = 0; count < 10 && i + step < used; i += step)
    {
        size_t v = 0, w = 0, j;
        for (j = 0; j < step; ++j)
        { v = v * 10 + (unsigned)(digits[i+j]-'0'); w = w * 10 + (unsigned)(digits[i+j+1]-'0'); }
        positions[count++] = v % m; positions[count++] = w % m;
    }
    return count;
}
static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
wr_result_t wr_content_decode(char *joined, size_t size, char **out, size_t *out_size)
{
    unsigned char *data;
    size_t n, positions[10], count, i, padding = 0, used = 0;
    char *decoded;
    unsigned char *normalized;
    size_t normalized_size;
    *out = NULL; *out_size = 0;
    if (!joined || size < 2) return WR_ERR_INTEGRITY;
    data = (unsigned char *)joined + 1; n = size - 1;
    count = swap_positions(data, n, positions);
    while (count >= 2)
    {
        size_t a = positions[count-2], b = positions[count-1];
        int offset;
        for (offset = 1; offset >= 0; --offset)
        { unsigned char t = data[a+(size_t)offset]; data[a+(size_t)offset] = data[b+(size_t)offset]; data[b+(size_t)offset] = t; }
        count -= 2;
    }
    while (padding < n && data[n-1-padding] == '=') ++padding;
    if (padding > 2 || (padding && n%4) || (n-padding)%4 == 1 ||
        (padding == 1 && (n-padding)%4 != 3) || (padding == 2 && (n-padding)%4 != 2))
        return WR_ERR_INTEGRITY;
    if (n > SIZE_MAX - 4) return WR_ERR_MEMORY;
    normalized = wr_alloc(n + 4);
    if (!normalized) return WR_ERR_MEMORY;
    normalized_size = n;
    memcpy(normalized,data,n);
    for (i = 0; i < n-padding; ++i)
    {
        int v = b64_value(data[i]);
        if (v < 0) { epd_app_free(normalized); return WR_ERR_INTEGRITY; }
        if (data[i] == '-') normalized[i] = '+';
        if (data[i] == '_') normalized[i] = '/';
    }
    i = (n-padding)%4;
    if ((i == 2 && (b64_value(data[n-padding-1]) & 15)) ||
        (i == 3 && (b64_value(data[n-padding-1]) & 3)))
    { epd_app_free(normalized); return WR_ERR_INTEGRITY; }
    while (normalized_size%4) normalized[normalized_size++] = '=';
    decoded = wr_alloc(n + 1);
    if (!decoded) { epd_app_free(normalized); return WR_ERR_MEMORY; }
    if (wr_mbedtls_base64_decode((unsigned char *)decoded,n+1,&used,normalized,normalized_size))
    { epd_app_free(normalized); epd_app_free(decoded); return WR_ERR_INTEGRITY; }
    epd_app_free(normalized);
    if (!used || memchr(decoded, 0, used)) { epd_app_free(decoded); return WR_ERR_INTEGRITY; }
    decoded[used] = 0; *out = decoded; *out_size = used;
    return WR_OK;
}
