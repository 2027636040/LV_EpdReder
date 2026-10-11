/* Included by test_client.c to share its mock HTTP and checked allocator. */
static const char otp_parameters[] =
    "{\"vid\":\"mock-account\",\"skey\":\"mock-poll\",\"uid\":\"mock-uid\","
    "\"fp\":\"b5b10a1f4dd84a0e9c5f0a7b3c6d2e11\",\"isAutoLogout\":0,\"code\":\"mock-code\"}";
static const char otp_request[] =
    "{\"vid\":\"mock-account\",\"skey\":\"mock-poll\",\"uid\":\"mock-uid\","
    "\"fp\":\"b5b10a1f4dd84a0e9c5f0a7b3c6d2e11\",\"isAutoLogout\":0,\"code\":\"mock-code\",\"otp\":\"0123\"}";
static const char otp_tokens[] =
    "{\"accessToken\":\"mock-verified\",\"refreshToken\":\"mock-refresh\",\"nickname\":\"Verified Reader\",\"pf\":2}";

static void otp_login_replies(void)
{
    add("/web/login/getuid","{\"uid\":\"mock-uid\"}","scan=mock-scan; Path=/","");
    add("/web/login/getinfo","{\"vid\":\"mock-account\",\"skey\":\"mock-poll\",\"code\":\"mock-code\"}",NULL,"scan=mock-scan");
    add("/web/login/weblogin","{\"needRemoteLoginVerify\":true}","gate=mock-gate; Path=/","wr_skey=mock-poll");
    replies[reply_count-1].request_body=otp_parameters;
}
static wr_client_t *otp_begin(void)
{
    char *url;
    wr_client_t *c;
    reset_http(); otp_login_replies();
    c=wr_client_create(cancel_cb,&stop); CHECK(c);
    CHECK(wr_client_login_begin(c,&url)==WR_OK); epd_app_free(url);
    return c;
}
static wr_client_t *otp_waiting(void)
{
    wr_client_t *c=otp_begin();
    CHECK(wr_client_login_poll(c)==WR_VERIFY_REQUIRED && reply_index==3);
    CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c));
    return c;
}
static void otp_verify_reply(const char *body, const char *set_cookie, const char *cookie)
{
    add("/web/login/weblogin",body,set_cookie,cookie);
    replies[reply_count-1].request_body=otp_request;
}
static void otp_init_reply(bool ok)
{
    add("/web/login/session/init",ok ? "{\"success\":true}" : "{\"success\":false}",NULL,NULL);
    replies[reply_count-1].request_body=
        "{\"vid\":\"mock-account\",\"skey\":\"mock-verified\",\"rt\":\"mock-refresh\",\"pf\":\"2\",\"ql\":false}";
}
static bool otp_export_check(wr_client_t *c)
{
    static const char *const allowed[]={"version","authenticated","initPending","vid","skey",
        "accessToken","refreshToken","nickname","pf","cookies"};
    char *text;
    size_t size,i;
    wr_json *root;
    const wr_json *field;
    bool pending;
    CHECK(wr_client_session_export(c,&text,&size)==WR_OK);
    CHECK(!strstr(text,"mock-code") && !strstr(text,"mock-uid") && !strstr(text,"0123"));
    CHECK(wr_json_parse(text,size,&root)==WR_OK);
    CHECK(!j_get(root,"otp") && !j_get(root,"login_pending") && !j_get(root,"challenge") && !j_get(root,"loginUid"));
    CHECK(!j_get(root,"code") && !j_get(root,"uid") && !j_get(root,"fp") && !j_get(root,"isAutoLogout"));
    for(field=root->child;field;field=field->next)
    {
        for(i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i) if(!strcmp(field->string,allowed[i])) break;
        CHECK(i<sizeof(allowed)/sizeof(allowed[0]));
    }
    pending=wr_json_true(j_get(root,"initPending"));
    j_delete(root); epd_app_free(text);
    return pending;
}
static void otp_finish_retry(wr_client_t *c)
{
    bool init=otp_export_check(c);
    reset_http();
    if (!init) otp_verify_reply(otp_tokens,NULL,NULL);
    otp_init_reply(true);
    CHECK(wr_client_login_verify(c,init ? NULL : "0123")==WR_OK);
    CHECK(reply_index==(init ? 1u : 2u));
    CHECK(wr_client_authenticated(c) && !wr_client_login_waiting_otp(c));
}
static void test_otp_success(void)
{
    const char *bad[]={NULL,"","0","012","01234","123456","12 3"," 123","+123","12a3","123\n","\xef\xbc\x90\xef\xbc\x91\xef\xbc\x92\xef\xbc\x93"};
    size_t i, base, n, m;
    char *before,*after;
    wr_client_t *c=otp_waiting(),*copy;
    CHECK(WR_VERIFY_REQUIRED==3 && WR_VERIFY_FAILED==4 && WR_VERIFY_EXPIRED==5 && WR_VERIFY_LIMIT==6);
    CHECK(!wr_client_login_waiting_otp(NULL) && wr_client_login_verify(NULL,"0123")==WR_ERR_ARGUMENT);
    CHECK(!otp_export_check(c));
    CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
    base=allocations; fail_after=0;
    for(i=0;i<4;++i) CHECK(wr_client_login_poll(c)==WR_VERIFY_REQUIRED);
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) CHECK(wr_client_login_verify(c,bad[i])==WR_ERR_ARGUMENT);
    fail_after=-1; CHECK(allocations==base && reply_index==3);
    stop=true; CHECK(wr_client_login_poll(c)==WR_ERR_CANCELLED && wr_client_login_waiting_otp(c)); stop=false;
    CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after)); epd_app_free(after);
    copy=wr_client_create(NULL,NULL); CHECK(copy);
    CHECK(wr_client_session_import(copy,before,n)==WR_OK && !wr_client_login_waiting_otp(copy));
    CHECK(wr_client_login_poll(copy)==WR_ERR_ARGUMENT); wr_client_destroy(copy); epd_app_free(before);
    otp_verify_reply("{\"errcode\":-999,\"data\":{\"isVerifyFailed\":true,\"needRemoteLoginVerify\":true}}",
        "wr_skey=mock-rotated; Path=/","gate=mock-gate");
    CHECK(wr_client_login_verify(c,"0123")==WR_VERIFY_FAILED && wr_client_login_waiting_otp(c));
    CHECK(!wr_client_authenticated(c) && !otp_export_check(c));
    otp_verify_reply("{\"needRemoteLoginVerify\":true}",NULL,"wr_skey=mock-rotated");
    CHECK(wr_client_login_verify(c,"0123")==WR_VERIFY_REQUIRED);
    /* The retained body still contains the original skey; the Cookie is newer. */
    otp_verify_reply(otp_tokens,"wr_vid=mock-account; Path=/","wr_skey=mock-rotated");
    otp_init_reply(true);
    CHECK(wr_client_login_verify(c,"0123")==WR_OK && reply_index==7);
    CHECK(wr_client_authenticated(c) && !wr_client_login_waiting_otp(c));
    CHECK(!strcmp(wr_client_account_name(c),"Verified Reader"));
    CHECK(wr_client_login_verify(c,"0123")==WR_ERR_ARGUMENT && wr_client_login_poll(c)==WR_OK);
    CHECK(!otp_export_check(c)); wr_client_destroy(c); CHECK(!live); ++checks;
}
static void test_otp_init_retry(void)
{
    wr_client_t *c=otp_waiting();
    otp_verify_reply(otp_tokens,NULL,"gate=mock-gate"); otp_init_reply(true); replies[4].fail=true;
    CHECK(wr_client_login_verify(c,"0123")==WR_ERR_HTTP && reply_index==5);
    CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c) && otp_export_check(c));
    CHECK(wr_client_login_poll(c)==WR_VERIFY_REQUIRED && reply_index==5);
    otp_init_reply(false);
    CHECK(wr_client_login_verify(c,"ignored")==WR_ERR_REMOTE && reply_index==6);
    CHECK(wr_client_login_waiting_otp(c));
    otp_init_reply(true);
    CHECK(wr_client_login_verify(c,NULL)==WR_OK && reply_index==7);
    CHECK(!wr_client_login_waiting_otp(c) && wr_client_authenticated(c));
    wr_client_destroy(c); CHECK(!live); ++checks;
}
static void test_otp_terminal(void)
{
    const char *body[]={"{\"verifyCodeExpired\":true,\"needRemoteLoginVerify\":true}",
        "{\"error\":\"blocked\",\"data\":{\"reachMaxAttemptTimes\":true,\"isVerifyFailed\":true}}"};
    wr_result_t expected[]={WR_VERIFY_EXPIRED,WR_VERIFY_LIMIT};
    unsigned i,initial;
    for(initial=0;initial<2;++initial) for(i=0;i<2;++i)
    {
        wr_client_t *c=initial ? otp_begin() : otp_waiting();
        if(initial) replies[2].body=body[i]; else otp_verify_reply(body[i],NULL,NULL);
        CHECK((initial ? wr_client_login_poll(c) : wr_client_login_verify(c,"0123"))==expected[i]);
        CHECK(!wr_client_login_waiting_otp(c) && !wr_client_authenticated(c));
        CHECK(wr_client_login_poll(c)==WR_ERR_ARGUMENT && wr_client_login_verify(c,"0123")==WR_ERR_ARGUMENT);
        CHECK(!otp_export_check(c)); wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_otp_cleanup(void)
{
    wr_client_t *c;
    char *url;
    unsigned mode;
    for(mode=0;mode<5;++mode)
    {
        c=otp_waiting();
        if(mode<2)
        {
            add("/web/login/getuid","{\"uid\":\"new-uid\"}",NULL,"");
            replies[3].fail=mode==1;
            CHECK(wr_client_login_begin(c,&url)==(mode ? WR_ERR_HTTP : WR_OK));
            CHECK(!wr_client_login_waiting_otp(c) && !wr_client_authenticated(c));
            if(!mode)
            {
                CHECK(strstr(url,"new-uid")); epd_app_free(url);
                add("/web/login/getinfo","{\"status\":\"waiting\"}",NULL,"");
                replies[4].request_body="{\"uid\":\"new-uid\"}";
                CHECK(wr_client_login_poll(c)==WR_PENDING);
            }
            else CHECK(!url && wr_client_login_poll(c)==WR_ERR_ARGUMENT);
        }
        else if(mode==2)
        { wr_client_logout(c); CHECK(!wr_client_login_waiting_otp(c) && wr_client_login_poll(c)==WR_ERR_ARGUMENT); }
        else if(mode==3)
        {
            CHECK(wr_client_session_import(c,session,sizeof(session)-1)==WR_OK);
            CHECK(!wr_client_login_waiting_otp(c) && wr_client_authenticated(c));
        }
        else
        {
            CHECK(wr_client_session_import(c,"{}",2)==WR_ERR_PROTOCOL && wr_client_login_waiting_otp(c));
            otp_finish_retry(c);
        }
        wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_otp_failures(void)
{
    unsigned i;
    for(i=0;i<5;++i)
    {
        wr_client_t *c=otp_waiting();
        otp_verify_reply(i==0 ? "{bad" : i==1 ? "{\"success\":true}" : i==2 ?
            "{\"accessToken\":\"partial-token\",\"nickname\":{}}" : otp_tokens,NULL,NULL);
        if(i==3) replies[3].fail=true;
        if(i==4) replies[3].status=403;
        CHECK(wr_client_login_verify(c,"0123")<0);
        CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c) && !otp_export_check(c));
        CHECK(!strcmp(wr_client_account_name(c),""));
        otp_finish_retry(c); wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_otp_cancellation(void)
{
    unsigned i,kind;
    char *before,*after,*url;
    size_t n,m;
    wr_client_t *c;
    for(kind=0;kind<4;++kind)
    {
        c=kind==0 ? otp_begin() : otp_waiting();
        CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        if(kind==0) replies[2].cancel_after=true;
        else if(kind==1)
        { add("/web/login/getuid","{\"uid\":\"new-uid\"}","other=cancelled; Path=/",NULL); replies[3].cancel_after=true; }
        else
        { otp_verify_reply(kind==2 ? "{\"verifyCodeExpired\":true}" : "{\"reachMaxAttemptTimes\":true}",NULL,NULL); replies[3].cancel_after=true; }
        CHECK((kind==0 ? wr_client_login_poll(c) : kind==1 ? wr_client_login_begin(c,&url) :
            wr_client_login_verify(c,"0123"))==WR_ERR_CANCELLED);
        CHECK(wr_client_login_waiting_otp(c)==(kind!=0));
        CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after));
        epd_app_free(before); epd_app_free(after);
        if(kind) otp_finish_retry(c);
        wr_client_destroy(c); CHECK(!live);
    }
    for(i=1;i<=40;++i)
    {
        c=otp_waiting(); CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        otp_verify_reply(otp_tokens,"wr_skey=mock-cancelled; Path=/",NULL); otp_init_reply(true);
        cancel_on_check=i; cancel_checks=0;
        wr_result_t r=wr_client_login_verify(c,"0123");
        if(r==WR_ERR_CANCELLED)
        {
            CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c));
            CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after)); epd_app_free(after);
            otp_finish_retry(c);
        }
        else CHECK(r==WR_OK && !wr_client_login_waiting_otp(c));
        epd_app_free(before); wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_otp_oom(void)
{
    long i;
    unsigned success=0, kind;
    for(i=0;i<240;++i)
    {
        wr_client_t *c=otp_begin(); fail_after=i;
        wr_result_t r=wr_client_login_poll(c); fail_after=-1;
        CHECK(r==WR_VERIFY_REQUIRED || r<0);
        CHECK(wr_client_login_waiting_otp(c)==(r==WR_VERIFY_REQUIRED));
        if(r==WR_VERIFY_REQUIRED) ++success;
        wr_client_destroy(c); CHECK(!live);
    }
    CHECK(success); success=0;
    for(i=0;i<400;++i)
    {
        wr_client_t *c=otp_waiting();
        otp_verify_reply(otp_tokens,NULL,NULL); otp_init_reply(true); fail_after=i;
        wr_result_t r=wr_client_login_verify(c,"0123"); fail_after=-1;
        CHECK(r==WR_OK || r<0);
        if(r==WR_OK) { CHECK(!wr_client_login_waiting_otp(c) && wr_client_authenticated(c)); ++success; }
        else
        { CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c)); otp_finish_retry(c); }
        wr_client_destroy(c); CHECK(!live);
    }
    CHECK(success);
    for(kind=0;kind<2;++kind) for(i=0;i<160;++i)
    {
        wr_client_t *c=otp_waiting();
        wr_result_t expected=kind ? WR_VERIFY_LIMIT : WR_VERIFY_EXPIRED;
        otp_verify_reply(kind ? "{\"reachMaxAttemptTimes\":true}" : "{\"verifyCodeExpired\":true}",NULL,NULL);
        fail_after=i; wr_result_t r=wr_client_login_verify(c,"0123"); fail_after=-1;
        CHECK(r==expected || r<0);
        CHECK(wr_client_login_waiting_otp(c)==(r!=expected));
        if(r==expected) CHECK(wr_client_login_poll(c)==WR_ERR_ARGUMENT); else otp_finish_retry(c);
        wr_client_destroy(c); CHECK(!live);
    }
    ++checks;
}
static void test_otp_init_faults(void)
{
    unsigned kind, successes=0;
    long i;
    for(kind=0;kind<2;++kind) for(i=0;i<(kind ? 200 : 25);++i)
    {
        char *before,*after;
        size_t n,m;
        wr_client_t *c=otp_waiting();
        otp_verify_reply(otp_tokens,NULL,NULL); otp_init_reply(true); replies[4].fail=true;
        CHECK(wr_client_login_verify(c,"0123")==WR_ERR_HTTP && otp_export_check(c));
        CHECK(wr_client_session_export(c,&before,&n)==WR_OK);
        reset_http(); otp_init_reply(true);
        if(kind) fail_after=i; else { cancel_on_check=(size_t)i+1; cancel_checks=0; }
        wr_result_t r=wr_client_login_verify(c,NULL);
        fail_after=-1; cancel_on_check=0;
        CHECK(r==WR_OK || r<0);
        if(r==WR_OK) { CHECK(!wr_client_login_waiting_otp(c) && wr_client_authenticated(c)); ++successes; }
        else
        {
            CHECK(wr_client_login_waiting_otp(c) && !wr_client_authenticated(c) && otp_export_check(c));
            CHECK(wr_client_session_export(c,&after,&m)==WR_OK && !strcmp(before,after)); epd_app_free(after);
            otp_finish_retry(c);
        }
        epd_app_free(before); wr_client_destroy(c); CHECK(!live);
    }
    CHECK(successes); ++checks;
}
