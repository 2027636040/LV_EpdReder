#include "wr_internal.h"
#include "epd_http.h"
#include "private/wr_md5.h"
#include "private/wr_base64.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static size_t live, allocations, checks;
static long fail_after = -1;
typedef union { struct { size_t size; unsigned magic; } m; long double align; void *pointer; } allocation;
void *epd_app_alloc(size_t n, epd_app_memory_t region)
{
    allocation *p;
    CHECK(region == EPD_APP_PSRAM);
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    p = malloc(sizeof(*p) + n);
    CHECK(p != NULL);
    p->m.size = n; p->m.magic = 0x77617265; ++live; ++allocations;
    return p+1;
}
void epd_app_free(void *ptr)
{
    allocation *p;
    if (!ptr) return;
    p = (allocation *)ptr - 1;
    CHECK(p->m.magic == 0x77617265 && live > 0);
    p->m.magic = 0; --live; free(p);
}
typedef struct
{
    const char *path, *body, *set_cookie, *cookie_contains;
    int status;
    bool fail, cancel_after;
    const char *set_cookie2;
    const char *cookie_absent;
    const char *request_body;
} reply;
static reply replies[32];
static size_t reply_count, reply_index;
static bool stop;
static size_t cancel_on_check, cancel_checks;
static bool cancel_cb(void *user)
{
    CHECK(user == &stop);
    if (cancel_on_check && ++cancel_checks == cancel_on_check) stop = true;
    return stop;
}
static void reset_http(void)
{
    memset(replies,0,sizeof(replies)); reply_count = reply_index = 0;
    stop = false; cancel_on_check = cancel_checks = 0;
}
static void add(const char *path, const char *body, const char *set_cookie, const char *cookie)
{
    reply *p = &replies[reply_count++];
    CHECK(reply_count <= 32);
    p->path=path; p->body=body; p->set_cookie=set_cookie; p->cookie_contains=cookie; p->status=200;
}
static const char *header_value(const epd_http_request_t *req, const char *name)
{
    size_t i;
    for (i=0;i<req->header_count;++i) if (!strcmp(req->headers[i].name,name)) return req->headers[i].value;
    return "";
}
bool epd_http_perform(const epd_http_request_t *req, epd_http_response_t *res)
{
    reply *p;
    size_t i;
    const char *cookie = NULL;
    CHECK(res->body == NULL && res->size == 0 && res->status == 0);
    CHECK(reply_index < reply_count);
    p = &replies[reply_index++];
    CHECK(!strcmp(req->host,"weread.qq.com")); CHECK(!strcmp(req->path,p->path));
    CHECK(!strcmp(header_value(req,"User-Agent"),"WeRead/1.0.0 WRBrand/remarkable wr_eink"));
    CHECK(!strcmp(header_value(req,"Accept"),"application/json, text/plain, */*"));
    CHECK(!strcmp(header_value(req,"Origin"),"https://weread.qq.com"));
    CHECK(!strcmp(header_value(req,"CLIENT_TYPE"),"1"));
    CHECK(!strcmp(header_value(req,"X-Is-Serverside"),"1"));
    CHECK(!*header_value(req,"Connection") && !*header_value(req,"Host"));
    CHECK(req->timeout_ms==(req->body ? 75000u : 20000u));
    if (req->body) CHECK(!strcmp(header_value(req,"Content-Type"),"application/json"));
    if (strstr(req->path,"/web/book/chapter/e_"))
        CHECK(!strcmp(header_value(req,"Referer"),"https://weread.qq.com/web/reader/6ef420314626f6f6b2d616c706861774"));
    else CHECK(!strcmp(header_value(req,"Referer"),"https://weread.qq.com/"));
    CHECK(req->body_limit == 0);
    if (p->request_body) CHECK(req->body && !strcmp(req->body,p->request_body));
    for (i=0;i<req->header_count;++i)
        if (!strcmp(req->headers[i].name,"Cookie")) cookie=req->headers[i].value;
    if (p->cookie_contains)
    {
        if (*p->cookie_contains) CHECK(cookie && strstr(cookie,p->cookie_contains));
        else CHECK(cookie == NULL);
    }
    if (p->cookie_absent) CHECK(!cookie || !strstr(cookie,p->cookie_absent));
    if (req->body)
    {
        wr_json *json=NULL;
        CHECK(req->body_size == strlen(req->body));
        if (wr_json_parse(req->body,req->body_size,&json) != WR_OK) return false;
        j_delete(json);
    }
    if (req->cancelled(req->user)) return false;
    res->status=p->status;
    if (!req->on_header(req->user,"Date","Sun, 11 Oct 2026 00:00:00 GMT")) return false;
    if (p->set_cookie && !req->on_header(req->user,"Set-Cookie",p->set_cookie)) return false;
    if (p->set_cookie2 && !req->on_header(req->user,"set-cookie",p->set_cookie2)) return false;
    if (p->cancel_after) { stop=true; return false; }
    if (p->fail) return false;
    res->body=wr_dup(p->body ? p->body : "");
    if (!res->body) return false;
    res->size=strlen(res->body);
    return true;
}
void epd_http_response_clear(epd_http_response_t *res)
{ epd_app_free(res->body); memset(res,0,sizeof(*res)); }

