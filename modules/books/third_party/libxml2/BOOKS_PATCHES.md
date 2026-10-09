# Books libxml2 subset

Source: local `solution2.0/sdk/external/libxml2-2.6.24`, version 2.6.24. Selected implementation files and headers were copied before modification. Original licensing and authorship remain in `Copyright`, `AUTHORS` and source headers.

The compiled source list is in `../../formats/sources.py`. It retains SAX2/HTML push parsing, encoding, parser internals, required tree primitives, dictionary/hash/list, URI, errors and memory infrastructure. Unused feature flags are disabled in `include/libxml/xmlversion.h`; target configuration overrides are at the end of `config.h`.

Local changes:

- `libxml.h` includes `book_xml_port.h`. Default heap references bind to the books allocator; implicit filesystem, environment and logging access are disabled. Structured parser callbacks carry errors.
- `xmlIO.c` does not register default filename input callbacks.
- `encoding.c` uses a 256-byte scratch buffer instead of a 32000-byte stack buffer for encoded byte offsets. The adapter supplies a UTF-16 offset fallback when output encoders are disabled.
- `parser.c` expands the numeric character-reference formatting buffer to 16 bytes.
- `HTMLparser.c` exits start-tag attribute parsing and releases parsed values when a fatal allocation error sets EOF. `xmlNextChar()` does not advance in that state, so the old invalid-attribute recovery loop otherwise never terminates.
- The adapter suppresses SAX callbacks on sink failure and exits at the end of the current input block. It does not call `xmlStopParser()` inside an HTML SAX callback, because this version continues reading the replaced input pointer.

This is the supplied older version with integration changes, not an update to current upstream. External entity loading is disabled, entity declarations are rejected by the adapter, and parser memory/nesting/resource sizes are bounded by the conversion layer.
