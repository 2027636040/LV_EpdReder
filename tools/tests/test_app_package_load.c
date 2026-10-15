#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "constants.h"
#include "intent.h"

typedef int rt_err_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
#define RT_NAME_MAX 16
#define EPD_APP_ABI 1
#define EPD_APP_BUILD_ID "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#include "content_headers.h"

#define open(path, flags) _open(path, (flags) | _O_BINARY)
#define close _close
#define read _read
#define lseek _lseek
#define fstat _fstat64
#define stat _stat64
#define O_RDONLY _O_RDONLY

static bool hashes_ok = true;
static uint32_t main_span = 3u * 1024u * 1024u, library_span = 4096;
static cJSON *manifest_read(const char *root)
{
    assert(!strcmp(root, "."));
    char text[1024];
    snprintf(text, sizeof(text),
        "{\"package_format\":1,\"abi\":1,\"id\":\"sample\",\"name\":\"Sample\","
        "\"version\":1,\"data_version\":1,\"load_bytes\":%u,"
        "\"build_id\":\"" EPD_APP_BUILD_ID "\",\"files_sha256\":\"" EPD_APP_BUILD_ID "\","
        "\"settings\":false,\"background\":false,\"libraries\":{\"mobi\":%u}}",
        main_span, library_span);
    return cJSON_Parse(text);
}

/* These tests isolate load-size calculation and selection. Package digest
 * generation is exercised separately by test_package_capacity.py. */
static bool file_hash(const char *path, const char *digest)
{ (void)path; (void)digest; return hashes_ok; }
static bool file_count(const char *root, unsigned depth, uint64_t *count)
{ (void)root; (void)depth; *count = 4; return true; }
#include "package_load_under_test.h"

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static void write_elf(const char *path, uint32_t span, bool truncated)
{
    unsigned char data[512] = {0};
    memcpy(data, "\177ELF\1\1\1", 7);
    data[16] = 3; data[18] = 40;
    put32(data + 28, 52);
    data[42] = 32; data[44] = 1;
    put32(data + 52, 1);
    put32(data + 68, sizeof(data));
    put32(data + 72, span);
    FILE *file = fopen(path, "wb");
    assert(file);
    size_t size = truncated ? 80 : sizeof(data);
    assert(fwrite(data, 1, size, file) == size);
    assert(!fclose(file));
}

int main(void)
{
    assert(!_mkdir("codecs"));
    write_elf("sample.so", main_span, false);
    write_elf("codecs/mobi.so", library_span, false);
    FILE *index = fopen("files.sha256", "wb");
    assert(index);
    assert(fprintf(index, EPD_APP_BUILD_ID "  codecs/mobi.so\n"
                         EPD_APP_BUILD_ID "  sample.so\n") > 0);
    assert(!fclose(index));
    size_t peak = 0;
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_OK);
    assert(peak == (size_t)main_span + 512); /* No 2 MiB ceiling. */
    assert(app_package_verify_library(".", "sample", "mobi", &peak) == APP_PACKAGE_OK);
    assert(peak == (size_t)library_span + 512); /* Main ELF must not overwrite it. */
    assert(app_package_verify(".", "sample", true, NULL) == APP_PACKAGE_OK);

    main_span = 1024;
    write_elf("sample.so", main_span, false);
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_OK);
    assert(peak == 1536); /* Small modules do not evict 2 MiB of warm glyphs. */
    write_elf("sample.so", main_span, true);
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_INTEGRITY_ERROR);
    assert(peak == 0);
    write_elf("sample.so", main_span + 1, false);
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_INTEGRITY_ERROR);
    assert(peak == 0);
    write_elf("sample.so", main_span, false);
    hashes_ok = false;
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_INTEGRITY_ERROR);
    assert(peak == 0);
    hashes_ok = true;
    assert(app_package_verify_library(".", "sample", "../mobi", &peak) == APP_PACKAGE_FORMAT_ERROR);
    assert(peak == 0);
    assert(!_unlink("sample.so"));
    assert(app_package_verify(".", "sample", false, &peak) == APP_PACKAGE_INTEGRITY_ERROR);
    assert(peak == 0);
    puts("package load: main/private ELF sizes, small/large spans, truncation and missing files passed");
    return 0;
}