static const char session[] = "{\"version\":1,\"authenticated\":true,\"vid\":\"mock-account\",\"skey\":\"mock-old\",\"nickname\":\"Mock Reader\",\"cookies\":[]}";
static wr_client_t *logged_in(void)
{
    wr_client_t *c=wr_client_create(cancel_cb,&stop);
    CHECK(c); CHECK(wr_client_session_import(c,session,sizeof(session)-1)==WR_OK);
    CHECK(wr_client_authenticated(c)); CHECK(!strcmp(wr_client_account_id(c),"mock-account"));
    return c;
}
static const char shelf[] = "{\"books\":[{\"bookId\":\"book-alpha\",\"title\":\"Mock & Book\",\"author\":\"A\",\"format\":\"epub\",\"cover\":\"https://example.invalid/mock.png\"}]}";
static const char catalog[] = "{\"data\":[{\"bookId\":\"book-alpha\",\"chapterInfos\":[{\"chapterUid\":1,\"title\":\"One\",\"chapterIdx\":0}]}]}";
static const char *const shards[] = {
    "005cea2f616a8a4d8ef69f39d962f27f0PGh0bWw+PGJG1HvZZHA+VG",
    "0186c217885b89aea06a838115fe3026VzdCAmYW1wOyB0ZXh0Ljwvc",
    "5f17bf57378447975b6f1ef5afdcfad4D48L2JkvHk+PC9od+PsPg=="};
static const char *const paths[] = {"/web/book/chapter/e_0","/web/book/chapter/e_1","/web/book/chapter/e_3"};
static void chapter_replies(void)
{ size_t i; for(i=0;i<3;++i) add(paths[i],shards[i],NULL,"wr_vid=mock-account"); }

