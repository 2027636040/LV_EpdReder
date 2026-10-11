#define CJSON_HIDE_SYMBOLS
#define CJSON_NESTING_LIMIT 64
/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef wr_cjson__h
#define wr_cjson__h

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

/* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called from a project with a different default calling convention.  For windows you have 3 define options:

CJSON_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
CJSON_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
CJSON_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

For *nix builds that support visibility attribute, you can define similar behavior by

setting default visibility to hidden by adding
-fvisibility=hidden (for gcc)
or
-xldscope=hidden (for sun cc)
to CFLAGS

then using the CJSON_API_VISIBILITY flag to "export" the same symbols the way CJSON_EXPORT_SYMBOLS does

*/

#define CJSON_CDECL __cdecl
#define CJSON_STDCALL __stdcall

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type)   type CJSON_STDCALL
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllexport) type CJSON_STDCALL
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllimport) type CJSON_STDCALL
#endif
#else /* !__WINDOWS__ */
#define CJSON_CDECL
#define CJSON_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/* project version */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 17

#include <stddef.h>

/* wr_cjson Types: */
#define wr_cjson_Invalid (0)
#define wr_cjson_False  (1 << 0)
#define wr_cjson_True   (1 << 1)
#define wr_cjson_NULL   (1 << 2)
#define wr_cjson_Number (1 << 3)
#define wr_cjson_String (1 << 4)
#define wr_cjson_Array  (1 << 5)
#define wr_cjson_Object (1 << 6)
#define wr_cjson_Raw    (1 << 7) /* raw json */

#define wr_cjson_IsReference 256
#define wr_cjson_StringIsConst 512

/* The wr_cjson structure: */
typedef struct wr_cjson
{
    /* next/prev allow you to walk array/object chains. Alternatively, use GetArraySize/GetArrayItem/GetObjectItem */
    struct wr_cjson *next;
    struct wr_cjson *prev;
    /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */
    struct wr_cjson *child;

    /* The type of the item, as above. */
    int type;

    /* The item's string, if type==wr_cjson_String  and type == wr_cjson_Raw */
    char *valuestring;
    /* writing to valueint is DEPRECATED, use wr_cjson_SetNumberValue instead */
    int valueint;
    /* The item's number, if type==wr_cjson_Number */
    double valuedouble;

    /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
    char *string;
} wr_cjson;

typedef int wr_cjson_bool;

/* Limits how deeply nested arrays/objects can be before wr_cjson rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

/* returns the version of wr_cjson as a string */
CJSON_PUBLIC(const char*) wr_cjson_Version(void);

/* All allocations use epd_app_alloc/free. Delete parsed trees with
 * wr_cjson_Delete and printed strings with wr_cjson_free or epd_app_free. */
/* Supply a block of JSON, and this returns a wr_cjson object you can interrogate. */
CJSON_PUBLIC(wr_cjson *) wr_cjson_Parse(const char *value);
CJSON_PUBLIC(wr_cjson *) wr_cjson_ParseWithLength(const char *value, size_t buffer_length);
/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to the final byte parsed. */
/* If you supply a ptr in return_parse_end and parsing fails, then return_parse_end will contain a pointer to the error so will match wr_cjson_GetErrorPtr(). */
CJSON_PUBLIC(wr_cjson *) wr_cjson_ParseWithOpts(const char *value, const char **return_parse_end, wr_cjson_bool require_null_terminated);
CJSON_PUBLIC(wr_cjson *) wr_cjson_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, wr_cjson_bool require_null_terminated);

/* Render a wr_cjson entity to text for transfer/storage. */
CJSON_PUBLIC(char *) wr_cjson_Print(const wr_cjson *item);

CJSON_PUBLIC(void) wr_cjson_DeleteChar(char *buffer);

/* Render a wr_cjson entity to text for transfer/storage without any formatting. */
CJSON_PUBLIC(char *) wr_cjson_PrintUnformatted(const wr_cjson *item);
/* Render a wr_cjson entity to text using a buffered strategy. prebuffer is a guess at the final size. guessing well reduces reallocation. fmt=0 gives unformatted, =1 gives formatted */
CJSON_PUBLIC(char *) wr_cjson_PrintBuffered(const wr_cjson *item, int prebuffer, wr_cjson_bool fmt);
/* Render a wr_cjson entity to text using a buffer already allocated in memory with given length. Returns 1 on success and 0 on failure. */
/* NOTE: wr_cjson is not always 100% accurate in estimating how much memory it will use, so to be safe allocate 5 bytes more than you actually need */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_PrintPreallocated(wr_cjson *item, char *buffer, const int length, const wr_cjson_bool format);
/* Delete a wr_cjson entity and all subentities. */
CJSON_PUBLIC(void) wr_cjson_Delete(wr_cjson *item);

