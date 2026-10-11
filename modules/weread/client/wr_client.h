#ifndef WR_CLIENT_H
#define WR_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wr_client wr_client_t;
typedef bool (*wr_cancel_fn)(void *user);

typedef enum
{
    WR_OK = 0,
    WR_PENDING = 1,
    WR_EXPIRED = 2,
    WR_VERIFY_REQUIRED = 3,
    WR_VERIFY_FAILED = 4,
    WR_VERIFY_EXPIRED = 5,
    WR_VERIFY_LIMIT = 6,
    WR_ERR_ARGUMENT = -1,
    WR_ERR_MEMORY = -2,
    WR_ERR_CANCELLED = -3,
    WR_ERR_HTTP = -4,
    WR_ERR_PROTOCOL = -5,
    WR_ERR_AUTH = -6,
    WR_ERR_PERMISSION = -7,
    WR_ERR_REMOTE = -8,
    WR_ERR_INTEGRITY = -9
} wr_result_t;

typedef struct
{
    char *id;
    char *title;
    char *author;
    char *format;
    char *cover;
    int64_t version;
    int64_t update_time;
} wr_book_t;

typedef struct
{
    char *uid;
    char *title;
    int64_t index;
    int64_t update_time;
} wr_chapter_t;

/* The caller serializes all calls on a client, including getters and destroy.
 * The callback may be NULL and runs synchronously during network operations;
 * it must not reenter this client. No threads, files or UI are created.
 * Cancelled operations discard all session/account/cookie changes and outputs.
 * Owned strings are NUL terminated and released with epd_app_free(). Output
 * lengths exclude the terminator. On failure owned outputs are NULL/0. */
wr_client_t *wr_client_create(wr_cancel_fn cancel, void *user);
void wr_client_destroy(wr_client_t *client);
void wr_client_set_cancel(wr_client_t *client, wr_cancel_fn cancel, void *user);
void wr_client_logout(wr_client_t *client);

/* begin replaces an earlier login attempt and returns an owned QR URL.
 * poll performs one getinfo request, then weblogin and session/init if ready.
 * A failed session/init can be retried by poll without replaying weblogin,
 * except during OTP login: poll returns WR_VERIFY_REQUIRED without I/O while
 * waiting_otp is true; login_verify retries that attempt's session/init.
 * Only WR_OK means the complete login sequence succeeded. */
wr_result_t wr_client_login_begin(wr_client_t *client, char **qr_url);
wr_result_t wr_client_login_poll(wr_client_t *client);
/* NULL returns false. True means the in-memory OTP challenge is retryable,
 * including when OTP succeeded but session/init still needs a retry. */
bool wr_client_login_waiting_otp(const wr_client_t *client);
/* Sends exactly four ASCII digits as a string, preserving leading zeroes.
 * If session/init is pending, retries only init and ignores otp (may be NULL).
 * REQUIRED/FAILED retain the challenge; EXPIRED/LIMIT clear it and the QR UID.
 * A successful init, a new begin, or logout clears the challenge. Neither the
 * challenge parameters nor OTP are part of exported session JSON. */
wr_result_t wr_client_login_verify(wr_client_t *client, const char *otp);
wr_result_t wr_client_renew(wr_client_t *client);
/* NULL is accepted: authenticated returns false; account getters return "". */
bool wr_client_authenticated(const wr_client_t *client);
/* Borrowed strings valid until the next mutating call or destroy. An absent
 * field is an empty string. Account fields are never silently truncated. */
const char *wr_client_account_id(const wr_client_t *client);
const char *wr_client_account_name(const wr_client_t *client);

/* Versioned session JSON includes credentials and cookies. Import is atomic;
 * restoring local state does not validate credentials with the server. */
wr_result_t wr_client_session_export(const wr_client_t *client, char **json, size_t *size);
wr_result_t wr_client_session_import(wr_client_t *client, const char *json, size_t size);

/* Fetch operations renew once on a definite authentication/expiry failure,
 * then retry the whole fetch once. Other failures are never retried. */
wr_result_t wr_client_shelf(wr_client_t *client, wr_book_t **books, size_t *count);
wr_result_t wr_client_catalog(wr_client_t *client, const char *book_id,
                              wr_chapter_t **chapters, size_t *count);
/* Returns verified e_0/e_1/e_3 TXT or XHTML bytes, preserving markup. A failed
 * request or fragment returns no partial output. */
wr_result_t wr_client_chapter(wr_client_t *client, const char *book_id,
                              const char *chapter_uid, char **content, size_t *size);

/* Cache and fetch share these parsers. They accept canonical arrays emitted
 * here and the corresponding server shelf/catalog envelopes. Arrays have no
 * fixed book/chapter limit. All elements are owned. */
wr_result_t wr_books_parse(const char *json, size_t size, wr_book_t **books, size_t *count);
wr_result_t wr_books_json(const wr_book_t *books, size_t count, char **json, size_t *size);
void wr_books_free(wr_book_t *books, size_t count);
wr_result_t wr_chapters_parse(const char *json, size_t size,
                               wr_chapter_t **chapters, size_t *count);
wr_result_t wr_chapters_json(const wr_chapter_t *chapters, size_t count,
                              char **json, size_t *size);
void wr_chapters_free(wr_chapter_t *chapters, size_t count);

#ifdef __cplusplus
}
#endif
#endif
