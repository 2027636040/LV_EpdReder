/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <string.h>
#include <tls_client.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>

#define DBG_TAG "tls.cert"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

int __real_mbedtls_x509_crt_parse(mbedtls_x509_crt *chain,
                                const unsigned char *buffer, size_t length);
int __real_mbedtls_client_context(MbedTLSSession *session);
int __real_mbedtls_md(const mbedtls_md_info_t *info, const unsigned char *input,
                     size_t length, unsigned char *output);
int __real_mbedtls_pk_verify_ext(mbedtls_pk_type_t type, const void *options,
                               mbedtls_pk_context *key, mbedtls_md_type_t hash_type,
                               const unsigned char *hash, size_t hash_length,
                               const unsigned char *signature, size_t signature_length);

static rt_uint32_t ram0_free(void)
{
    rt_uint32_t total, used, peak;
    rt_memory_info(&total, &used, &peak);
    return total - used;
}

static void common_name(const mbedtls_x509_name *name, char *text, size_t capacity)
{
    rt_strncpy(text, "<no CN>", capacity);
    for (; name; name = name->next)
    {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &name->oid) == 0)
        {
            size_t length = name->val.len < capacity - 1 ? name->val.len : capacity - 1;
            rt_memcpy(text, name->val.p, length);
            text[length] = '\0';
            return;
        }
    }
}

static int certificate_verify(void *context, mbedtls_x509_crt *certificate,
                              int depth, uint32_t *flags)
{
    char name[64];
    (void)context;
    LOG_I("chain depth=%d flags=%08x key=%p bits=%u", depth, (unsigned)*flags,
          (void *)&certificate->pk, (unsigned)mbedtls_pk_get_bitlen(&certificate->pk));
    common_name(&certificate->subject, name, sizeof(name));
    LOG_I("  subject=%s", name);
    common_name(&certificate->issuer, name, sizeof(name));
    LOG_I("  issuer=%s", name);
    /* Keep every verification flag; returning zero only continues the walk. */
    return 0;
}

int __wrap_mbedtls_x509_crt_parse(mbedtls_x509_crt *chain,
                                const unsigned char *buffer, size_t length)
{
    rt_uint32_t before = ram0_free();
    int ret = __real_mbedtls_x509_crt_parse(chain, buffer, length);
    rt_uint32_t after = ram0_free();
    unsigned count = 0;
    for (mbedtls_x509_crt *certificate = chain; certificate; certificate = certificate->next)
    {
        char name[64];
        if (!certificate->raw.p) continue;
        common_name(&certificate->subject, name, sizeof(name));
        LOG_I("CA[%u] key=%p cn=%s", ++count, (void *)&certificate->pk, name);
    }
    LOG_I("CA parse ret=%d count=%u bytes=%u", ret, count, (unsigned)length);
    LOG_I("CA RAM0 free=%u -> %u", (unsigned)before, (unsigned)after);
    return ret;
}

int __wrap_mbedtls_client_context(MbedTLSSession *session)
{
    int ret = __real_mbedtls_client_context(session);
    LOG_I("context ret=%d RAM0 free=%u", ret, (unsigned)ram0_free());
    if (ret == 0)
    {
        LOG_I("context auth=%u CA=%p session_CA=%p", (unsigned)session->conf.authmode,
              (void *)session->conf.ca_chain, (void *)&session->cacert);
        mbedtls_ssl_conf_verify(&session->conf, certificate_verify, RT_NULL);
    }
    return ret;
}

int __wrap_mbedtls_md(const mbedtls_md_info_t *info, const unsigned char *input,
                     size_t length, unsigned char *output)
{
    rt_uint32_t before = ram0_free();
    int ret = __real_mbedtls_md(info, input, length, output);
    if (ret != 0)
    {
        LOG_E("digest ret=%d type=%d bytes=%u", ret, (int)mbedtls_md_get_type(info),
              (unsigned)length);
        LOG_E("digest RAM0 free=%u -> %u", (unsigned)before, (unsigned)ram0_free());
    }
    return ret;
}

int __wrap_mbedtls_pk_verify_ext(mbedtls_pk_type_t type, const void *options,
                               mbedtls_pk_context *key, mbedtls_md_type_t hash_type,
                               const unsigned char *hash, size_t hash_length,
                               const unsigned char *signature, size_t signature_length)
{
    rt_uint32_t before = ram0_free();
    int ret = __real_mbedtls_pk_verify_ext(type, options, key, hash_type, hash,
                                        hash_length, signature, signature_length);
    rt_uint32_t after = ram0_free();
    LOG_I("signature key=%p ret=%d bits=%u", (void *)key, ret,
          (unsigned)mbedtls_pk_get_bitlen(key));
    LOG_I("signature type=%d hash=%d bytes=%u", (int)type, (int)hash_type,
          (unsigned)signature_length);
    LOG_I("signature RAM0 free=%u -> %u", (unsigned)before, (unsigned)after);
    return ret;
}
