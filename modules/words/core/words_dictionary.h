#ifndef WORDS_DICTIONARY_H
#define WORDS_DICTIONARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WORDS_KEY_SIZE 96u
#define WORDS_ENTRY_MAX 32768u
#define WORDS_MATCH_MAX 8u
#define WORDS_FIELD_COUNT 6u
enum { WORDS_WORD, WORDS_PHONETIC, WORDS_TRANSLATION, WORDS_DEFINITION, WORDS_EXCHANGE, WORDS_TAGS };
typedef bool (*words_read_fn)(void *context, uint32_t offset, void *buffer, size_t size);
typedef struct
{
    words_read_fn read;
    void *context;
    uint32_t count, aliases, index, alias_index, data, size;
} words_dictionary_t;
typedef struct
{
    uint32_t count, entry[WORDS_MATCH_MAX];
    bool more;
} words_matches_t;

/* An immutable dictionary generation is required for the lifetime of this handle. */
bool words_dictionary_open(words_dictionary_t *dictionary, words_read_fn read,
                           void *context, uint32_t file_size);
bool words_dictionary_search(words_dictionary_t *dictionary, const char *query, words_matches_t *matches);
bool words_dictionary_entry(words_dictionary_t *dictionary, uint32_t entry, char *buffer,
                            size_t capacity, const char *fields[WORDS_FIELD_COUNT]);

#endif
