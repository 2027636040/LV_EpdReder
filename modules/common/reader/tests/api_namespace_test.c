/* Both public headers and both engines' linker symbols must coexist. */
#include "../reader.h"
#include "../../../books/reader.h"

#if defined(reader_open) || defined(reader_close)
#error "The shared reader must not alias the legacy TXT API"
#endif

/* Link these legacy signatures alongside the real shared implementations. */
rt_err_t reader_open(const char *path) { (void)path; return 0; }
void reader_close(void) {}
