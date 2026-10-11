/* Endpoint conventions adapted from epdiy-epub/src/weread; ../LICENSE.reference. */
#include "wr_internal.h"
#include "epd_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WR_HOST "weread.qq.com"
#define WR_FP "b5b10a1f4dd84a0e9c5f0a7b3c6d2e11"
enum { F_VID, F_SKEY, F_ACCESS, F_REFRESH, F_NAME, F_PF, F_UID, F_COUNT };
static const char *const field_names[F_COUNT] =
    { "vid", "skey", "accessToken", "refreshToken", "nickname", "pf", "loginUid" };

struct wr_client
{
    wr_cancel_fn cancel;
    void *user;
    char *fields[F_COUNT];
    wr_json *cookies;
    wr_json *login_pending; /* Original weblogin parameters, never the OTP. */
    bool authenticated;
    bool init_pending;
    uint32_t nonce;
    int64_t server_time;
    time_t local_time;
};
static const char *str(const wr_client_t *c, unsigned f)
{ return c && c->fields[f] ? c->fields[f] : ""; }
static bool cancelled(const wr_client_t *c)
{ return c->cancel && c->cancel(c->user); }
static void login_clear(wr_client_t *c)
{
    j_delete(c->login_pending); c->login_pending = NULL;
    epd_app_free(c->fields[F_UID]); c->fields[F_UID] = NULL;
    c->init_pending = false;
}
static void state_clear(wr_client_t *c)
{
    unsigned i;
    login_clear(c);
    for (i = 0; i < F_COUNT; ++i) { epd_app_free(c->fields[i]); c->fields[i] = NULL; }
    j_delete(c->cookies); c->cookies = NULL;
    c->authenticated = false; c->init_pending = false;
}
wr_client_t *wr_client_create(wr_cancel_fn cancel, void *user)
{
    wr_client_t *c = wr_alloc(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c)); c->cancel = cancel; c->user = user;
    return c;
}
void wr_client_destroy(wr_client_t *c)
{ if (c) { state_clear(c); epd_app_free(c); } }
void wr_client_logout(wr_client_t *c) { if (c) state_clear(c); }
void wr_client_set_cancel(wr_client_t *c, wr_cancel_fn cancel, void *user)
{ if (c) { c->cancel = cancel; c->user = user; } }
bool wr_client_authenticated(const wr_client_t *c) { return c && c->authenticated; }
bool wr_client_login_waiting_otp(const wr_client_t *c) { return c && c->login_pending; }
const char *wr_client_account_id(const wr_client_t *c) { return str(c, F_VID); }
const char *wr_client_account_name(const wr_client_t *c) { return str(c, F_NAME); }
static wr_result_t clone(const wr_client_t *c, wr_client_t *out)
{
    unsigned i;
    *out = *c;
    memset(out->fields, 0, sizeof(out->fields)); out->cookies = NULL;
    out->login_pending = NULL;
    for (i = 0; i < F_COUNT; ++i)
        if (c->fields[i] && !(out->fields[i] = wr_dup(c->fields[i]))) goto oom;
    if (c->cookies && !(out->cookies = wr_cjson_Duplicate(c->cookies, true))) goto oom;
    if (c->login_pending && !(out->login_pending = wr_cjson_Duplicate(c->login_pending, true))) goto oom;
    return WR_OK;
oom:
    state_clear(out); return WR_ERR_MEMORY;
}
static wr_result_t finish(wr_client_t *c, wr_client_t *next, wr_result_t r)
{
    /* A cancellation never publishes even successfully rotated cookies. */
    if (r == WR_ERR_CANCELLED || cancelled(c)) r = WR_ERR_CANCELLED;
    else
    {
        state_clear(c);
        *c = *next;
        memset(next->fields, 0, sizeof(next->fields)); next->cookies = NULL;
        next->login_pending = NULL;
    }
    state_clear(next);
    return r;
}
static wr_result_t set_field(wr_client_t *c, unsigned field, const wr_json *value, bool required)
{
    char *s;
    wr_result_t r = wr_json_text(value, required, &s);
    if (r != WR_OK) return r;
    epd_app_free(c->fields[field]); c->fields[field] = s;
    return WR_OK;
}
static const char *jstr(const wr_json *j, const char *key)
{
    const wr_json *v = j_get(j, key);
    return j_is_string(v) ? v->valuestring : "";
}
static int64_t now_seconds(const wr_client_t *c)
{
    time_t now = time(NULL);
    if (c->server_time > 0)
        return c->server_time + ((now > c->local_time && c->local_time > 0) ? (int64_t)(now-c->local_time) : 0);
    return now > 0 ? (int64_t)now : 0;
}
/* IMF-fixdate in UTC; avoid platform-specific timegm/local timezone state. */
static int64_t http_date(const char *s)
{
    char weekday[4], mon[4], zone[4], extra;
    int day, year, hour, minute, second, month, era, yoe, doy, doe;
    int64_t days;
    static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (sscanf(s, "%3s, %d %3s %d %d:%d:%d %3s%c", weekday, &day, mon, &year,
               &hour, &minute, &second, zone, &extra) != 8) return 0;
    (void)weekday;
    for (month = 0; month < 12; ++month) if (!strcmp(mon, months[month])) break;
    if (month == 12 || strcmp(zone,"GMT") || year < 1970 || year > 9999 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    ++month;
    year -= month <= 2;
    era = year / 400; yoe = year - era * 400;
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}
static bool cookie_token(const char *s, bool name)
{
    if (name && !*s) return false;
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (c <= 32 || c >= 127 || c == ';' || c == ',' || c == '\\' || c == '"') return false;
        if (name && strchr("()<>@:/[]?={}", c)) return false;
    }
    return true;
}
static bool cookie_account_matches(const wr_client_t *c, const wr_json *jar)
{
    const wr_json *item;
    for (item = jar ? jar->child : NULL; item; item = item->next)
    {
        int64_t expiry = 0;
        wr_json_int(j_get(item,"expires"),&expiry);
        if (!strcmp(jstr(item,"name"),"wr_vid") && *jstr(item,"value") &&
            (!expiry || expiry > now_seconds(c)) &&
            strcmp(jstr(item,"value"),str(c,F_VID))) return false;
    }
    return true;
}
static wr_result_t cookie_put(wr_json **jar, const char *name, const char *value,
                               const char *path, int64_t expiry)
{
    wr_json *item, *old;
    char number[32];
    if (!cookie_token(name,true) || !cookie_token(value,false) || path[0] != '/' ||
        strchr(path,'\r') || strchr(path,'\n')) return WR_ERR_PROTOCOL;
    if (!*jar && !(*jar = j_array())) return WR_ERR_MEMORY;
    item = j_object();
    snprintf(number, sizeof(number), "%lld", (long long)expiry);
    if (!item || !j_string(item,"name",name) || !j_string(item,"value",value) ||
        !j_string(item,"path",path) || !j_string(item,"expires",number))
    { j_delete(item); return WR_ERR_MEMORY; }
    for (old = (*jar)->child; old; old = old->next)
        if (!strcmp(jstr(old,"name"),name) && !strcmp(jstr(old,"path"),path)) break;
    if (old) j_delete(wr_cjson_DetachItemViaPointer(*jar,old));
    if (!wr_cjson_AddItemToArray(*jar,item)) { j_delete(item); return WR_ERR_MEMORY; }
    return WR_OK;
}
static bool path_matches(const char *cookie, const char *path)
{
    size_t n = strlen(cookie);
    return !strncmp(cookie,path,n) && (!path[n] || path[n] == '?' || cookie[n-1] == '/' || path[n] == '/');
}
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return s;
}
static wr_result_t set_cookie(wr_client_t *c, wr_json **jar, const char *header, const char *request_path)
{
    char *copy = wr_dup(header), *first, *part, *next, *eq, *name, *value;
    char *path = NULL;
    int64_t expiry = 0, maxage = 0;
    bool has_maxage = false, accept = true;
    wr_result_t r = WR_ERR_MEMORY;
    size_t pathlen;
    const char *query;
    if (!copy) return r;
    query = strchr(request_path,'?');
    pathlen = query ? (size_t)(query-request_path) : strlen(request_path);
    while (pathlen > 1 && request_path[pathlen-1] != '/') --pathlen;
    path = wr_dup_n(request_path, pathlen > 1 ? pathlen-1 : 1);
    if (!path) goto done;
    first = copy; part = strchr(copy,';');
    if (part) *part++ = 0;
    eq = strchr(first,'=');
    if (!eq) { r = WR_ERR_PROTOCOL; goto done; }
    *eq++ = 0; name = trim(first); value = trim(eq);
    for (; part; part = next)
    {
        next = strchr(part,';'); if (next) *next++ = 0;
        part = trim(part); eq = strchr(part,'=');
        if (!eq) continue;
        *eq++ = 0; part = trim(part); eq = trim(eq);
        if (wr_ascii_equal(part,"Domain"))
        {
            if (*eq == '.') ++eq;
            accept = wr_ascii_equal(eq,WR_HOST) || wr_ascii_equal(eq,"qq.com");
        }
        else if (wr_ascii_equal(part,"Path") && *eq == '/')
        {
            char *newpath = wr_dup(eq);
            if (!newpath) goto done;
            epd_app_free(path); path = newpath;
        }
        else if (wr_ascii_equal(part,"Expires")) expiry = http_date(eq);
        else if (wr_ascii_equal(part,"Max-Age"))
        {
            wr_json v; memset(&v,0,sizeof(v)); v.type = wr_cjson_String; v.valuestring = eq;
            if (wr_json_int(&v,&maxage) == WR_OK) has_maxage = true;
        }
    }
    if (has_maxage)
    {
        int64_t now = now_seconds(c);
        expiry = maxage <= 0 ? -1 : maxage > INT64_MAX-now ? INT64_MAX : now+maxage;
    }
    else if (expiry && expiry <= now_seconds(c)) expiry = -1;
    r = accept ? cookie_put(jar,name,value,path,expiry) : WR_OK;
done:
    epd_app_free(copy); epd_app_free(path); return r;
}
static wr_result_t cookie_header(wr_client_t *c, const char *path, char **out)
{
    const wr_json *item;
    size_t needed = 1, used = 0;
    bool has_vid = false, has_skey = false;
    char *text;
    const char *fallback_skey = *str(c,F_SKEY) ? str(c,F_SKEY) : str(c,F_ACCESS);
    *out = NULL;
    for (item = c->cookies ? c->cookies->child : NULL; item; item = item->next)
    {
        int64_t expiry = 0;
        const char *name = jstr(item,"name"), *value = jstr(item,"value");
        wr_json_int(j_get(item,"expires"),&expiry);
        if (!path_matches(jstr(item,"path"),path)) continue;
        if (!strcmp(name,"wr_vid")) has_vid = true;
        if (!strcmp(name,"wr_skey")) has_skey = true;
        if (!*value || (expiry && expiry <= now_seconds(c))) continue;
        if (strlen(name) > SIZE_MAX-needed-3 || strlen(value) > SIZE_MAX-needed-strlen(name)-3) return WR_ERR_MEMORY;
        needed += strlen(name)+strlen(value)+3;
    }
    if (!has_vid && *str(c,F_VID))
    {
        if (!cookie_token(str(c,F_VID),false)) return WR_ERR_PROTOCOL;
        if (strlen(str(c,F_VID)) > SIZE_MAX-needed-9) return WR_ERR_MEMORY;
        needed += strlen(str(c,F_VID))+9;
    }
    if (!has_skey && *fallback_skey)
    {
        if (!cookie_token(fallback_skey,false)) return WR_ERR_PROTOCOL;
        if (strlen(fallback_skey) > SIZE_MAX-needed-10) return WR_ERR_MEMORY;
        needed += strlen(fallback_skey)+10;
    }
    if (needed == 1) return WR_OK;
    text = wr_alloc(needed);
    if (!text) return WR_ERR_MEMORY;
    for (item = c->cookies ? c->cookies->child : NULL; item; item = item->next)
    {
        int64_t expiry = 0;
        wr_json_int(j_get(item,"expires"),&expiry);
        if (!*jstr(item,"value") || (expiry && expiry <= now_seconds(c)) || !path_matches(jstr(item,"path"),path)) continue;
        used += (size_t)sprintf(text+used,"%s%s=%s",used ? "; " : "",jstr(item,"name"),jstr(item,"value"));
    }
    if (!has_vid && *str(c,F_VID)) used += (size_t)sprintf(text+used,"%swr_vid=%s",used ? "; " : "",str(c,F_VID));
    if (!has_skey && *fallback_skey) used += (size_t)sprintf(text+used,"%swr_skey=%s",used ? "; " : "",fallback_skey);
    text[used] = 0; *out = text; return WR_OK;
}
typedef struct
{
    wr_client_t *client;
    const char *path;
    wr_json *cookies;
    wr_result_t error;
    int64_t date;
} request_context;
static bool http_cancel(void *user) { return cancelled(((request_context *)user)->client); }
static bool http_header(void *user, const char *name, const char *value)
{
    request_context *ctx = user;
    if (http_cancel(user)) { ctx->error = WR_ERR_CANCELLED; return false; }
    if (wr_ascii_equal(name,"Set-Cookie"))
        ctx->error = set_cookie(ctx->client,&ctx->cookies,value,ctx->path);
    else if (wr_ascii_equal(name,"Date")) ctx->date = http_date(value);
    return ctx->error == WR_OK;
}
static wr_result_t request(wr_client_t *c, const char *path, const char *body,
                           const char *referer, char **out, size_t *size)
{
    epd_http_response_t response = {0};
    epd_http_request_t req = {0};
    epd_http_header_t headers[8];
    request_context ctx = {0};
    char *cookie = NULL;
    wr_result_t r;
    bool ok;
    *out = NULL; *size = 0;
    if (cancelled(c)) return WR_ERR_CANCELLED;
    r = cookie_header(c,path,&cookie);
    if (r != WR_OK) return r;
    ctx.client = c; ctx.path = path;
    if (c->cookies && !(ctx.cookies = wr_cjson_Duplicate(c->cookies,true)))
    { epd_app_free(cookie); return WR_ERR_MEMORY; }
    headers[0].name = "User-Agent"; headers[0].value = "WeRead/1.0.0 WRBrand/remarkable wr_eink";
    headers[1].name = "Accept"; headers[1].value = "application/json, text/plain, */*";
    headers[2].name = "Origin"; headers[2].value = "https://weread.qq.com";
    headers[3].name = "Referer"; headers[3].value = referer ? referer : "https://weread.qq.com/";
    headers[4].name = "CLIENT_TYPE"; headers[4].value = "1";
    headers[5].name = "X-Is-Serverside"; headers[5].value = "1";
    req.header_count = 6;
    if (body) { headers[req.header_count].name = "Content-Type"; headers[req.header_count++].value = "application/json"; }
    if (cookie && *cookie) { headers[req.header_count].name = "Cookie"; headers[req.header_count++].value = cookie; }
    req.host = WR_HOST; req.path = path; req.method = body ? "POST" : "GET";
    req.headers = headers; req.body = body; req.body_size = body ? strlen(body) : 0;
    req.timeout_ms = body ? 75000 : 20000;
    req.cancelled = http_cancel; req.user = &ctx; req.on_header = http_header;
    ok = epd_http_perform(&req,&response);
    epd_app_free(cookie);
    if (cancelled(c)) r = WR_ERR_CANCELLED;
    else if (ctx.error != WR_OK) r = ctx.error;
    else if (!ok) r = WR_ERR_HTTP;
    else if (response.status == 401) r = WR_ERR_AUTH;
    else if (response.status == 403) r = WR_ERR_PERMISSION;
    else if (response.status != 200) r = WR_ERR_HTTP;
    else if (!response.body || !response.size || memchr(response.body,0,response.size)) r = WR_ERR_PROTOCOL;
    else if (*str(c,F_VID) && strcmp(path,"/web/login/getinfo") &&
             strcmp(path,"/web/login/weblogin") && !cookie_account_matches(c,ctx.cookies))
        r = WR_ERR_AUTH;
    else
    {
        r = WR_OK;
        j_delete(c->cookies); c->cookies = ctx.cookies; ctx.cookies = NULL;
        if (ctx.date) { c->server_time = ctx.date; c->local_time = time(NULL); }
        *out = response.body; *size = response.size; response.body = NULL;
    }
    if (r == WR_ERR_AUTH) c->authenticated = false;
    j_delete(ctx.cookies); epd_http_response_clear(&response);
    return r;
}
static wr_result_t request_json_raw(wr_client_t *c, const char *path, const wr_json *payload, wr_json **out)
{
    char *body = NULL, *response = NULL;
    size_t n;
    wr_result_t r;
    *out = NULL;
    if (payload)
    {
        r = wr_json_print(payload,&body,&n);
        if (r != WR_OK) return r;
    }
    r = request(c,path,body,NULL,&response,&n); epd_app_free(body);
    if (r == WR_OK) r = wr_json_parse(response,n,out);
    epd_app_free(response);
    return r;
}
static wr_result_t request_json(wr_client_t *c, const char *path, const wr_json *payload, wr_json **out)
{
    wr_result_t r = request_json_raw(c,path,payload,out);
    if (r == WR_OK) r = wr_remote_result(*out);
    if (r == WR_ERR_AUTH) c->authenticated = false;
    if (r != WR_OK) { j_delete(*out); *out = NULL; }
    return r;
}
static wr_result_t begin(wr_client_t *c, char **url)
{
    wr_json *payload, *root = NULL;
    char *encoded = NULL;
    wr_result_t r;
    state_clear(c);
    payload = j_object();
    if (!payload) return WR_ERR_MEMORY;
    r = request_json(c,"/web/login/getuid",payload,&root);
    j_delete(payload);
    if (r != WR_OK) return r;
    r = set_field(c,F_UID,j_get(wr_payload(root),"uid"),true);
    j_delete(root);
    if (r != WR_OK) return r;
    encoded = wr_url_encode(str(c,F_UID));
    if (!encoded) return WR_ERR_MEMORY;
    *url = wr_format("https://weread.qq.com/web/confirm?pf=2&uid=%s",encoded);
    epd_app_free(encoded);
    return *url ? WR_OK : WR_ERR_MEMORY;
}
wr_result_t wr_client_login_begin(wr_client_t *c, char **url)
{
    wr_client_t next;
    wr_result_t r;
    if (url) *url = NULL;
    if (!c || !url) return WR_ERR_ARGUMENT;
    r = clone(c,&next); if (r != WR_OK) return r;
    r = begin(&next,url);
    r = finish(c,&next,r);
    if (r != WR_OK) { epd_app_free(*url); *url = NULL; }
    return r;
}
static wr_result_t init_session(wr_client_t *c)
{
    wr_json *p = j_object(), *root = NULL;
    wr_result_t r = WR_ERR_MEMORY;
    if (!p || !j_string(p,"vid",str(c,F_VID)) || !j_string(p,"skey",str(c,F_ACCESS)) ||
        !j_string(p,"rt",str(c,F_REFRESH)) || !j_string(p,"pf",str(c,F_PF)) || !j_bool(p,"ql",false)) goto done;
    r = request_json(c,"/web/login/session/init",p,&root);
    if (r == WR_OK)
    {
        if (!wr_json_true(j_get(wr_payload(root),"success"))) r = WR_ERR_REMOTE;
        else { c->authenticated = true; login_clear(c); }
    }
done:
    j_delete(p); j_delete(root); return r;
}
static bool login_flag(const wr_json *root, const char *name)
{
    return wr_json_true(j_get(root,name)) || wr_json_true(j_get(wr_payload(root),name));
}
static wr_result_t weblogin(wr_client_t *c, const wr_json *parameters)
{
    static const unsigned login_fields[] = {F_ACCESS,F_REFRESH,F_NAME,F_PF};
    wr_json *root = NULL;
    const wr_json *data;
    char *values[F_COUNT] = {0};
    unsigned i;
    wr_result_t r = request_json_raw(c,"/web/login/weblogin",parameters,&root);
    if (r != WR_OK) goto done;
    /* These explicit OTP outcomes may accompany a generic business error. */
    if (login_flag(root,"verifyCodeExpired")) r = WR_VERIFY_EXPIRED;
    else if (login_flag(root,"reachMaxAttemptTimes")) r = WR_VERIFY_LIMIT;
    else if (login_flag(root,"isVerifyFailed")) r = WR_VERIFY_FAILED;
    else if (login_flag(root,"needRemoteLoginVerify")) r = WR_VERIFY_REQUIRED;
    else r = wr_remote_result(root);
    if (r != WR_OK) goto done;
    data = wr_payload(root);
    for (i = 0; i < sizeof(login_fields)/sizeof(login_fields[0]); ++i)
    {
        unsigned f = login_fields[i];
        r = wr_json_text(j_get(data,field_names[f]),f == F_ACCESS,&values[f]);
        if (r != WR_OK) goto done;
    }
    if (j_get(data,"vid"))
    { r = wr_json_text(j_get(data,"vid"),true,&values[F_VID]); if (r != WR_OK) goto done; }
    values[F_SKEY] = wr_dup(values[F_ACCESS]);
    if (!values[F_SKEY]) { r = WR_ERR_MEMORY; goto done; }
    /* Publish tokens together, so a parse/allocation failure cannot mix them. */
    for (i = 0; i < F_COUNT; ++i)
        if (values[i]) { epd_app_free(c->fields[i]); c->fields[i] = values[i]; values[i] = NULL; }
    c->init_pending = true;
    r = init_session(c);
done:
    if (r == WR_VERIFY_EXPIRED || r == WR_VERIFY_LIMIT) login_clear(c);
    if (r == WR_ERR_AUTH) c->authenticated = false;
    for (i = 0; i < F_COUNT; ++i) epd_app_free(values[i]);
    j_delete(root);
    return r;
}
static wr_result_t poll(wr_client_t *c)
{
    wr_json *p = NULL, *root = NULL;
    const wr_json *data, *v;
    wr_result_t r = WR_ERR_MEMORY;
    if (c->init_pending) return init_session(c);
    if (c->authenticated) return WR_OK;
    if (!*str(c,F_UID)) return WR_ERR_ARGUMENT;
    p = j_object();
    if (!p || !j_string(p,"uid",str(c,F_UID))) goto done;
    r = request_json(c,"/web/login/getinfo",p,&root);
    j_delete(p); p = NULL;
    if (r != WR_OK) goto done;
    data = wr_payload(root);
    v = j_get(data,"state"); if (!v) v = j_get(data,"status");
    if (j_is_string(v))
    {
        if (wr_ascii_equal(v->valuestring,"expired") || wr_ascii_equal(v->valuestring,"timeout"))
        { r = WR_EXPIRED; goto done; }
        if (wr_ascii_equal(v->valuestring,"cancelled") || wr_ascii_equal(v->valuestring,"cancel") ||
            wr_ascii_equal(v->valuestring,"failed") || wr_ascii_equal(v->valuestring,"fail"))
        { r = WR_ERR_REMOTE; goto done; }
    }
    if (!j_get(data,"vid") && !j_get(data,"skey"))
    {
        r = (wr_json_true(j_get(data,"success")) || wr_json_true(j_get(data,"succeed"))) ? WR_ERR_PROTOCOL : WR_PENDING;
        goto done;
    }
    r = set_field(c,F_VID,j_get(data,"vid"),true); if (r != WR_OK) goto done;
    r = set_field(c,F_SKEY,j_get(data,"skey"),true); if (r != WR_OK) goto done;
    p = j_object(); r = WR_ERR_MEMORY;
    if (!p || !j_string(p,"vid",str(c,F_VID)) || !j_string(p,"skey",str(c,F_SKEY)) ||
        !j_string(p,"uid",str(c,F_UID)) || !j_string(p,"fp",WR_FP) || !j_number(p,"isAutoLogout",0)) goto done;
    {
        char *code = NULL;
        r = wr_json_text(j_get(data,"code"),true,&code);
        if (r != WR_OK) goto done;
        v = j_string(p,"code",code); epd_app_free(code);
        if (!v) { r = WR_ERR_MEMORY; goto done; }
    }
    r = weblogin(c,p);
    if (r == WR_VERIFY_REQUIRED || r == WR_VERIFY_FAILED)
    { c->login_pending = p; p = NULL; }
done:
    if (r == WR_EXPIRED) login_clear(c);
    j_delete(p); j_delete(root); return r;
}
wr_result_t wr_client_login_poll(wr_client_t *c)
{
    wr_client_t next;
    wr_result_t r;
    if (!c) return WR_ERR_ARGUMENT;
    if (c->login_pending) return cancelled(c) ? WR_ERR_CANCELLED : WR_VERIFY_REQUIRED;
    r = clone(c,&next); if (r != WR_OK) return r;
    r = poll(&next);
    return finish(c,&next,r);
}
wr_result_t wr_client_login_verify(wr_client_t *c, const char *otp)
{
    wr_client_t next;
    wr_json *p = NULL;
    wr_result_t r;
    unsigned i;
    if (!c || !c->login_pending) return WR_ERR_ARGUMENT;
    if (!c->init_pending)
    {
        if (!otp) return WR_ERR_ARGUMENT;
        for (i = 0; i < 4; ++i) if (otp[i] < '0' || otp[i] > '9') return WR_ERR_ARGUMENT;
        if (otp[4]) return WR_ERR_ARGUMENT;
    }
    if (cancelled(c)) return WR_ERR_CANCELLED;
    r = clone(c,&next); if (r != WR_OK) return r;
    if (next.init_pending) r = init_session(&next);
    else
    {
        p = wr_cjson_Duplicate(next.login_pending,true);
        if (!p || !j_string(p,"otp",otp)) r = WR_ERR_MEMORY;
        else r = weblogin(&next,p);
    }
    j_delete(p);
    return finish(c,&next,r);
}
static wr_result_t renew(wr_client_t *c)
{
    wr_json *p = NULL, *root = NULL;
    const wr_json *data, *cookie;
    wr_result_t r = WR_ERR_AUTH;
    if (!*str(c,F_VID) || (!*str(c,F_SKEY) && !*str(c,F_ACCESS))) return r;
    p = j_object(); r = WR_ERR_MEMORY;
    if (!p || !j_string(p,"rq","%2Fweb%2Fbook%2Fread") || !j_bool(p,"ql",false)) goto done;
    r = request_json(c,"/web/login/renewal",p,&root);
    if (r != WR_OK) goto done;
    data = wr_payload(root);
    if (!wr_json_true(j_get(data,"succ")) && !wr_json_true(j_get(data,"success")))
    { r = WR_ERR_AUTH; c->authenticated = false; goto done; }
    if (j_get(data,"accessToken"))
    {
        r = set_field(c,F_ACCESS,j_get(data,"accessToken"),true); if (r != WR_OK) goto done;
        r = set_field(c,F_SKEY,j_get(data,"accessToken"),true); if (r != WR_OK) goto done;
    }
    if (j_get(data,"refreshToken"))
    { r = set_field(c,F_REFRESH,j_get(data,"refreshToken"),false); if (r != WR_OK) goto done; }
    for (cookie = c->cookies ? c->cookies->child : NULL; cookie; cookie = cookie->next)
    {
        int64_t expiry = 0;
        wr_json_int(j_get(cookie,"expires"),&expiry);
        if (!strcmp(jstr(cookie,"name"),"wr_skey") &&
            path_matches(jstr(cookie,"path"),"/web/book/read") &&
            (!*jstr(cookie,"value") || (expiry && expiry <= now_seconds(c))))
        { r = WR_ERR_AUTH; c->authenticated = false; goto done; }
        if (expiry && expiry <= now_seconds(c)) continue;
        if (!strcmp(jstr(cookie,"name"),"wr_skey"))
        { r = set_field(c,F_SKEY,j_get(cookie,"value"),true); if (r != WR_OK) goto done; }
        if (!strcmp(jstr(cookie,"name"),"wr_vid") && strcmp(jstr(cookie,"value"),str(c,F_VID)))
        { r = WR_ERR_AUTH; c->authenticated = false; goto done; }
    }
    c->authenticated = true;
done:
    j_delete(p); j_delete(root); return r;
}
wr_result_t wr_client_renew(wr_client_t *c)
{
    wr_client_t next;
    wr_result_t r;
    if (!c) return WR_ERR_ARGUMENT;
    r = clone(c,&next); if (r != WR_OK) return r;
    r = renew(&next);
    return finish(c,&next,r);
}

