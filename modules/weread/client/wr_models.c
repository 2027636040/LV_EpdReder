#include "wr_internal.h"
#include <stdio.h>

static const wr_json *field(const wr_json *j, const char *a, const char *b, const char *c)
{
    const wr_json *v = j_get(j, a);
    if (!v && b) v = j_get(j, b);
    if (!v && c) v = j_get(j, c);
    return v;
}
static const wr_json *model_array(const wr_json *j, bool chapters, unsigned depth)
{
    const wr_json *v, *child;
    if (!j || depth > 64) return NULL;
    if (j_is_array(j))
    {
        child = j->child;
        if (!child || (j_is_object(child) && (chapters ?
            field(child, "uid", "chapterUid", "chapterInfo") :
            field(child, "id", "bookId", "bookInfo")))) return j;
    }
    if (j_is_object(j))
    {
        v = chapters ? field(j, "chapters", "chapterInfos", NULL) :
                       field(j, "books", "updatedBooks", "bookList");
        if (j_is_array(v))
        {
            const wr_json *found = model_array(v, chapters, depth + 1);
            if (found) return found;
        }
    }
    for (child = j->child; child; child = child->next)
    {
        v = model_array(child, chapters, depth + 1);
        if (v) return v;
    }
    return NULL;
}
static wr_result_t optional_integer(const wr_json *value, int64_t *out)
{
    *out = 0;
    return value ? wr_json_int(value,out) : WR_OK;
}
static wr_result_t parse_array(const char *text, size_t size, bool chapters,
                                wr_json **root, const wr_json **array, size_t *count)
{
    const wr_json *p;
    wr_result_t r = wr_json_parse(text, size, root);
    if (r != WR_OK) return r;
    r = wr_remote_result(*root);
    if (r != WR_OK) return r;
    {
        const wr_json *success = j_get(wr_payload(*root),"success");
        if (success && !wr_json_true(success)) return WR_ERR_REMOTE;
    }
    *array = model_array(*root, chapters, 0);
    if (!*array) return WR_ERR_PROTOCOL;
    for (p = (*array)->child; p; p = p->next)
    {
        if (*count == SIZE_MAX) return WR_ERR_MEMORY;
        ++*count;
    }
    return WR_OK;
}
void wr_books_free(wr_book_t *books, size_t count)
{
    size_t i;
    if (!books) return;
    for (i = 0; i < count; ++i)
    {
        epd_app_free(books[i].id); epd_app_free(books[i].title);
        epd_app_free(books[i].author); epd_app_free(books[i].format);
        epd_app_free(books[i].cover);
    }
    epd_app_free(books);
}
void wr_chapters_free(wr_chapter_t *chapters, size_t count)
{
    size_t i;
    if (!chapters) return;
    for (i = 0; i < count; ++i)
    { epd_app_free(chapters[i].uid); epd_app_free(chapters[i].title); }
    epd_app_free(chapters);
}
wr_result_t wr_books_parse(const char *text, size_t size, wr_book_t **out, size_t *count)
{
    wr_json *root = NULL;
    const wr_json *array = NULL, *p;
    wr_book_t *books = NULL;
    size_t n = 0, i = 0;
    wr_result_t r;
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!out || !count) return WR_ERR_ARGUMENT;
    r = parse_array(text, size, false, &root, &array, &n);
    if (r != WR_OK) goto done;
    if (n > SIZE_MAX / sizeof(*books)) { r = WR_ERR_MEMORY; goto done; }
    if (n)
    {
        books = wr_alloc(n * sizeof(*books));
        if (!books) { r = WR_ERR_MEMORY; goto done; }
        memset(books, 0, n * sizeof(*books));
    }
    for (p = array->child; p; p = p->next, ++i)
    {
        const wr_json *item = j_get(p, "bookInfo"), *author;
        if (!item) item = p;
        if (!j_is_object(item)) { r = WR_ERR_PROTOCOL; goto done; }
        r = wr_json_text(field(item, "id", "bookId", "book_id"), true, &books[i].id);
        if (r != WR_OK) goto done;
        r = wr_json_text(field(item, "title", "bookName", "name"), false, &books[i].title);
        if (r != WR_OK) goto done;
        author = field(item, "author", "authorName", NULL);
        if (j_is_object(author)) author = j_get(author, "name");
        r = wr_json_text(author, false, &books[i].author);
        if (r != WR_OK) goto done;
        r = wr_json_text(j_get(item, "format"), false, &books[i].format);
        if (r != WR_OK) goto done;
        r = wr_json_text(field(item, "cover", "coverUrl", NULL), false, &books[i].cover);
        if (r != WR_OK) goto done;
        r = optional_integer(j_get(item,"version"),&books[i].version);
        if (r == WR_OK && !books[i].version)
            r = optional_integer(j_get(item,"bookVersion"),&books[i].version);
        if (r != WR_OK) goto done;
        r = optional_integer(field(item,"update_time","updateTime",NULL),&books[i].update_time);
        if (r != WR_OK) goto done;
    }
    *out = books; *count = n; books = NULL;
