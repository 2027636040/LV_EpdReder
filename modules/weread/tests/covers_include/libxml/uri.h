#ifndef COVERS_TEST_URI_H
#define COVERS_TEST_URI_H
/* URI parsing itself belongs to libxml's tests. These tests use absolute URLs. */
typedef unsigned char xmlChar;
xmlChar *xmlBuildURI(const xmlChar *uri, const xmlChar *base);
void xmlFree(void *pointer);
#endif
