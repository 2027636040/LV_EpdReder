/* SPDX-License-Identifier: Apache-2.0 */
/* Keep the existing file/ZIP stream implementation. Resource dispatch here
 * also accepts absolute URLs; fetching is owned by resources.open. */
#define bf_picture epd_reader_local_picture
#include "../../books/formats/book_stream.c"
#undef bf_picture
#include "reader_resource.h"

static int resource_result(const book_sink_t *sink, int result)
{
    if (result != READER_RESOURCE_MISSING) return result;
    static const char placeholder[] = "[图片暂不可用]";
    return bf_text(sink, placeholder, sizeof(placeholder) - 1, BOOK_STYLE_ITALIC);
}

int bf_picture(const book_sink_t *sink, const book_resources_t *resources,
               const char *base, const char *reference)
{
    if (!reference || !*reference) return BOOK_OK;
    if (!bf_external(reference)) return resource_result(sink, epd_reader_local_picture(sink, resources, base, reference));
    if (!sink->image || !resources || !resources->open || !*reference) return BOOK_OK;
    int result = bf_cancel(sink);
    if (result != BOOK_OK) return result;
    book_stream_t stream = {0};
    result = resources->open(resources->user, base, reference, &stream);
    if (result == BOOK_OK && !stream.read) result = BOOK_IO_ERROR;
    if (result == BOOK_OK) result = sink->image(sink->user, reference, &stream);
    return resource_result(sink, bf_finish(&stream, result));
}
