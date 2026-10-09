# Books miniz subset

Source: project `src/thirdparty/miniz/miniz.c` and `miniz.h` (header identifies version 2.2.0). Files were copied before modification. Original public-domain/unlicense notices remain in both source files.

Local changes:

- Guarded configuration disables stdio, ZIP writing, zlib APIs/compatibility names and default heap allocation. Reading uses explicit allocation/input callbacks and incremental extraction.
- Unaligned load casts are disabled, including on the host test build.
- ZIP64 extra-field temporary allocation/free uses archive allocator callbacks.

The converter checks entry length and CRC through `mz_zip_reader_extract_iter_free()`. Whole-entry extraction and compression APIs are unused. Function/data sections permit the parent link to discard unused helpers.