wr_result_t wr_client_session_export(const wr_client_t *c, char **out, size_t *size)
{
    wr_json *root = NULL, *cookies;
    wr_result_t r = WR_ERR_MEMORY;
    unsigned i;
    if (out) *out = NULL;
    if (size) *size = 0;
    if (!c || !out || !size) return WR_ERR_ARGUMENT;
    root = j_object();
    if (!root || !j_number(root,"version",1) || !j_bool(root,"authenticated",c->authenticated) ||
        !j_bool(root,"initPending",c->init_pending)) goto done;
    for (i = 0; i < F_UID; ++i) if (!j_string(root,field_names[i],str(c,i))) goto done;
    cookies = c->cookies ? wr_cjson_Duplicate(c->cookies,true) : j_array();
    if (!cookies) goto done;
    if (!wr_cjson_AddItemToObject(root,"cookies",cookies)) { j_delete(cookies); goto done; }
    r = wr_json_print(root,out,size);
done:
    j_delete(root); return r;
}
wr_result_t wr_client_session_import(wr_client_t *c, const char *json, size_t size)
{
    wr_client_t next;
    wr_json *root = NULL;
    const wr_json *cookies, *cookie, *v;
    unsigned i;
    int64_t version;
    wr_result_t r;
    if (!c) return WR_ERR_ARGUMENT;
    memset(&next,0,sizeof(next)); next.cancel = c->cancel; next.user = c->user;
    r = wr_json_parse(json,size,&root);
    if (r != WR_OK) return r;
    r = WR_ERR_PROTOCOL;
    if (!j_is_object(root) || wr_json_int(j_get(root,"version"),&version) != WR_OK || version != 1) goto done;
    v = j_get(root,"authenticated");
    if (!wr_cjson_IsBool(v)) goto done;
    next.authenticated = wr_cjson_IsTrue(v);
    v = j_get(root,"initPending");
    if (v && !wr_cjson_IsBool(v)) goto done;
    next.init_pending = wr_cjson_IsTrue(v);
    if (next.init_pending && next.authenticated) goto done;
    for (i = 0; i < F_UID; ++i)
    {
        v = j_get(root,field_names[i]);
        if (v && !j_is_string(v)) { r = WR_ERR_PROTOCOL; goto done; }
        r = set_field(&next,i,v,false); if (r != WR_OK) goto done;
    }
    r = WR_ERR_PROTOCOL;
    if ((next.authenticated || next.init_pending) &&
        (!*str(&next,F_VID) || (!*str(&next,F_SKEY) && !*str(&next,F_ACCESS)))) goto done;
    if (next.init_pending && !*str(&next,F_ACCESS)) goto done;
    if (!cookie_token(str(&next,F_VID),false) || !cookie_token(str(&next,F_SKEY),false) ||
        !cookie_token(str(&next,F_ACCESS),false)) goto done;
    cookies = j_get(root,"cookies");
    if (cookies && !j_is_array(cookies)) goto done;
    for (cookie = cookies ? cookies->child : NULL; cookie; cookie = cookie->next)
    {
        int64_t expiry;
        if (!j_is_object(cookie) || !j_is_string(j_get(cookie,"name")) ||
            !j_is_string(j_get(cookie,"value")) || !j_is_string(j_get(cookie,"path")) ||
            wr_json_int(j_get(cookie,"expires"),&expiry) != WR_OK)
        { r = WR_ERR_PROTOCOL; goto done; }
        r = cookie_put(&next.cookies,jstr(cookie,"name"),jstr(cookie,"value"),jstr(cookie,"path"),expiry);
        if (r != WR_OK) goto done;
    }
    if (*str(&next,F_VID) && !cookie_account_matches(&next,next.cookies))
    { r = WR_ERR_PROTOCOL; goto done; }
    state_clear(c); *c = next; memset(next.fields,0,sizeof(next.fields)); next.cookies = NULL;
    r = WR_OK;
done:
    state_clear(&next); j_delete(root); return r;
}
typedef enum { FETCH_SHELF, FETCH_CATALOG, FETCH_CHAPTER } fetch_kind;
static wr_result_t model_fetch(wr_client_t *c, fetch_kind kind, const char *book,
                                char **out, size_t *size)
{
    char *body = NULL;
    wr_json *p = NULL, *ids = NULL;
    size_t ignored;
    wr_result_t r = WR_ERR_MEMORY;
    if (kind == FETCH_SHELF) return request(c,"/web/shelf/sync",NULL,NULL,out,size);
    p = j_object();
    if (!p) goto done;
    ids = wr_cjson_AddArrayToObject(p,"bookIds");
    if (!ids) goto done;
    {
        wr_json *id = wr_cjson_CreateString(book);
        if (!id) goto done;
        if (!wr_cjson_AddItemToArray(ids,id)) { j_delete(id); goto done; }
    }
    ids = wr_cjson_AddArrayToObject(p,"synckeys");
    if (!ids) goto done;
    {
        wr_json *zero = wr_cjson_CreateNumber(0);
        if (!zero) goto done;
        if (!wr_cjson_AddItemToArray(ids,zero)) { j_delete(zero); goto done; }
    }
    ids = wr_cjson_AddArrayToObject(p,"updateTimes");
    if (!ids) goto done;
    {
        wr_json *zero = wr_cjson_CreateNumber(0);
        if (!zero) goto done;
        if (!wr_cjson_AddItemToArray(ids,zero)) { j_delete(zero); goto done; }
    }
    r = wr_json_print(p,&body,&ignored);
    if (r == WR_OK) r = request(c,"/web/book/chapterInfos",body,NULL,out,size);
done:
    j_delete(p); epd_app_free(body); return r;
}
static wr_result_t chapter_fetch(wr_client_t *c, const char *book, const char *chapter,
                                  char **out, size_t *size)
{
    static const char *const paths[] = {"/web/book/chapter/e_0","/web/book/chapter/e_1","/web/book/chapter/e_3"};
    char *encoded = NULL, *referer = NULL, *joined = NULL;
    size_t total = 0, i;
    wr_result_t r = wr_encode_id(book,&encoded);
    if (r != WR_OK) return r;
    referer = wr_format("https://weread.qq.com/web/reader/%s",encoded);
    epd_app_free(encoded);
    if (!referer) return WR_ERR_MEMORY;
    for (i = 0; i < 3; ++i)
    {
        char *body = NULL, *response = NULL, *grown;
        size_t length = 0, checked_size = 0, pos = 0;
        const char *checked = NULL;
        int64_t now = now_seconds(c);
        if (cancelled(c)) { r = WR_ERR_CANCELLED; goto done; }
        if (now <= 0 || now > UINT32_MAX) { r = WR_ERR_PROTOCOL; goto done; }
        r = wr_content_request(book,chapter,(uint32_t)now,++c->nonce,&body);
        if (r != WR_OK) goto done;
        r = request(c,paths[i],body,referer,&response,&length); epd_app_free(body);
        if (r != WR_OK) { epd_app_free(response); goto done; }
        while (pos < length && (response[pos] == ' ' || response[pos] == '\n' || response[pos] == '\r' || response[pos] == '\t')) ++pos;
        if (pos < length && (response[pos] == '{' || response[pos] == '['))
        {
            wr_json *error = NULL;
            r = wr_json_parse(response,length,&error);
            if (r == WR_OK) { r = wr_remote_result(error); if (r == WR_OK) r = WR_ERR_PROTOCOL; }
            j_delete(error);
        }
        else r = wr_fragment_check(response,length,&checked,&checked_size);
        if (r != WR_OK) { epd_app_free(response); goto done; }
        if (checked_size > SIZE_MAX-total-1) { epd_app_free(response); r = WR_ERR_MEMORY; goto done; }
        grown = wr_alloc(total + checked_size + 1);
        if (!grown) { epd_app_free(response); r = WR_ERR_MEMORY; goto done; }
        if (total) memcpy(grown,joined,total);
        memcpy(grown+total,checked,checked_size); total += checked_size; grown[total] = 0;
        epd_app_free(joined); joined = grown; epd_app_free(response);
    }
    if (cancelled(c)) r = WR_ERR_CANCELLED;
    else r = wr_content_decode(joined,total,out,size);
done:
    epd_app_free(joined); epd_app_free(referer); return r;
}
static wr_result_t fetch(wr_client_t *c, fetch_kind kind, const char *book, const char *chapter,
                          void **out, size_t *count)
{
    wr_client_t next;
    wr_result_t r;
    unsigned attempt;
    *out = NULL; *count = 0;
    if (!c) return WR_ERR_ARGUMENT;
    if (!c->authenticated) return WR_ERR_AUTH;
    r = clone(c,&next); if (r != WR_OK) return r;
    for (attempt = 0; attempt < 2; ++attempt)
    {
        if (kind == FETCH_CHAPTER)
        {
            char *text = NULL;
            r = chapter_fetch(&next,book,chapter,&text,count); *out = text;
        }
        else
        {
            char *json = NULL;
            size_t size = 0;
            r = model_fetch(&next,kind,book,&json,&size);
            if (r == WR_OK)
            {
                if (kind == FETCH_SHELF)
                { wr_book_t *books = NULL; r = wr_books_parse(json,size,&books,count); *out = books; }
                else
                { wr_chapter_t *chapters = NULL; r = wr_chapters_parse(json,size,&chapters,count); *out = chapters; }
            }
            epd_app_free(json);
        }
        if (r == WR_EXPIRED) r = WR_ERR_AUTH;
        if (r != WR_ERR_AUTH) break;
        next.authenticated = false;
        if (attempt) break;
        r = renew(&next);
        if (r != WR_OK) break;
    }
    r = finish(c,&next,r);
    if (r != WR_OK)
    {
        if (kind == FETCH_SHELF) wr_books_free(*out,*count);
        else if (kind == FETCH_CATALOG) wr_chapters_free(*out,*count);
        else epd_app_free(*out);
        *out = NULL; *count = 0;
    }
    return r;
}
wr_result_t wr_client_shelf(wr_client_t *c, wr_book_t **books, size_t *count)
{
    void *out = NULL;
    wr_result_t r;
    if (books) *books = NULL;
    if (count) *count = 0;
    if (!books || !count) return WR_ERR_ARGUMENT;
    r = fetch(c,FETCH_SHELF,NULL,NULL,&out,count); *books = out; return r;
}
wr_result_t wr_client_catalog(wr_client_t *c, const char *book, wr_chapter_t **chapters, size_t *count)
{
    void *out = NULL;
    wr_result_t r;
    if (chapters) *chapters = NULL;
    if (count) *count = 0;
    if (!chapters || !count || !book || !*book) return WR_ERR_ARGUMENT;
    r = fetch(c,FETCH_CATALOG,book,NULL,&out,count); *chapters = out; return r;
}
wr_result_t wr_client_chapter(wr_client_t *c, const char *book, const char *chapter, char **content, size_t *size)
{
    void *out = NULL;
    wr_result_t r;
    if (content) *content = NULL;
    if (size) *size = 0;
    if (!content || !size || !book || !*book || !chapter || !*chapter) return WR_ERR_ARGUMENT;
    r = fetch(c,FETCH_CHAPTER,book,chapter,&out,size); *content = out; return r;
}