done:
    wr_books_free(books, n); j_delete(root);
    return r;
}
wr_result_t wr_chapters_parse(const char *text, size_t size, wr_chapter_t **out, size_t *count)
{
    wr_json *root = NULL;
    const wr_json *array = NULL, *p;
    wr_chapter_t *chapters = NULL;
    size_t n = 0, i = 0;
    wr_result_t r;
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!out || !count) return WR_ERR_ARGUMENT;
    r = parse_array(text, size, true, &root, &array, &n);
    if (r != WR_OK) goto done;
    if (n > SIZE_MAX / sizeof(*chapters) || n > INT64_MAX)
    { r = WR_ERR_MEMORY; goto done; }
    if (n)
    {
        chapters = wr_alloc(n * sizeof(*chapters));
        if (!chapters) { r = WR_ERR_MEMORY; goto done; }
        memset(chapters, 0, n * sizeof(*chapters));
    }
    for (p = array->child; p; p = p->next, ++i)
    {
        const wr_json *item = j_get(p, "chapterInfo"), *index;
        if (!item) item = p;
        if (!j_is_object(item)) { r = WR_ERR_PROTOCOL; goto done; }
        r = wr_json_text(field(item, "uid", "chapterUid", NULL), true, &chapters[i].uid);
        if (r != WR_OK) goto done;
        r = wr_json_text(field(item, "title", "chapterTitle", NULL), false, &chapters[i].title);
        if (r != WR_OK) goto done;
        index = field(item, "index", "chapterIdx", NULL);
        chapters[i].index = (int64_t)i;
        if (index && (r = wr_json_int(index, &chapters[i].index)) != WR_OK) goto done;
        r = optional_integer(field(item,"update_time","updateTime",NULL),&chapters[i].update_time);
        if (r != WR_OK) goto done;
    }
    *out = chapters; *count = n; chapters = NULL;
done:
    wr_chapters_free(chapters, n); j_delete(root);
    return r;
}
wr_result_t wr_books_json(const wr_book_t *books, size_t count, char **out, size_t *size)
{
    wr_json *root;
    wr_result_t r = WR_ERR_MEMORY;
    size_t i;
    if (out) *out = NULL;
    if (size) *size = 0;
    if (!out || !size || (!books && count)) return WR_ERR_ARGUMENT;
    root = j_array();
    if (!root) return WR_ERR_MEMORY;
    for (i = 0; i < count; ++i)
    {
        wr_json *v;
        char version[32], update_time[32];
        if (!books[i].id || !books[i].id[0]) { r = WR_ERR_ARGUMENT; goto done; }
        v = j_object();
        if (!v) goto done;
        if (!wr_cjson_AddItemToArray(root, v)) { j_delete(v); goto done; }
        snprintf(version,sizeof(version),"%lld",(long long)books[i].version);
        snprintf(update_time,sizeof(update_time),"%lld",(long long)books[i].update_time);
        if (!j_string(v, "id", books[i].id) ||
            !j_string(v, "title", books[i].title ? books[i].title : "") ||
            !j_string(v, "author", books[i].author ? books[i].author : "") ||
            !j_string(v, "format", books[i].format ? books[i].format : "") ||
            !j_string(v, "cover", books[i].cover ? books[i].cover : "") ||
            !j_string(v,"version",version) || !j_string(v,"update_time",update_time)) goto done;
    }
    r = wr_json_print(root, out, size);
done:
    j_delete(root); return r;
}
wr_result_t wr_chapters_json(const wr_chapter_t *chapters, size_t count, char **out, size_t *size)
{
    wr_json *root;
    wr_result_t r = WR_ERR_MEMORY;
    size_t i;
    if (out) *out = NULL;
    if (size) *size = 0;
    if (!out || !size || (!chapters && count)) return WR_ERR_ARGUMENT;
    root = j_array();
    if (!root) return WR_ERR_MEMORY;
    for (i = 0; i < count; ++i)
    {
        wr_json *v;
        char index[32], update_time[32];
        if (!chapters[i].uid || !chapters[i].uid[0]) { r = WR_ERR_ARGUMENT; goto done; }
        v = j_object();
        if (!v) goto done;
        if (!wr_cjson_AddItemToArray(root, v)) { j_delete(v); goto done; }
        snprintf(index, sizeof(index), "%lld", (long long)chapters[i].index);
        snprintf(update_time,sizeof(update_time),"%lld",(long long)chapters[i].update_time);
        if (!j_string(v, "uid", chapters[i].uid) ||
            !j_string(v, "title", chapters[i].title ? chapters[i].title : "") ||
            !j_string(v, "index", index) || !j_string(v,"update_time",update_time)) goto done;
    }
    r = wr_json_print(root, out, size);
done:
    j_delete(root); return r;
}
