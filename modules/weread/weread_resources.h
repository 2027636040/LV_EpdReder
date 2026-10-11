#ifndef WEREAD_RESOURCES_H
#define WEREAD_RESOURCES_H
#include "weread_service.h"
int weread_resource_open(void *user, const char *base, const char *reference, book_stream_t *stream);
/* Synchronous worker call. Always checks weread_unavailable(). When provided,
 * cancel replaces generation checks; otherwise content->generation is checked.
 * Neither content nor callback/user
 * is retained after return. The returned stream owns its bytes independently.
 * content supplies account/book cache identities only. No auth cookies. */
int weread_resource_open_ex(const weread_result_t *content, const char *base,
    const char *reference, book_stream_t *stream, bool (*cancel)(void *), void *cancel_user);
#endif