static void test_vectors(void)
{
    const char *ids[]={"1","1234567890123","book-alpha","0"};
    const char *expected[]={"c4c329b011c4ca4238a0201","ee732dd0775bcd15g027ba79","6ef420314626f6f6b2d616c706861774","cfc32da010cfcd208495488"};
    char digest[33],*out=NULL,*wire,*body=NULL;
    size_t i,n;
    CHECK(wr_mbedtls_md5_self_test(0)==0); CHECK(wr_mbedtls_base64_self_test(0)==0);
    wr_md5_hex("abc",3,digest); CHECK(!strcmp(digest,"900150983cd24fb0d6963f7d28e17f72"));
    for(i=0;i<4;++i) { CHECK(wr_encode_id(ids[i],&out)==WR_OK); CHECK(!strcmp(out,expected[i])); epd_app_free(out); }
    CHECK(wr_sign_query("b=test&c=1&ct=1700000000&pc=p&prevChapter=false&ps=s&r=49&sc=0&st=0")==0x7b303ba7u);
    CHECK(wr_content_request("book-alpha","1",1700000000,7,&body)==WR_OK);
    CHECK(strstr(body,"\"r\":\"49\"")); epd_app_free(body);
    wire=wr_dup("0VsaGbG8="); CHECK(wr_content_decode(wire,strlen(wire),&out,&n)==WR_OK);
    CHECK(n==5 && !strcmp(out,"hello")); epd_app_free(wire); epd_app_free(out);
    wire=wr_dup("0bGlBvSZSGbuKlUubmZB0d28K"); CHECK(wr_content_decode(wire,strlen(wire),&out,&n)==WR_OK);
    CHECK(!strcmp(out,"line one\nline two\n")); epd_app_free(wire); epd_app_free(out);
    wire=wr_dup("0YgLt5paH8J+i5A=="); CHECK(wr_content_decode(wire,strlen(wire),&out,&n)==WR_OK);
    CHECK(!strcmp(out,"\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80")); epd_app_free(wire); epd_app_free(out);
    { const char *bad[]={"0!","0A","0AB","0!!!!","0====","0AA=A","0AA"};
      for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { wire=wr_dup(bad[i]); CHECK(wr_content_decode(wire,strlen(wire),&out,&n)!=WR_OK); CHECK(!out && !n); epd_app_free(wire); } }
    { const char *part=NULL;
      CHECK(wr_fragment_check("d41d8cd98f00b204e9800998ecf8427e",32,&part,&n)==WR_ERR_INTEGRITY && !part && !n); }
    ++checks;
}
static void test_models(void)
{
    wr_book_t *books=NULL,*again=NULL, *many;
    wr_chapter_t *chapters=NULL,*ca=NULL,*cm;
    size_t n,m,size,i;
    char *json=NULL,*longtext=wr_alloc(8193);
    CHECK(longtext); memset(longtext,'x',8192); longtext[8192]=0;
    CHECK(wr_books_parse(shelf,sizeof(shelf)-1,&books,&n)==WR_OK && n==1);
    CHECK(wr_books_json(books,n,&json,&size)==WR_OK);
    CHECK(wr_books_parse(json,size,&again,&m)==WR_OK && m==n);
    CHECK(!strcmp(books[0].id,again[0].id)); wr_books_free(books,n); wr_books_free(again,m); epd_app_free(json);
    CHECK(wr_chapters_parse(catalog,sizeof(catalog)-1,&chapters,&n)==WR_OK && n==1);
    CHECK(!strcmp(chapters[0].uid,"1")); wr_chapters_free(chapters,n);
    many=wr_alloc(100*sizeof(*many)); cm=wr_alloc(700*sizeof(*cm)); CHECK(many && cm);
    memset(many,0,100*sizeof(*many)); memset(cm,0,700*sizeof(*cm));
    for(i=0;i<100;++i) { many[i].id=longtext; many[i].title=longtext; many[i].author="A"; many[i].format="txt"; many[i].cover=""; }
    CHECK(wr_books_json(many,100,&json,&size)==WR_OK); CHECK(wr_books_parse(json,size,&books,&n)==WR_OK && n==100);
    CHECK(strlen(books[99].title)==8192); wr_books_free(books,n); epd_app_free(json); epd_app_free(many);
    for(i=0;i<700;++i) { cm[i].uid="999999999999999999999"; cm[i].title="Chapter"; cm[i].index=INT64_MAX-(int64_t)i; }
    CHECK(wr_chapters_json(cm,700,&json,&size)==WR_OK); CHECK(wr_chapters_parse(json,size,&ca,&n)==WR_OK && n==700);
    CHECK(ca[699].index==INT64_MAX-699); wr_chapters_free(ca,n); epd_app_free(json); epd_app_free(cm); epd_app_free(longtext);
    CHECK(wr_books_parse("[]",2,&books,&n)==WR_OK && !books && n==0);
    CHECK(wr_chapters_parse("{\"chapters\":[]}",15,&ca,&n)==WR_OK && !ca && n==0);
    { const char *bad[]={"{\"books\":[", "[]garbage", "[{\"id\":\"a\\u0000b\"}]", "[{\"id\":9007199254740992}]", "[{\"id\":\"\\ud800\"}]"};
      for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { CHECK(wr_books_parse(bad[i],strlen(bad[i]),&books,&n)!=WR_OK); CHECK(!books && !n); } }
    CHECK(wr_books_parse("{\"errcode\":-2012}",17,&books,&n)==WR_ERR_AUTH);
    ++checks;
}
static void login_replies(void)
{
    add("/web/login/getuid","{\"uid\":\"mock-uid\"}","scan=mock-scan; Path=/; HttpOnly","");
    add("/web/login/getinfo","{\"vid\":\"mock-account\",\"skey\":\"mock-poll\",\"code\":\"mock-code\"}",NULL,"scan=mock-scan");
    add("/web/login/weblogin","{\"accessToken\":\"mock-token\",\"refreshToken\":\"mock-refresh\",\"nickname\":\"Mock Reader\",\"pf\":2}","wr_skey=mock-cookie; Path=/; HttpOnly","wr_skey=mock-poll");
    add("/web/login/session/init","{\"success\":true}","wr_vid=mock-account; Path=/","wr_skey=mock-cookie");
}
static void test_login(void)
{
    wr_client_t *c, *copy;
    char *url=NULL,*json=NULL,*json2=NULL;
    size_t n,m;
    reset_http(); login_replies();
    c=wr_client_create(cancel_cb,&stop); CHECK(c);
    CHECK(wr_client_login_begin(c,&url)==WR_OK); CHECK(!strcmp(url,"https://weread.qq.com/web/confirm?pf=2&uid=mock-uid")); epd_app_free(url);
    CHECK(wr_client_login_poll(c)==WR_OK && reply_index==4 && wr_client_authenticated(c));
    CHECK(!strcmp(wr_client_account_name(c),"Mock Reader"));
    CHECK(wr_client_session_export(c,&json,&n)==WR_OK);
    copy=wr_client_create(NULL,NULL); CHECK(copy);
    CHECK(wr_client_session_import(copy,json,n)==WR_OK && wr_client_authenticated(copy));
    CHECK(wr_client_session_export(copy,&json2,&m)==WR_OK && n==m && !strcmp(json,json2));
    CHECK(wr_client_session_import(copy,"{}",2)==WR_ERR_PROTOCOL && wr_client_authenticated(copy));
    epd_app_free(json); epd_app_free(json2); wr_client_destroy(copy); wr_client_destroy(c);
    CHECK(!wr_client_authenticated(NULL)); CHECK(!strcmp(wr_client_account_id(NULL),"")); CHECK(!strcmp(wr_client_account_name(NULL),"")); wr_client_destroy(NULL);
    reset_http(); login_replies(); replies[3].fail=true;
    c=wr_client_create(cancel_cb,&stop); CHECK(wr_client_login_begin(c,&url)==WR_OK); epd_app_free(url);
    CHECK(wr_client_login_poll(c)==WR_ERR_HTTP && !wr_client_authenticated(c));
    add("/web/login/session/init","{\"success\":true}",NULL,"wr_skey=mock-cookie");
    CHECK(wr_client_login_poll(c)==WR_OK && reply_index==5); wr_client_destroy(c);
    reset_http();
    add("/web/login/getuid","{\"uid\":\"mock\"}",NULL,"");
    add("/web/login/getinfo","{\"status\":\"waiting\"}",NULL,"");
    add("/web/login/getinfo","{\"logicCode\":\"QR_EXPIRED\"}",NULL,"");
    c=wr_client_create(NULL,NULL); CHECK(wr_client_login_begin(c,&url)==WR_OK); epd_app_free(url);
    CHECK(wr_client_login_poll(c)==WR_PENDING); CHECK(wr_client_login_poll(c)==WR_EXPIRED); wr_client_destroy(c);
    ++checks;
}
static void test_fetch_retry(void)
{
    wr_client_t *c;
    wr_book_t *books;
    wr_chapter_t *chapters;
    char *text=NULL;
    size_t n;
    reset_http(); c=logged_in();
    add("/web/shelf/sync","{\"errcode\":-2012}",NULL,"wr_skey=mock-old");
    add("/web/login/renewal","{\"succ\":true}","wr_skey=mock-new; Path=/","wr_skey=mock-old");
    add("/web/shelf/sync",shelf,NULL,"wr_skey=mock-new");
    CHECK(wr_client_shelf(c,&books,&n)==WR_OK && n==1 && reply_index==3); wr_books_free(books,n);
    reset_http();
    add("/web/book/chapterInfos","{\"errcode\":-2012}",NULL,"wr_skey=mock-new");
    add("/web/login/renewal","{\"success\":true}",NULL,"wr_skey=mock-new");
    add("/web/book/chapterInfos",catalog,NULL,"wr_skey=mock-new");
    CHECK(wr_client_catalog(c,"book-alpha",&chapters,&n)==WR_OK && n==1 && reply_index==3); wr_chapters_free(chapters,n);
    reset_http();
    add(paths[0],shards[0],NULL,NULL);
    add(paths[1],"{\"errcode\":-2012}",NULL,NULL);
    add("/web/login/renewal","{\"succ\":true}",NULL,NULL);
    chapter_replies();
    CHECK(wr_client_chapter(c,"book-alpha","1",&text,&n)==WR_OK && reply_index==6);
    CHECK(!strcmp(text,"<html><body><p>Test &amp; text.</p></body></html>")); epd_app_free(text);
    reset_http();
    add("/web/shelf/sync","{\"errcode\":-2012}",NULL,NULL);
    add("/web/login/renewal","{\"succ\":true}",NULL,NULL);
    add("/web/shelf/sync","{\"errcode\":-2012}",NULL,NULL);
    CHECK(wr_client_shelf(c,&books,&n)==WR_ERR_AUTH && reply_index==3 && !books && !n && !wr_client_authenticated(c));
    wr_client_destroy(c); ++checks;
}
static void test_failures(void)
{
    size_t i,n;
    char *out;
    wr_client_t *c;
    wr_book_t *books;
    for(i=0;i<3;++i)
    {
        reset_http(); c=logged_in(); chapter_replies(); replies[i].fail=true;
        CHECK(wr_client_chapter(c,"book-alpha","1",&out,&n)==WR_ERR_HTTP && !out && !n && reply_index==i+1);
        wr_client_destroy(c);
        reset_http(); c=logged_in(); chapter_replies(); replies[i].body="01234567890123456789012345678901bad";
        CHECK(wr_client_chapter(c,"book-alpha","1",&out,&n)==WR_ERR_INTEGRITY && !out && !n);
        wr_client_destroy(c);
    }
    reset_http(); c=logged_in(); add(paths[0],"{\"logicCode\":\"PERMISSION_DENIED\"}",NULL,NULL);
    CHECK(wr_client_chapter(c,"book-alpha","1",&out,&n)==WR_ERR_PERMISSION && !out && !n && reply_index==1);
    wr_client_destroy(c);
    reset_http(); c=logged_in(); add("/web/shelf/sync","{}",NULL,NULL); replies[0].status=403;
    CHECK(wr_client_shelf(c,&books,&n)==WR_ERR_PERMISSION && !books && !n && reply_index==1); wr_client_destroy(c);
    reset_http(); c=wr_client_create(NULL,NULL); add("/web/login/getuid",NULL,NULL,""); replies[0].fail=true;
    CHECK(wr_client_login_begin(c,&out)==WR_ERR_HTTP && !out); wr_client_destroy(c);
    ++checks;
}
static void test_cancellation(void)
{
    size_t i,n,m;
    char *before,*after,*url,*text;
    wr_client_t *c;
    wr_book_t *books;
    for(i=0;i<3;++i)
    {
        reset_http(); c=logged_in(); CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        chapter_replies(); replies[i].cancel_after=true; replies[i].set_cookie="wr_skey=cancelled-token; Path=/";
        CHECK(wr_client_chapter(c,"book-alpha","1",&text,&m)==WR_ERR_CANCELLED && !text && !m);
        CHECK(wr_client_session_export(c,&after,&m)==WR_OK && n==m && !strcmp(before,after));
        epd_app_free(before); epd_app_free(after); wr_client_destroy(c);
    }
    reset_http(); c=logged_in(); CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
    add("/web/shelf/sync","{\"errcode\":-2012}",NULL,NULL);
    add("/web/login/renewal","{\"succ\":true}","wr_skey=cancelled; Path=/",NULL); replies[1].cancel_after=true;
    CHECK(wr_client_shelf(c,&books,&m)==WR_ERR_CANCELLED && !books && !m);
    CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after));
    epd_app_free(before); epd_app_free(after); wr_client_destroy(c);
    for(i=1;i<=24;++i)
    {
        reset_http(); c=wr_client_create(cancel_cb,&stop); login_replies();
        CHECK(wr_client_login_begin(c,&url)==WR_OK); epd_app_free(url);
        CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        cancel_on_check=i; cancel_checks=0;
        { wr_result_t r=wr_client_login_poll(c);
          if(r==WR_ERR_CANCELLED) { CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after)); epd_app_free(after); }
          else CHECK(r==WR_OK); }
        epd_app_free(before); wr_client_destroy(c);
    }
    ++checks;
}
static void test_oom(void)
{
    long i;
    size_t n,base;
    wr_book_t *books;
    char *out;
    for(i=0;i<80;++i)
    {
        base=live; fail_after=i;
        { wr_result_t r=wr_books_parse(shelf,sizeof(shelf)-1,&books,&n);
          if(r==WR_OK) wr_books_free(books,n); else CHECK(!books && !n); }
        fail_after=-1; CHECK(live==base);
    }
    for(i=0;i<360;++i)
    {
        wr_client_t *c;
        reset_http(); c=wr_client_create(cancel_cb,&stop); CHECK(c); login_replies();
        CHECK(wr_client_login_begin(c,&out)==WR_OK); epd_app_free(out);
        fail_after=i;
        { wr_result_t r=wr_client_login_poll(c); CHECK(r==WR_OK || r<0); }
        fail_after=-1; wr_client_destroy(c); CHECK(live==0);
    }
    ++checks;
}
static void test_cookies(void)
{
    wr_client_t *c;
    wr_book_t *books;
    char *json,*after;
    size_t n,size;
    reset_http(); c=logged_in();
    add("/web/shelf/sync",shelf,"temp=one; Path=/web/shelf; Domain=.qq.com",NULL);
    replies[0].set_cookie2="ignored=foreign; Domain=example.invalid; Path=/";
    CHECK(wr_client_shelf(c,&books,&n)==WR_OK); wr_books_free(books,n);
    add("/web/shelf/sync",shelf,"temp=two; Path=/web/shelf","temp=one");
    replies[1].cookie_absent="ignored=";
    CHECK(wr_client_shelf(c,&books,&n)==WR_OK); wr_books_free(books,n);
    add("/web/login/renewal","{\"succ\":true}",NULL,"wr_skey=mock-old"); replies[2].cookie_absent="temp=";
    CHECK(wr_client_renew(c)==WR_OK);
    add("/web/shelf/sync",shelf,"temp=gone; Max-Age=0; Path=/web/shelf","temp=two");
    CHECK(wr_client_shelf(c,&books,&n)==WR_OK); wr_books_free(books,n);
    add("/web/shelf/sync",shelf,NULL,NULL); replies[4].cookie_absent="temp=";
    CHECK(wr_client_shelf(c,&books,&n)==WR_OK); wr_books_free(books,n);
    add("/web/login/renewal","{\"succ\":true}","wr_skey=; Max-Age=0; Path=/",NULL);
    CHECK(wr_client_renew(c)==WR_ERR_AUTH && !wr_client_authenticated(c));
    add("/web/login/renewal","{\"succ\":true}",NULL,NULL); replies[6].cookie_absent="wr_skey=";
    CHECK(wr_client_renew(c)==WR_ERR_AUTH);
    wr_client_destroy(c);
    reset_http(); c=logged_in();
    CHECK(wr_client_session_export(c,&json,&size)==WR_OK);
    add("/web/login/renewal","{\"succ\":true}","wr_vid=another-account; Path=/",NULL);
    CHECK(wr_client_renew(c)==WR_ERR_AUTH && !strcmp(wr_client_account_id(c),"mock-account"));
    CHECK(wr_client_session_export(c,&after,&n)==WR_OK && !strstr(after,"another-account"));
    epd_app_free(json); epd_app_free(after); wr_client_destroy(c);
    ++checks;
}
static void test_long_session(void)
{
    wr_client_t *c,*other;
    wr_json *j=j_object();
    char *longtext=wr_alloc(12001),*json,*out;
    size_t n,m;
    CHECK(j && longtext); memset(longtext,'a',12000); longtext[12000]=0;
    CHECK(j_number(j,"version",1) && j_bool(j,"authenticated",true) && j_string(j,"vid",longtext) &&
          j_string(j,"skey",longtext) && j_string(j,"nickname",longtext));
    CHECK(wr_json_print(j,&json,&n)==WR_OK); j_delete(j);
    c=wr_client_create(NULL,NULL); other=wr_client_create(NULL,NULL); CHECK(c && other);
    CHECK(wr_client_session_import(c,json,n)==WR_OK); CHECK(strlen(wr_client_account_name(c))==12000);
    CHECK(wr_client_session_export(c,&out,&m)==WR_OK && m>36000);
    CHECK(wr_client_session_import(other,out,m)==WR_OK && !strcmp(wr_client_account_id(other),longtext));
    epd_app_free(json); epd_app_free(out); epd_app_free(longtext); wr_client_destroy(c); wr_client_destroy(other);
    ++checks;
}
static void test_import_and_fetch_oom(void)
{
    long i;
    size_t n,m,base;
    wr_book_t *books;
    wr_client_t *c;
    char *before,*after;
    for(i=0;i<80;++i)
    {
        reset_http(); c=logged_in(); CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        base=live; fail_after=i;
        { wr_result_t r=wr_client_session_import(c,session,sizeof(session)-1);
          fail_after=-1;
          if(r!=WR_OK) CHECK(live==base);
          CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after)); }
        epd_app_free(before); epd_app_free(after); wr_client_destroy(c); CHECK(!live);
    }
    for(i=0;i<130;++i)
    {
        reset_http(); c=logged_in(); add("/web/shelf/sync",shelf,"extra=mock; Path=/",NULL);
        fail_after=i;
        { wr_result_t r=wr_client_shelf(c,&books,&n);
          if(r==WR_OK) wr_books_free(books,n); else CHECK(!books && !n); }
        fail_after=-1; wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_integer_ranges(void)
{
    const char *valid[]={"0","-0","9223372036854775807","-9223372036854775808","000123"};
    int64_t expected[]={0,0,INT64_MAX,INT64_MIN,123};
    const char *bad[]={"9223372036854775808","-9223372036854775809","18446744073709551616","1.0","1e3","+1"," 1","1 ","-",""};
    wr_json value;
    int64_t result;
    size_t i;
    memset(&value,0,sizeof(value)); value.type=wr_cjson_String;
    for(i=0;i<sizeof(valid)/sizeof(valid[0]);++i)
    { value.valuestring=(char *)valid[i]; CHECK(wr_json_int(&value,&result)==WR_OK && result==expected[i]); }
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
    { value.valuestring=(char *)bad[i]; CHECK(wr_json_int(&value,&result)==WR_ERR_PROTOCOL); }
    ++checks;
}
static void test_revisions(void)
{
    const char source[]="{\"books\":[{\"bookInfo\":{\"bookId\":\"a\",\"version\":12,\"updateTime\":1700000000}},"
        "{\"id\":\"b\",\"bookVersion\":\"9223372036854775807\",\"updateTime\":\"-9223372036854775808\"},"
        "{\"id\":\"c\",\"version\":0,\"bookVersion\":7},{\"id\":\"d\"}]}";
    const char chapters[]="{\"chapterInfos\":[{\"chapterUid\":\"1\",\"updateTime\":1700000001},"
        "{\"chapterInfo\":{\"uid\":\"2\",\"update_time\":\"9223372036854775807\"}},"
        "{\"uid\":\"3\",\"updateTime\":\"-9223372036854775808\"},{\"uid\":\"4\"}]}";
    const char *bad[]={"\"9223372036854775808\"","9007199254740992","1.5","true","null","\"1e3\""};
    wr_book_t *books,*copy;
    wr_chapter_t *catalogs,*ccopy;
    char *json, invalid[160];
    size_t n,m,size,i;
    CHECK(wr_books_parse(source,sizeof(source)-1,&books,&n)==WR_OK && n==4);
    CHECK(books[0].version==12 && books[0].update_time==1700000000);
    CHECK(books[1].version==INT64_MAX && books[1].update_time==INT64_MIN);
    CHECK(books[2].version==7 && books[2].update_time==0);
    CHECK(!books[3].version && !books[3].update_time);
    CHECK(wr_books_json(books,n,&json,&size)==WR_OK);
    CHECK(wr_books_parse(json,size,&copy,&m)==WR_OK && m==n);
    for(i=0;i<n;++i) CHECK(books[i].version==copy[i].version && books[i].update_time==copy[i].update_time);
    wr_books_free(books,n); wr_books_free(copy,m); epd_app_free(json);
    CHECK(wr_chapters_parse(chapters,sizeof(chapters)-1,&catalogs,&n)==WR_OK && n==4);
    CHECK(catalogs[0].update_time==1700000001 && catalogs[1].update_time==INT64_MAX);
    CHECK(catalogs[2].update_time==INT64_MIN && !catalogs[3].update_time);
    CHECK(wr_chapters_json(catalogs,n,&json,&size)==WR_OK);
    CHECK(wr_chapters_parse(json,size,&ccopy,&m)==WR_OK && m==n);
    for(i=0;i<n;++i) CHECK(catalogs[i].update_time==ccopy[i].update_time);
    wr_chapters_free(catalogs,n); wr_chapters_free(ccopy,m); epd_app_free(json);
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
    {
        snprintf(invalid,sizeof(invalid),"[{\"id\":\"a\",\"version\":%s}]",bad[i]);
        CHECK(wr_books_parse(invalid,strlen(invalid),&books,&n)==WR_ERR_PROTOCOL && !books && !n);
        snprintf(invalid,sizeof(invalid),"[{\"id\":\"a\",\"updateTime\":%s}]",bad[i]);
        CHECK(wr_books_parse(invalid,strlen(invalid),&books,&n)==WR_ERR_PROTOCOL && !books && !n);
        snprintf(invalid,sizeof(invalid),"[{\"uid\":\"1\",\"updateTime\":%s}]",bad[i]);
        CHECK(wr_chapters_parse(invalid,strlen(invalid),&catalogs,&n)==WR_ERR_PROTOCOL && !catalogs && !n);
    }
    ++checks;
}
#include "test_otp.c"
int main(void)
{
    test_vectors(); test_models(); test_login(); test_fetch_retry(); test_failures(); test_cancellation(); test_oom(); test_cookies(); test_long_session(); test_import_and_fetch_oom(); test_integer_ranges();
    test_revisions();
    test_otp_success(); test_otp_init_retry(); test_otp_terminal(); test_otp_cleanup();
    test_otp_failures(); test_otp_cancellation(); test_otp_oom();
    test_otp_init_faults();
    CHECK(live==0);
    printf("PASS: %zu test groups; %zu checked allocations; no live allocations; HTTP is fully mocked.\n",checks,allocations);
    return 0;
}
