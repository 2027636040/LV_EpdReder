/* Target ordering, title ownership, index bounds and allocation failure tests. */
#define main regression_main
#include "regression.c"
#undef main
#include "../mobi_converter.c"

static const book_allocator_t test_allocator = { allocate, resize, release };
static const book_sink_t test_sink = { .cancelled = cancelled };

static void start_case(conversion_t *ctx, MOBIRawml *raw, MOBIPart parts[4])
{
    unsigned i;
    memset(ctx, 0, sizeof(*ctx));
    memset(raw, 0, sizeof(*raw));
    memset(parts, 0, 4 * sizeof(*parts));
    for (i = 0; i < 4; ++i) {
        parts[i].uid = 100 + i;
        parts[i].size = 65536;
        parts[i].next = i == 3 ? NULL : &parts[i + 1];
    }
    raw->markup = parts;
    ctx->raw = raw;
    calls = polls = peak = 0;
    assert(mobi_port_start(&test_allocator, &test_sink) == BOOK_OK);
}

static int collect(conversion_t *ctx, unsigned count, unsigned order)
{
    unsigned i;
    int result = index_parts(ctx);
    if (result) return result;
    for (i = 0; i < count; ++i) {
        unsigned key = order == 0 ? i : order == 1 ? count - 1 - i : (i * 73u) % count;
        unsigned part = key % 4, offset = key / 4;
        result = add_target(ctx, part, offset, i & 1 ? "" : NULL, 2);
        if (result) return result;
        result = add_target(ctx, part, offset, "first", key + 1);
        if (result) return result;
        result = add_target(ctx, part, offset, "later", key + 10);
        if (result) return result;
    }
    return finish_targets(ctx);
}

static void check_targets(conversion_t *ctx, unsigned count)
{
    size_t i;
    assert(ctx->target_count == count);
    for (i = 0; i < count; ++i) {
        const target_t *t = &ctx->targets[i];
        char id[64];
        assert(t->part == i / (count / 4));
        assert(t->offset == i % (count / 4));
        assert(t->level == t->offset * 4 + t->part + 1);
        assert(t->title && !strcmp(t->title, "first"));
        target_id(id, sizeof(id), t->part, t->offset);
        assert(find_target(ctx, id) == t);
    }
    assert(!find_target(ctx, "mobi:00:0"));
    assert(!find_target(ctx, "mobi:+0:0"));
    assert(!find_target(ctx, "mobi:0:"));
    assert(!find_target(ctx, "mobi:0:0suffix"));
    assert(!find_target(ctx, "mobi:4294967296:0"));
    assert(!find_target(ctx, "mobi:9:0"));
    assert(!find_target(ctx, "mobi:0:65536"));
}

static void end_case(conversion_t *ctx)
{
    free_targets(ctx);
    /* These tests allocate only the target/index objects. Assert their explicit
     * cleanup before the port's final sweep can recover orphaned allocations. */
    assert(live == 0 && bytes_live == 0 && files == 0 && !locked);
    mobi_port_finish();
}

static void check_stream(void)
{
    static const char input[] = "<p>Hi</p><img recindex='7'/><a filepos='1'>back</a>";
    static const char expected[] = "<a id=\"mobi:0:0\"></a><a id=\"mobi:0:1\"></a>"
        "<p>Hi</p><img src=\"mobi-resource-6\"/><a href=\"#mobi:0:1\">back</a>";
    conversion_t ctx;
    MOBIRawml raw;
    MOBIPart parts[4];
    markup_t stream;
    char actual[512], wanted[512];
    unsigned chunk;
    start_case(&ctx, &raw, parts);
    assert(index_parts(&ctx) == BOOK_OK);
    ctx.have_cover = 1;
    ctx.cover_uid = 6;
    memset(&stream, 0, sizeof(stream));
    stream.source.ctx = &ctx;
    stream.source.data = (const unsigned char *)input;
    stream.source.size = sizeof(input) - 1;
    assert(scan_links(&stream) == BOOK_OK && ctx.cover_in_body);
    assert(add_target(&ctx, 0, 0, NULL, 0) == BOOK_OK);
    assert(add_target(&ctx, 0, sizeof(input) - 1, NULL, 0) == BOOK_OK);
    assert(finish_targets(&ctx) == BOOK_OK);
    snprintf(wanted, sizeof(wanted), "%s<a id=\"mobi:0:%u\"></a>",
             expected, (unsigned)sizeof(input) - 1);
    for (chunk = 1; chunk <= 251; chunk += 250) {
        int n;
        size_t used = 0;
        memset(&stream, 0, sizeof(stream));
        stream.source.ctx = &ctx;
        stream.source.data = (const unsigned char *)input;
        stream.source.size = sizeof(input) - 1;
        while ((n = markup_read(&stream, actual + used, chunk)) > 0) used += n;
        assert(n == 0 && used == strlen(wanted) && !memcmp(actual, wanted, used));
    }
    end_case(&ctx);
}

int main(void)
{
    conversion_t ctx;
    MOBIRawml raw;
    MOBIPart parts[4];
    size_t baseline, i;
    unsigned order;
    int result;
    for (order = 0; order < 3; ++order) {
        start_case(&ctx, &raw, parts);
        assert(collect(&ctx, 16384, order) == BOOK_OK);
        check_targets(&ctx, 16384);
        parts[0].next = NULL;
        assert(part_at(&ctx, 3) == &parts[3]);
        assert(part_at(&ctx, 4) == NULL);
        assert(add_target(&ctx, 4, 0, NULL, 0) == BOOK_ERROR);
        assert(add_target(&ctx, 0, 65537, NULL, 0) == BOOK_ERROR);
        end_case(&ctx);
    }
    start_case(&ctx, &raw, parts);
    assert(collect(&ctx, 64, 2) == BOOK_OK);
    baseline = calls;
    end_case(&ctx);
    for (i = 1; i <= baseline; ++i) {
        fail_at = i;
        start_case(&ctx, &raw, parts);
        result = collect(&ctx, 64, 2);
        assert(result == BOOK_NO_MEMORY);
        end_case(&ctx);
    }
    fail_at = 0;
    start_case(&ctx, &raw, parts);
    assert(index_parts(&ctx) == BOOK_OK);
    for (i = 0; i < 4096; ++i)
        assert(add_target(&ctx, i % 4, (unsigned)i, "cancel", 1) == BOOK_OK);
    cancel_at = polls + 1;
    assert(finish_targets(&ctx) == BOOK_CANCELLED);
    end_case(&ctx);
    cancel_at = 0;
    check_stream();
    start_case(&ctx, &raw, parts);
    raw.markup = NULL;
    raw.flow = &parts[0];
    parts[0].next = NULL;
    assert(index_parts(&ctx) == BOOK_OK && calls == 0);
    assert(part_at(&ctx, 0) == parts && !part_at(&ctx, 1));
    assert(finish_targets(&ctx) == BOOK_OK);
    assert(add_target(&ctx, 0, 65536, NULL, 0) == BOOK_OK);
    assert(finish_targets(&ctx) == BOOK_OK && ctx.target_count == 1);
    assert(find_target(&ctx, "mobi:0:65536") == ctx.targets);
    end_case(&ctx);
    printf("PASS targets: 3 x 49152 entries, first nonempty title/level, exact IDs, "
           "part index/bounds, cover scan, tag/EOF anchors, empty set, sort cancellation, "
           "%zu allocation failures; no leaks\n", baseline);
    return 0;
}
