/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_MOBI_PORT_H
#define BOOK_MOBI_PORT_H
#include <stddef.h>
#include "book_codec.h"
#ifndef BOOK_MOBI_MEASURE
#define BOOK_MOBI_MEASURE 0
#endif
#if BOOK_MOBI_MEASURE
#include "compression.h"
/* Phase 0 is setup; phases 1 and 2 are the link scan and body conversion.
 * Counts include failed calls. Timing uses the platform monotonic tick. */
void mobi_port_measure_phase(unsigned phase);
void mobi_port_measure_report(void);
MOBI_RET mobi_measure_lz77(unsigned char *out, const unsigned char *in,
                         size_t *len_out, size_t len_in);
MOBI_RET mobi_measure_huffman(unsigned char *out, const unsigned char *in,
                            size_t *len_out, size_t len_in, const MOBIHuffCdic *dictionary);
/* Only the vendor util translation unit redirects decompressor calls. */
#ifdef MOBI_MEASURE_DECOMPRESSION
#define mobi_decompress_lz77 mobi_measure_lz77
#define mobi_decompress_huffman mobi_measure_huffman
#endif
#else
#define mobi_port_measure_phase(phase) ((void)0)
#define mobi_port_measure_report() ((void)0)
#endif
struct dfs_fd;
void *mobi_alloc(size_t size);
void *mobi_calloc(size_t count, size_t size);
void *mobi_realloc(void *memory, size_t size);
void mobi_release(void *memory);
char *bk_mobi_strdup(const char *text);
int mobi_dfs_read(struct dfs_fd *file, void *buffer, size_t size);
int mobi_dfs_open(struct dfs_fd *file, const char *path, int flags);
int mobi_dfs_seek(struct dfs_fd *file, long offset);
int mobi_dfs_close(struct dfs_fd *file);
int mobi_read_at(const char *path, size_t offset, void *buffer, size_t size);
void mobi_port_trace(const char *stage);
int mobi_port_start(const book_allocator_t *allocator, const book_sink_t *sink);
void mobi_port_finish(void);
int mobi_port_status(void);
#endif