/* Returns the number of items in an array (or object). */
CJSON_PUBLIC(int) wr_cjson_GetArraySize(const wr_cjson *array);
/* Retrieve item number "index" from array "array". Returns NULL if unsuccessful. */
CJSON_PUBLIC(wr_cjson *) wr_cjson_GetArrayItem(const wr_cjson *array, int index);
/* Get item "string" from object. Case insensitive. */
CJSON_PUBLIC(wr_cjson *) wr_cjson_GetObjectItem(const wr_cjson * const object, const char * const string);
CJSON_PUBLIC(wr_cjson *) wr_cjson_GetObjectItemCaseSensitive(const wr_cjson * const object, const char * const string);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_HasObjectItem(const wr_cjson *object, const char *string);
/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to look a few chars back to make sense of it. Defined when wr_cjson_Parse() returns 0. 0 when wr_cjson_Parse() succeeds. */
CJSON_PUBLIC(const char *) wr_cjson_GetErrorPtr(void);

/* Check item type and return its value */
CJSON_PUBLIC(char *) wr_cjson_GetStringValue(const wr_cjson * const item);
CJSON_PUBLIC(double) wr_cjson_GetNumberValue(const wr_cjson * const item);

/* These functions check the type of an item */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsInvalid(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsFalse(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsTrue(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsBool(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsNull(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsNumber(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsString(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsArray(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsObject(const wr_cjson * const item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_IsRaw(const wr_cjson * const item);

/* These calls create a wr_cjson item of the appropriate type. */
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateNull(void);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateTrue(void);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateFalse(void);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateBool(wr_cjson_bool boolean);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateNumber(double num);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateString(const char *string);
/* raw json */
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateRaw(const char *raw);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateArray(void);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateObject(void);

/* Create a string where valuestring references a string so
 * it will not be freed by wr_cjson_Delete */
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateStringReference(const char *string);
/* Create an object/array that only references it's elements so
 * they will not be freed by wr_cjson_Delete */
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateObjectReference(const wr_cjson *child);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateArrayReference(const wr_cjson *child);

/* These utilities create an Array of count items.
 * The parameter count cannot be greater than the number of elements in the number array, otherwise array access will be out of bounds.*/
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateIntArray(const int *numbers, int count);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateFloatArray(const float *numbers, int count);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateDoubleArray(const double *numbers, int count);
CJSON_PUBLIC(wr_cjson *) wr_cjson_CreateStringArray(const char *const *strings, int count);

/* Append item to the specified array/object. */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_AddItemToArray(wr_cjson *array, wr_cjson *item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_AddItemToObject(wr_cjson *object, const char *string, wr_cjson *item);
/* Use this when string is definitely const (i.e. a literal, or as good as), and will definitely survive the wr_cjson object.
 * WARNING: When this function was used, make sure to always check that (item->type & wr_cjson_StringIsConst) is zero before
 * writing to `item->string` */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_AddItemToObjectCS(wr_cjson *object, const char *string, wr_cjson *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing wr_cjson to a new wr_cjson, but don't want to corrupt your existing wr_cjson. */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_AddItemReferenceToArray(wr_cjson *array, wr_cjson *item);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_AddItemReferenceToObject(wr_cjson *object, const char *string, wr_cjson *item);

/* Remove/Detach items from Arrays/Objects. */
CJSON_PUBLIC(wr_cjson *) wr_cjson_DetachItemViaPointer(wr_cjson *parent, wr_cjson * const item);
CJSON_PUBLIC(wr_cjson *) wr_cjson_DetachItemFromArray(wr_cjson *array, int which);
CJSON_PUBLIC(void) wr_cjson_DeleteItemFromArray(wr_cjson *array, int which);
CJSON_PUBLIC(wr_cjson *) wr_cjson_DetachItemFromObject(wr_cjson *object, const char *string);
CJSON_PUBLIC(wr_cjson *) wr_cjson_DetachItemFromObjectCaseSensitive(wr_cjson *object, const char *string);
CJSON_PUBLIC(void) wr_cjson_DeleteItemFromObject(wr_cjson *object, const char *string);
CJSON_PUBLIC(void) wr_cjson_DeleteItemFromObjectCaseSensitive(wr_cjson *object, const char *string);

/* Update array items. */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_InsertItemInArray(wr_cjson *array, int which, wr_cjson *newitem); /* Shifts pre-existing items to the right. */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_ReplaceItemViaPointer(wr_cjson * const parent, wr_cjson * const item, wr_cjson * replacement);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_ReplaceItemInArray(wr_cjson *array, int which, wr_cjson *newitem);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_ReplaceItemInObject(wr_cjson *object,const char *string,wr_cjson *newitem);
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_ReplaceItemInObjectCaseSensitive(wr_cjson *object,const char *string,wr_cjson *newitem);

/* Duplicate a wr_cjson item */
CJSON_PUBLIC(wr_cjson *) wr_cjson_Duplicate(const wr_cjson *item, wr_cjson_bool recurse);
/* Duplicate will create a new, identical wr_cjson item to the one you pass, in new memory that will
 * need to be released. With recurse!=0, it will duplicate any children connected to the item.
 * The item->next and ->prev pointers are always zero on return from Duplicate. */
/* Recursively compare two wr_cjson items for equality. If either a or b is NULL or invalid, they will be considered unequal.
 * case_sensitive determines if object keys are treated case sensitive (1) or case insensitive (0) */
CJSON_PUBLIC(wr_cjson_bool) wr_cjson_Compare(const wr_cjson * const a, const wr_cjson * const b, const wr_cjson_bool case_sensitive);

/* Minify a strings, remove blank characters(such as ' ', '\t', '\r', '\n') from strings.
 * The input pointer json cannot point to a read-only address area, such as a string constant,
 * but should point to a readable and writable address area. */
CJSON_PUBLIC(void) wr_cjson_Minify(char *json);

/* Helper functions for creating and adding items to an object at the same time.
 * They return the added item or NULL on failure. */
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddNullToObject(wr_cjson * const object, const char * const name);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddTrueToObject(wr_cjson * const object, const char * const name);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddFalseToObject(wr_cjson * const object, const char * const name);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddBoolToObject(wr_cjson * const object, const char * const name, const wr_cjson_bool boolean);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddNumberToObject(wr_cjson * const object, const char * const name, const double number);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddStringToObject(wr_cjson * const object, const char * const name, const char * const string);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddRawToObject(wr_cjson * const object, const char * const name, const char * const raw);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddObjectToObject(wr_cjson * const object, const char * const name);
CJSON_PUBLIC(wr_cjson*) wr_cjson_AddArrayToObject(wr_cjson * const object, const char * const name);

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define wr_cjson_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* helper for the wr_cjson_SetNumberValue macro */
CJSON_PUBLIC(double) wr_cjson_SetNumberHelper(wr_cjson *object, double number);
#define wr_cjson_SetNumberValue(object, number) ((object != NULL) ? wr_cjson_SetNumberHelper(object, (double)number) : (number))
/* Change the valuestring of a wr_cjson_String object, only takes effect when type of object is wr_cjson_String */
CJSON_PUBLIC(char*) wr_cjson_SetValuestring(wr_cjson *object, const char *valuestring);

/* If the object is not a boolean type this does nothing and returns wr_cjson_Invalid else it returns the new type*/
#define wr_cjson_SetBoolValue(object, boolValue) ( \
    (object != NULL && ((object)->type & (wr_cjson_False|wr_cjson_True))) ? \
    (object)->type=((object)->type &(~(wr_cjson_False|wr_cjson_True)))|((boolValue)?wr_cjson_True:wr_cjson_False) : \
    wr_cjson_Invalid\
)

/* Macro for iterating over an array or object */
#define wr_cjson_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/* Allocate/free objects through the fixed application allocator. */
CJSON_PUBLIC(void *) wr_cjson_malloc(size_t size);
CJSON_PUBLIC(void) wr_cjson_free(void *object);

#ifdef __cplusplus
}
#endif

#endif
