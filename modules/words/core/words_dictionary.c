#include "words_dictionary.h"
#include <string.h>

#define HEADER_SIZE 64u
#define ROW_SIZE (WORDS_KEY_SIZE + 8u)
#define ALIAS_SIZE (WORDS_KEY_SIZE + 4u)
static uint32_t u32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool words_dictionary_open(words_dictionary_t *d, words_read_fn read, void *context, uint32_t size)
{
    unsigned char header[HEADER_SIZE];
    if (!d || !read || size < HEADER_SIZE || size > INT32_MAX ||
        !read(context, 0, header, sizeof(header)) || memcmp(header, "EPDWD01", 8) ||
        u32(header + 8) != 1 || u32(header + 36) != WORDS_KEY_SIZE || u32(header + 32) != size)
        return false;
    words_dictionary_t value = {read, context, u32(header + 12), u32(header + 16),
                                u32(header + 20), u32(header + 24), u32(header + 28), size};
    if (value.index != HEADER_SIZE || !value.count ||
        value.count > (size - HEADER_SIZE) / ROW_SIZE ||
        value.alias_index != HEADER_SIZE + value.count * ROW_SIZE ||
        value.aliases > (size - value.alias_index) / ALIAS_SIZE ||
        value.data != value.alias_index + value.aliases * ALIAS_SIZE || value.data >= size)
        return false;
    *d = value;
    return true;
}

static bool row(words_dictionary_t *d, bool alias, uint32_t n, unsigned char *buffer)
{
    uint32_t count = alias ? d->aliases : d->count;
    uint32_t stride = alias ? ALIAS_SIZE : ROW_SIZE;
    uint32_t offset = alias ? d->alias_index : d->index;
    return n < count && d->read(d->context, offset + n * stride, buffer, stride) &&
           memchr(buffer, 0, WORDS_KEY_SIZE) != NULL;
}

static bool lower_bound(words_dictionary_t *d, const char *key, bool alias, uint32_t *position)
{
    uint32_t first = 0, last = alias ? d->aliases : d->count;
    unsigned char data[ROW_SIZE];
    while (first < last)
    {
        uint32_t middle = first + (last - first) / 2;
        if (!row(d, alias, middle, data)) return false;
        if (strcmp((char *)data, key) < 0) first = middle + 1;
        else last = middle;
    }
    *position = first;
    return true;
}

static bool collect(words_dictionary_t *d, const char *key, bool alias, bool prefix,
                    words_matches_t *matches)
{
    uint32_t pos, count = alias ? d->aliases : d->count;
    unsigned char data[ROW_SIZE];
    if (!lower_bound(d, key, alias, &pos)) return false;
    for (; pos < count; ++pos)
    {
        if (!row(d, alias, pos, data)) return false;
        int different = prefix ? strncmp((char *)data, key, strlen(key)) : strcmp((char *)data, key);
        if (different) break;
        uint32_t entry = alias ? u32(data + WORDS_KEY_SIZE) : pos;
        if (entry >= d->count) return false;
        bool duplicate = false;
        for (unsigned i = 0; i < matches->count; ++i) duplicate |= matches->entry[i] == entry;
        if (duplicate) continue;
        if (matches->count == WORDS_MATCH_MAX) { matches->more = true; break; }
        matches->entry[matches->count++] = entry;
    }
    return true;
}

bool words_dictionary_search(words_dictionary_t *d, const char *query, words_matches_t *matches)
{
    char key[WORDS_KEY_SIZE];
    if (!d || !query || !matches) return false;
    size_t length = strlen(query);
    if (!length || length >= sizeof(key)) return false;
    for (size_t i = 0; i <= length; ++i)
        key[i] = query[i] >= 'A' && query[i] <= 'Z' ? query[i] + ('a' - 'A') : query[i];
    memset(matches, 0, sizeof(*matches));
    if (!collect(d, key, false, false, matches)) return false;
    if (!matches->count && !collect(d, key, true, false, matches)) return false;
    if (!matches->count && !collect(d, key, false, true, matches)) return false;
    return true;
}

bool words_dictionary_entry(words_dictionary_t *d, uint32_t entry, char *buffer,
                            size_t capacity, const char *fields[WORDS_FIELD_COUNT])
{
    unsigned char data[ROW_SIZE];
    if (!d || !buffer || !fields || !row(d, false, entry, data)) return false;
    uint32_t offset = u32(data + WORDS_KEY_SIZE), size = u32(data + WORDS_KEY_SIZE + 4);
    if (offset < d->data || offset > d->size || size > d->size - offset ||
        size < WORDS_FIELD_COUNT || size > WORDS_ENTRY_MAX || size > capacity ||
        !d->read(d->context, offset, buffer, size)) return false;
    char *start = buffer, *end = buffer + size;
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i)
    {
        if (start >= end) return false;
        fields[i] = start;
        char *zero = memchr(start, 0, (size_t)(end - start));
        if (!zero) return false;
        start = zero + 1;
    }
    return start == end && fields[WORDS_WORD][0] && strlen(fields[WORDS_WORD]) < WORDS_KEY_SIZE;
}
