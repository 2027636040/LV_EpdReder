#ifndef WR_INTERNAL_H
#define WR_INTERNAL_H
#include "wr_client.h"
#include "epd_memory.h"
#include "private/wr_cjson.h"
#include <string.h>

typedef wr_cjson wr_json;
#define j_get wr_cjson_GetObjectItemCaseSensitive
#define j_object wr_cjson_CreateObject
#define j_array wr_cjson_CreateArray
#define j_delete wr_cjson_Delete
#define j_string wr_cjson_AddStringToObject
#define j_number wr_cjson_AddNumberToObject
#define j_bool wr_cjson_AddBoolToObject
#define j_is_string wr_cjson_IsString
#define j_is_number wr_cjson_IsNumber
#define j_is_object wr_cjson_IsObject
#define j_is_array wr_cjson_IsArray

void *wr_alloc(size_t size);
char *wr_dup_n(const char *text, size_t size);
char *wr_dup(const char *text);
char *wr_format(const char *format, ...);
char *wr_url_encode(const char *text);
bool wr_ascii_equal(const char *a, const char *b);
wr_result_t wr_json_parse(const char *text, size_t size, wr_json **out);
wr_result_t wr_json_print(const wr_json *root, char **out, size_t *size);
wr_result_t wr_json_text(const wr_json *value, bool required, char **out);
wr_result_t wr_json_int(const wr_json *value, int64_t *out);
wr_result_t wr_remote_result(const wr_json *root);
bool wr_json_true(const wr_json *value);
const wr_json *wr_payload(const wr_json *root);

void wr_md5_hex(const void *data, size_t size, char out[33]);
wr_result_t wr_encode_id(const char *value, char **out);
uint32_t wr_sign_query(const char *query);
wr_result_t wr_content_request(const char *book, const char *chapter,
                                uint32_t now, uint32_t nonce, char **body);
wr_result_t wr_fragment_check(const char *data, size_t size,
                              const char **payload, size_t *payload_size);
wr_result_t wr_content_decode(char *joined, size_t size, char **out, size_t *out_size);
#endif
