/*
 SPDX-License-Identifier: MIT

 Parson 1.5.2 (https://github.com/kgabis/parson)
 Copyright (c) 2012 - 2023 Krzysztof Gabis

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
#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif /* _CRT_SECURE_NO_WARNINGS */
#endif /* _MSC_VER */

#include "parson.h"

#define PARSON_IMPL_VERSION_MAJOR 1
#define PARSON_IMPL_VERSION_MINOR 5
#define PARSON_IMPL_VERSION_PATCH 2

#if (PARSON_VERSION_MAJOR != PARSON_IMPL_VERSION_MAJOR)\
|| (PARSON_VERSION_MINOR != PARSON_IMPL_VERSION_MINOR)\
|| (PARSON_VERSION_PATCH != PARSON_IMPL_VERSION_PATCH)
#error "parson version mismatch between parson.c and parson.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

/* Apparently sscanf is not implemented in some "standard" libraries, so don't use it, if you
 * don't have to. */
#ifdef sscanf
#undef sscanf
#define sscanf THINK_TWICE_ABOUT_USING_SSCANF
#endif

/* strcpy is unsafe */
#ifdef strcpy
#undef strcpy
#endif
#define strcpy USE_MEMCPY_INSTEAD_OF_STRCPY

#define STARTING_CAPACITY 16
#define MAX_NESTING       2048

#ifndef PARSON_DEFAULT_FLOAT_FORMAT
#define PARSON_DEFAULT_FLOAT_FORMAT "%1.17g" /* do not increase precision without incresing NUM_BUF_SIZE */
#endif

#ifndef PARSON_NUM_BUF_SIZE
#define PARSON_NUM_BUF_SIZE 64 /* double printed with "%1.17g" shouldn't be longer than 25 bytes so let's be paranoid and use 64 */
#endif

#ifndef PARSON_INDENT_STR
#define PARSON_INDENT_STR "    "
#endif

#define SIZEOF_TOKEN(a)       (sizeof(a) - 1)
#define SKIP_CHAR(str)        ((*str)++)
#define SKIP_WHITESPACES(str) while (isspace((unsigned char)(**str))) { SKIP_CHAR(str); }
#define MAX(a, b)             ((a) > (b) ? (a) : (b))

#undef malloc
#undef free

#if defined(isnan) && defined(isinf)
#define IS_NUMBER_INVALID(x) (isnan((x)) || isinf((x)))
#else
#define IS_NUMBER_INVALID(x) (((x) * 0.0) != 0.0)
#endif

#define OBJECT_INVALID_IX ((size_t)-1)

#define IS_CONT(b) (((unsigned char)(b) & 0xC0) == 0x80) /* is utf-8 continuation byte */

typedef int parson_bool_t;

#define PARSON_TRUE 1
#define PARSON_FALSE 0

typedef struct json_string {
    char *chars;
    size_t length;
} JSON_String;

/* Type definitions */
typedef union json_value_value {
    JSON_String  string;
    double       number;
    JSON_Object *object;
    JSON_Array  *array;
    int          boolean;
    int          null;
} JSON_Value_Value;

struct json_value_t {
    JSON_Value      *parent;
    JSON_Value_Type  type;
    JSON_Value_Value value;
};

struct json_object_t {
    JSON_Value    *wrapping_value;
    size_t        *cells;
    unsigned long *hashes;
    char         **names;
    JSON_Value   **values;
    size_t        *cell_ixs;
    size_t         count;
    size_t         item_capacity;
    size_t         cell_capacity;
};

struct json_array_t {
    JSON_Value  *wrapping_value;
    JSON_Value **items;
    size_t       count;
    size_t       capacity;
};

/* Memory */
static void * default_malloc(size_t size, void * userdata);
static void default_free(void * ptr, void * userdata);

/* Various */
static char * read_file(JSON_Parser const * parser, const char *filename);
static void   remove_comments(char *string, const char *start_token, const char *end_token);
static char * parson_strndup(JSON_Parser const * parser, const char *string, size_t n);
static char * parson_strdup(JSON_Parser const * parser, const char *string);
static int    hex_char_to_int(char c);
static JSON_Status parse_utf16_hex(const char *string, unsigned int *result);
static int         num_bytes_in_utf8_sequence(unsigned char c);
static JSON_Status   verify_utf8_sequence(const unsigned char *string, int *len);
static parson_bool_t is_valid_utf8(const char *string, size_t string_len);
static parson_bool_t is_decimal(const char *string, size_t length);
static unsigned long hash_string(const char *string, size_t n);

/* JSON Object */
static JSON_Object * json_object_make(JSON_Parser const * parser, JSON_Value *wrapping_value);
static JSON_Status   json_object_init(JSON_Parser const * parser, JSON_Object *object, size_t capacity);
static void          json_object_deinit(JSON_Parser const * parser, JSON_Object *object, parson_bool_t free_keys, parson_bool_t free_values);
static JSON_Status   json_object_grow_and_rehash(JSON_Parser const * parser, JSON_Object *object);
static size_t        json_object_get_cell_ix(const JSON_Object *object, const char *key, size_t key_len, unsigned long hash, parson_bool_t *out_found);
static JSON_Status   json_object_add(JSON_Parser const * parser, JSON_Object *object, char *name, JSON_Value *value);
static JSON_Value  * json_object_getn_value(const JSON_Object *object, const char *name, size_t name_len);
static JSON_Status   json_object_remove_internal(JSON_Parser const * parser, JSON_Object *object, const char *name, parson_bool_t free_value);
static JSON_Status   json_object_dotremove_internal(JSON_Parser const * parser, JSON_Object *object, const char *name, parson_bool_t free_value);
static void          json_object_free(JSON_Parser const * parser, JSON_Object *object);

/* JSON Array */
static JSON_Array * json_array_make(JSON_Parser const * parser, JSON_Value *wrapping_value);
static JSON_Status  json_array_add(JSON_Parser const * parser, JSON_Array *array, JSON_Value *value);
static JSON_Status  json_array_resize(JSON_Parser const * parser, JSON_Array *array, size_t new_capacity);
static void         json_array_free(JSON_Parser const * parser, JSON_Array *array);

/* JSON Value */
static JSON_Value * json_value_init_string_no_copy(JSON_Parser const * parser, char *string, size_t length);
static const JSON_String * json_value_get_string_desc(const JSON_Value *value);

/* Parser */
static JSON_Status   skip_quotes(const char **string);
static JSON_Status   parse_utf16(const char **unprocessed, char **processed);
static char *        process_string(JSON_Parser const * parser, const char *input, size_t input_len, size_t *output_len);
static char *        get_quoted_string(JSON_Parser const * parser, const char **string, size_t *output_string_len);
static JSON_Value *  parse_object_value(JSON_Parser const * parser, const char **string, size_t nesting);
static JSON_Value *  parse_array_value(JSON_Parser const * parser, const char **string, size_t nesting);
static JSON_Value *  parse_string_value(JSON_Parser const * parser, const char **string);
static JSON_Value *  parse_boolean_value(JSON_Parser const * parser, const char **string);
static JSON_Value *  parse_number_value(JSON_Parser const * parser, const char **string);
static JSON_Value *  parse_null_value(JSON_Parser const * parser, const char **string);
static JSON_Value *  parse_value(JSON_Parser const * parser, const char **string, size_t nesting);

/* Serialization */
static int json_serialize_to_buffer_r(JSON_Parser const * parser, const JSON_Value *value, char *buf, int level, parson_bool_t is_pretty, char *num_buf);
static int json_serialize_string(JSON_Parser const * parser, const char *string, size_t len, char *buf);

/* Memory */
static void * default_malloc(size_t size, void * userdata) {
    (void)userdata;
    return malloc(size);
}

static void default_free(void * ptr, void * userdata) {
    (void)userdata;
    free(ptr);
}

/* Various */
static char * read_file(JSON_Parser const * parser, const char * filename) {
    FILE *fp = fopen(filename, "r");
    size_t size_to_read = 0;
    size_t size_read = 0;
    long pos;
    char *file_contents;
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0L, SEEK_END);
    pos = ftell(fp);
    if (pos < 0) {
        fclose(fp);
        return NULL;
    }
    size_to_read = pos;
    rewind(fp);
    file_contents = (char*)parser->malloc_func(sizeof(char) * (size_to_read + 1), parser->malloc_userdata);
    if (!file_contents) {
        fclose(fp);
        return NULL;
    }
    size_read = fread(file_contents, 1, size_to_read, fp);
    if (size_read == 0 || ferror(fp)) {
        fclose(fp);
        parser->free_func(file_contents, parser->malloc_userdata);
        return NULL;
    }
    fclose(fp);
    file_contents[size_read] = '\0';
    return file_contents;
}

static void remove_comments(char *string, const char *start_token, const char *end_token) {
    parson_bool_t in_string = PARSON_FALSE, escaped = PARSON_FALSE;
    size_t i;
    char *ptr = NULL, current_char;
    size_t start_token_len = strlen(start_token);
    size_t end_token_len = strlen(end_token);
    if (start_token_len == 0 || end_token_len == 0) {
        return;
    }
    while ((current_char = *string) != '\0') {
        if (current_char == '\\' && !escaped) {
            escaped = PARSON_TRUE;
            string++;
            continue;
        } else if (current_char == '\"' && !escaped) {
            in_string = !in_string;
        } else if (!in_string && strncmp(string, start_token, start_token_len) == 0) {
            for(i = 0; i < start_token_len; i++) {
                string[i] = ' ';
            }
            string = string + start_token_len;
            ptr = strstr(string, end_token);
            if (!ptr) {
                return;
            }
            for (i = 0; i < (ptr - string) + end_token_len; i++) {
                string[i] = ' ';
            }
            string = ptr + end_token_len - 1;
        }
        escaped = PARSON_FALSE;
        string++;
    }
}

static char * parson_strndup(JSON_Parser const * parser, const char *string, size_t n) {
    /* We expect the caller has validated that 'n' fits within the input buffer. */
    char *output_string = (char*)parser->malloc_func(n + 1, parser->malloc_userdata);
    if (!output_string) {
        return NULL;
    }
    output_string[n] = '\0';
    memcpy(output_string, string, n);
    return output_string;
}

static char * parson_strdup(JSON_Parser const * parser, const char *string) {
    return parson_strndup(parser, string, strlen(string));
}

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static JSON_Status parse_utf16_hex(const char *s, unsigned int *result) {
    int x1, x2, x3, x4;
    if (s[0] == '\0' || s[1] == '\0' || s[2] == '\0' || s[3] == '\0') {
        return JSONFailure;
    }
    x1 = hex_char_to_int(s[0]);
    x2 = hex_char_to_int(s[1]);
    x3 = hex_char_to_int(s[2]);
    x4 = hex_char_to_int(s[3]);
    if (x1 == -1 || x2 == -1 || x3 == -1 || x4 == -1) {
        return JSONFailure;
    }
    *result = (unsigned int)((x1 << 12) | (x2 << 8) | (x3 << 4) | x4);
    return JSONSuccess;
}

static int num_bytes_in_utf8_sequence(unsigned char c) {
    if (c == 0xC0 || c == 0xC1 || c > 0xF4 || IS_CONT(c)) {
        return 0;
    } else if ((c & 0x80) == 0) {    /* 0xxxxxxx */
        return 1;
    } else if ((c & 0xE0) == 0xC0) { /* 110xxxxx */
        return 2;
    } else if ((c & 0xF0) == 0xE0) { /* 1110xxxx */
        return 3;
    } else if ((c & 0xF8) == 0xF0) { /* 11110xxx */
        return 4;
    }
    return 0; /* won't happen */
}

static JSON_Status verify_utf8_sequence(const unsigned char *string, int *len) {
    unsigned int cp = 0;
    *len = num_bytes_in_utf8_sequence(string[0]);

    if (*len == 1) {
        cp = string[0];
    } else if (*len == 2 && IS_CONT(string[1])) {
        cp = string[0] & 0x1F;
        cp = (cp << 6) | (string[1] & 0x3F);
    } else if (*len == 3 && IS_CONT(string[1]) && IS_CONT(string[2])) {
        cp = ((unsigned char)string[0]) & 0xF;
        cp = (cp << 6) | (string[1] & 0x3F);
        cp = (cp << 6) | (string[2] & 0x3F);
    } else if (*len == 4 && IS_CONT(string[1]) && IS_CONT(string[2]) && IS_CONT(string[3])) {
        cp = string[0] & 0x7;
        cp = (cp << 6) | (string[1] & 0x3F);
        cp = (cp << 6) | (string[2] & 0x3F);
        cp = (cp << 6) | (string[3] & 0x3F);
    } else {
        return JSONFailure;
    }

    /* overlong encodings */
    if ((cp < 0x80    && *len > 1) ||
        (cp < 0x800   && *len > 2) ||
        (cp < 0x10000 && *len > 3)) {
        return JSONFailure;
    }

    /* invalid unicode */
    if (cp > 0x10FFFF) {
        return JSONFailure;
    }

    /* surrogate halves */
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return JSONFailure;
    }

    return JSONSuccess;
}

static int is_valid_utf8(const char *string, size_t string_len) {
    int len = 0;
    const char *string_end =  string + string_len;
    while (string < string_end) {
        if (verify_utf8_sequence((const unsigned char*)string, &len) != JSONSuccess) {
            return PARSON_FALSE;
        }
        string += len;
    }
    return PARSON_TRUE;
}

static parson_bool_t is_decimal(const char *string, size_t length) {
    if (length > 1 && string[0] == '0' && string[1] != '.') {
        return PARSON_FALSE;
    }
    if (length > 2 && !strncmp(string, "-0", 2) && string[2] != '.') {
        return PARSON_FALSE;
    }
    while (length--) {
        if (strchr("xX", string[length])) {
            return PARSON_FALSE;
        }
    }
    return PARSON_TRUE;
}

static unsigned long hash_string(const char *string, size_t n) {
#ifdef PARSON_FORCE_HASH_COLLISIONS
    (void)string;
    (void)n;
    return 0;
#else
    unsigned long hash = 5381;
    unsigned char c;
    size_t i = 0;
    for (i = 0; i < n; i++) {
        c = string[i];
        if (c == '\0') {
            break;
        }
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
#endif
}

/* JSON Object */
static JSON_Object * json_object_make(JSON_Parser const * parser, JSON_Value *wrapping_value) {
    JSON_Status res = JSONFailure;
    JSON_Object *new_obj = (JSON_Object*)parser->malloc_func(sizeof(JSON_Object), parser->malloc_userdata);
    if (new_obj == NULL) {
        return NULL;
    }
    new_obj->wrapping_value = wrapping_value;
    res = json_object_init(parser, new_obj, 0);
    if (res != JSONSuccess) {
        parser->free_func(new_obj, parser->malloc_userdata);
        return NULL;
    }
    return new_obj;
}

static JSON_Status json_object_init(JSON_Parser const * parser, JSON_Object *object, size_t capacity) {
    unsigned int i = 0;

    object->cells = NULL;
    object->names = NULL;
    object->values = NULL;
    object->cell_ixs = NULL;
    object->hashes = NULL;

    object->count = 0;
    object->cell_capacity = capacity;
    object->item_capacity = (unsigned int)(capacity * 7/10);

    if (capacity == 0) {
        return JSONSuccess;
    }

    object->cells = (size_t*)parser->malloc_func(object->cell_capacity * sizeof(*object->cells), parser->malloc_userdata);
    object->names = (char**)parser->malloc_func(object->item_capacity * sizeof(*object->names), parser->malloc_userdata);
    object->values = (JSON_Value**)parser->malloc_func(object->item_capacity * sizeof(*object->values), parser->malloc_userdata);
    object->cell_ixs = (size_t*)parser->malloc_func(object->item_capacity * sizeof(*object->cell_ixs), parser->malloc_userdata);
    object->hashes = (unsigned long*)parser->malloc_func(object->item_capacity * sizeof(*object->hashes), parser->malloc_userdata);
    if (object->cells == NULL
        || object->names == NULL
        || object->values == NULL
        || object->cell_ixs == NULL
        || object->hashes == NULL) {
        goto error;
    }
    for (i = 0; i < object->cell_capacity; i++) {
        object->cells[i] = OBJECT_INVALID_IX;
    }
    return JSONSuccess;
error:
    parser->free_func(object->cells, parser->malloc_userdata);
    parser->free_func(object->names, parser->malloc_userdata);
    parser->free_func(object->values, parser->malloc_userdata);
    parser->free_func(object->cell_ixs, parser->malloc_userdata);
    parser->free_func(object->hashes, parser->malloc_userdata);
    return JSONFailure;
}

static void json_object_deinit(JSON_Parser const * parser, JSON_Object *object, parson_bool_t free_keys, parson_bool_t free_values) {
    unsigned int i = 0;
    for (i = 0; i < object->count; i++) {
        if (free_keys) {
            parser->free_func(object->names[i], parser->malloc_userdata);
        }
        if (free_values) {
            json_value_free(parser, object->values[i]);
        }
    }

    object->count = 0;
    object->item_capacity = 0;
    object->cell_capacity = 0;

    parser->free_func(object->cells, parser->malloc_userdata);
    parser->free_func(object->names, parser->malloc_userdata);
    parser->free_func(object->values, parser->malloc_userdata);
    parser->free_func(object->cell_ixs, parser->malloc_userdata);
    parser->free_func(object->hashes, parser->malloc_userdata);

    object->cells = NULL;
    object->names = NULL;
    object->values = NULL;
    object->cell_ixs = NULL;
    object->hashes = NULL;
}

static JSON_Status json_object_grow_and_rehash(JSON_Parser const * parser, JSON_Object *object) {
    JSON_Value *wrapping_value = NULL;
    JSON_Object new_object;
    char *key = NULL;
    JSON_Value *value = NULL;
    unsigned int i = 0;
    size_t new_capacity = MAX(object->cell_capacity * 2, STARTING_CAPACITY);
    JSON_Status res = json_object_init(parser, &new_object, new_capacity);
    if (res != JSONSuccess) {
        return JSONFailure;
    }

    wrapping_value = json_object_get_wrapping_value(object);
    new_object.wrapping_value = wrapping_value;

    for (i = 0; i < object->count; i++) {
        key = object->names[i];
        value = object->values[i];
        res = json_object_add(parser, &new_object, key, value);
        if (res != JSONSuccess) {
            json_object_deinit(parser, &new_object, PARSON_FALSE, PARSON_FALSE);
            return JSONFailure;
        }
        value->parent = wrapping_value;
    }
    json_object_deinit(parser, object, PARSON_FALSE, PARSON_FALSE);
    *object = new_object;
    return JSONSuccess;
}

static size_t json_object_get_cell_ix(const JSON_Object *object, const char *key, size_t key_len, unsigned long hash, parson_bool_t *out_found) {
    size_t cell_ix = hash & (object->cell_capacity - 1);
    size_t cell = 0;
    size_t ix = 0;
    unsigned int i = 0;
    unsigned long hash_to_check = 0;
    const char *key_to_check = NULL;
    size_t key_to_check_len = 0;

    *out_found = PARSON_FALSE;

    for (i = 0; i < object->cell_capacity; i++) {
        ix = (cell_ix + i) & (object->cell_capacity - 1);
        cell = object->cells[ix];
        if (cell == OBJECT_INVALID_IX) {
            return ix;
        }
        hash_to_check = object->hashes[cell];
        if (hash != hash_to_check) {
            continue;
        }
        key_to_check = object->names[cell];
        key_to_check_len = strlen(key_to_check);
        if (key_to_check_len == key_len && strncmp(key, key_to_check, key_len) == 0) {
            *out_found = PARSON_TRUE;
            return ix;
        }
    }
    return OBJECT_INVALID_IX;
}

static JSON_Status json_object_add(JSON_Parser const * parser, JSON_Object *object, char *name, JSON_Value *value) {
    unsigned long hash = 0;
    parson_bool_t found = PARSON_FALSE;
    size_t cell_ix = 0;
    JSON_Status res = JSONFailure;

    if (!object || !name || !value) {
        return JSONFailure;
    }

    hash = hash_string(name, strlen(name));
    found = PARSON_FALSE;
    cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
    if (found) {
        return JSONFailure;
    }

    if (object->count >= object->item_capacity) {
        res = json_object_grow_and_rehash(parser, object);
        if (res != JSONSuccess) {
            return JSONFailure;
        }
        cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
    }

    object->names[object->count] = name;
    object->cells[cell_ix] = object->count;
    object->values[object->count] = value;
    object->cell_ixs[object->count] = cell_ix;
    object->hashes[object->count] = hash;
    object->count++;
    value->parent = json_object_get_wrapping_value(object);

    return JSONSuccess;
}

static JSON_Value * json_object_getn_value(const JSON_Object *object, const char *name, size_t name_len) {
    unsigned long hash = 0;
    parson_bool_t found = PARSON_FALSE;
    size_t cell_ix = 0;
    size_t item_ix = 0;
    if (!object || !name) {
        return NULL;
    }
    hash = hash_string(name, name_len);
    found = PARSON_FALSE;
    cell_ix = json_object_get_cell_ix(object, name, name_len, hash, &found);
    if (!found) {
        return NULL;
    }
    item_ix = object->cells[cell_ix];
    return object->values[item_ix];
}

static JSON_Status json_object_remove_internal(JSON_Parser const * parser, JSON_Object *object, const char *name, parson_bool_t free_value) {
    unsigned long hash = 0;
    parson_bool_t found = PARSON_FALSE;
    size_t cell = 0;
    size_t item_ix = 0;
    size_t last_item_ix = 0;
    size_t i = 0;
    size_t j = 0;
    size_t x = 0;
    size_t k = 0;
    JSON_Value *val = NULL;

    if (object == NULL) {
        return JSONFailure;
    }

    hash = hash_string(name, strlen(name));
    found = PARSON_FALSE;
    cell = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
    if (!found) {
        return JSONFailure;
    }

    item_ix = object->cells[cell];
    if (free_value) {
        val = object->values[item_ix];
        json_value_free(parser, val);
        val = NULL;
    }

    parser->free_func(object->names[item_ix], parser->malloc_userdata);
    last_item_ix = object->count - 1;
    if (item_ix < last_item_ix) {
        object->names[item_ix] = object->names[last_item_ix];
        object->values[item_ix] = object->values[last_item_ix];
        object->cell_ixs[item_ix] = object->cell_ixs[last_item_ix];
        object->hashes[item_ix] = object->hashes[last_item_ix];
        object->cells[object->cell_ixs[item_ix]] = item_ix;
    }
    object->count--;

    i = cell;
    j = i;
    for (x = 0; x < (object->cell_capacity - 1); x++) {
        j = (j + 1) & (object->cell_capacity - 1);
        if (object->cells[j] == OBJECT_INVALID_IX) {
            break;
        }
        k = object->hashes[object->cells[j]] & (object->cell_capacity - 1);
        if ((j > i && (k <= i || k > j))
         || (j < i && (k <= i && k > j))) {
            object->cell_ixs[object->cells[j]] = i;
            object->cells[i] = object->cells[j];
            i = j;
        }
    }
    object->cells[i] = OBJECT_INVALID_IX;
    return JSONSuccess;
}

static JSON_Status json_object_dotremove_internal(JSON_Parser const * parser, JSON_Object *object, const char *name, parson_bool_t free_value) {
    JSON_Value *temp_value = NULL;
    JSON_Object *temp_object = NULL;
    const char *dot_pos = strchr(name, '.');
    if (!dot_pos) {
        return json_object_remove_internal(parser, object, name, free_value);
    }
    temp_value = json_object_getn_value(object, name, dot_pos - name);
    if (json_value_get_type(temp_value) != JSONObject) {
        return JSONFailure;
    }
    temp_object = json_value_get_object(temp_value);
    return json_object_dotremove_internal(parser, temp_object, dot_pos + 1, free_value);
}

static void json_object_free(JSON_Parser const * parser, JSON_Object *object) {
    json_object_deinit(parser, object, PARSON_TRUE, PARSON_TRUE);
    parser->free_func(object, parser->malloc_userdata);
}

/* JSON Array */
static JSON_Array * json_array_make(JSON_Parser const * parser, JSON_Value *wrapping_value) {
    JSON_Array *new_array = (JSON_Array*)parser->malloc_func(sizeof(JSON_Array), parser->malloc_userdata);
    if (new_array == NULL) {
        return NULL;
    }
    new_array->wrapping_value = wrapping_value;
    new_array->items = (JSON_Value**)NULL;
    new_array->capacity = 0;
    new_array->count = 0;
    return new_array;
}

static JSON_Status json_array_add(JSON_Parser const * parser, JSON_Array *array, JSON_Value *value) {
    if (array->count >= array->capacity) {
        size_t new_capacity = MAX(array->capacity * 2, STARTING_CAPACITY);
        if (json_array_resize(parser, array, new_capacity) != JSONSuccess) {
            return JSONFailure;
        }
    }
    value->parent = json_array_get_wrapping_value(array);
    array->items[array->count] = value;
    array->count++;
    return JSONSuccess;
}

static JSON_Status json_array_resize(JSON_Parser const * parser, JSON_Array *array, size_t new_capacity) {
    JSON_Value **new_items = NULL;
    if (new_capacity == 0) {
        return JSONFailure;
    }
    new_items = (JSON_Value**)parser->malloc_func(new_capacity * sizeof(JSON_Value*), parser->malloc_userdata);
    if (new_items == NULL) {
        return JSONFailure;
    }
    if (array->items != NULL && array->count > 0) {
        memcpy(new_items, array->items, array->count * sizeof(JSON_Value*));
    }
    parser->free_func(array->items, parser->malloc_userdata);
    array->items = new_items;
    array->capacity = new_capacity;
    return JSONSuccess;
}

static void json_array_free(JSON_Parser const * parser, JSON_Array *array) {
    size_t i;
    for (i = 0; i < array->count; i++) {
        json_value_free(parser, array->items[i]);
    }
    parser->free_func(array->items, parser->malloc_userdata);
    parser->free_func(array, parser->malloc_userdata);
}

/* JSON Value */
static JSON_Value * json_value_init_string_no_copy(JSON_Parser const * parser, char *string, size_t length) {
    JSON_Value *new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (!new_value) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONString;
    new_value->value.string.chars = string;
    new_value->value.string.length = length;
    return new_value;
}

/* Parser */
static JSON_Status skip_quotes(const char **string) {
    if (**string != '\"') {
        return JSONFailure;
    }
    SKIP_CHAR(string);
    while (**string != '\"') {
        if (**string == '\0') {
            return JSONFailure;
        } else if (**string == '\\') {
            SKIP_CHAR(string);
            if (**string == '\0') {
                return JSONFailure;
            }
        }
        SKIP_CHAR(string);
    }
    SKIP_CHAR(string);
    return JSONSuccess;
}

static JSON_Status parse_utf16(const char **unprocessed, char **processed) {
    unsigned int cp, lead, trail;
    char *processed_ptr = *processed;
    const char *unprocessed_ptr = *unprocessed;
    JSON_Status status = JSONFailure;
    unprocessed_ptr++; /* skips u */
    status = parse_utf16_hex(unprocessed_ptr, &cp);
    if (status != JSONSuccess) {
        return JSONFailure;
    }
    if (cp < 0x80) {
        processed_ptr[0] = (char)cp; /* 0xxxxxxx */
    } else if (cp < 0x800) {
        processed_ptr[0] = ((cp >> 6) & 0x1F) | 0xC0; /* 110xxxxx */
        processed_ptr[1] = ((cp)      & 0x3F) | 0x80; /* 10xxxxxx */
        processed_ptr += 1;
    } else if (cp < 0xD800 || cp > 0xDFFF) {
        processed_ptr[0] = ((cp >> 12) & 0x0F) | 0xE0; /* 1110xxxx */
        processed_ptr[1] = ((cp >> 6)  & 0x3F) | 0x80; /* 10xxxxxx */
        processed_ptr[2] = ((cp)       & 0x3F) | 0x80; /* 10xxxxxx */
        processed_ptr += 2;
    } else if (cp >= 0xD800 && cp <= 0xDBFF) { /* lead surrogate (0xD800..0xDBFF) */
        lead = cp;
        unprocessed_ptr += 4; /* should always be within the buffer, otherwise previous sscanf would fail */
        if (*unprocessed_ptr++ != '\\' || *unprocessed_ptr++ != 'u') {
            return JSONFailure;
        }
        status = parse_utf16_hex(unprocessed_ptr, &trail);
        if (status != JSONSuccess || trail < 0xDC00 || trail > 0xDFFF) { /* valid trail surrogate? (0xDC00..0xDFFF) */
            return JSONFailure;
        }
        cp = ((((lead - 0xD800) & 0x3FF) << 10) | ((trail - 0xDC00) & 0x3FF)) + 0x010000;
        processed_ptr[0] = (((cp >> 18) & 0x07) | 0xF0); /* 11110xxx */
        processed_ptr[1] = (((cp >> 12) & 0x3F) | 0x80); /* 10xxxxxx */
        processed_ptr[2] = (((cp >> 6)  & 0x3F) | 0x80); /* 10xxxxxx */
        processed_ptr[3] = (((cp)       & 0x3F) | 0x80); /* 10xxxxxx */
        processed_ptr += 3;
    } else { /* trail surrogate before lead surrogate */
        return JSONFailure;
    }
    unprocessed_ptr += 3;
    *processed = processed_ptr;
    *unprocessed = unprocessed_ptr;
    return JSONSuccess;
}


/* Copies and processes passed string up to supplied length.
Example: "\u006Corem ipsum" -> lorem ipsum */
static char* process_string(JSON_Parser const * parser, const char *input, size_t input_len, size_t *output_len) {
    const char *input_ptr = input;
    size_t initial_size = (input_len + 1) * sizeof(char);
    size_t final_size = 0;
    char *output = NULL, *output_ptr = NULL, *resized_output = NULL;
    output = (char*)parser->malloc_func(initial_size, parser->malloc_userdata);
    if (output == NULL) {
        goto error;
    }
    output_ptr = output;
    while ((*input_ptr != '\0') && (size_t)(input_ptr - input) < input_len) {
        if (*input_ptr == '\\') {
            input_ptr++;
            switch (*input_ptr) {
                case '\"': *output_ptr = '\"'; break;
                case '\\': *output_ptr = '\\'; break;
                case '/':  *output_ptr = '/';  break;
                case 'b':  *output_ptr = '\b'; break;
                case 'f':  *output_ptr = '\f'; break;
                case 'n':  *output_ptr = '\n'; break;
                case 'r':  *output_ptr = '\r'; break;
                case 't':  *output_ptr = '\t'; break;
                case 'u':
                    if (parse_utf16(&input_ptr, &output_ptr) != JSONSuccess) {
                        goto error;
                    }
                    break;
                default:
                    goto error;
            }
        } else if ((unsigned char)*input_ptr < 0x20) {
            goto error; /* 0x00-0x19 are invalid characters for json string (http://www.ietf.org/rfc/rfc4627.txt) */
        } else {
            *output_ptr = *input_ptr;
        }
        output_ptr++;
        input_ptr++;
    }
    *output_ptr = '\0';
    /* resize to new length */
    final_size = (size_t)(output_ptr-output) + 1;
    /* todo: don't resize if final_size == initial_size */
    resized_output = (char*)parser->malloc_func(final_size, parser->malloc_userdata);
    if (resized_output == NULL) {
        goto error;
    }
    memcpy(resized_output, output, final_size);
    *output_len = final_size - 1;
    parser->free_func(output, parser->malloc_userdata);
    return resized_output;
error:
    parser->free_func(output, parser->malloc_userdata);
    return NULL;
}

/* Return processed contents of a string between quotes and
   skips passed argument to a matching quote. */
static char * get_quoted_string(JSON_Parser const * parser, const char **string, size_t *output_string_len) {
    const char *string_start = *string;
    size_t input_string_len = 0;
    JSON_Status status = skip_quotes(string);
    if (status != JSONSuccess) {
        return NULL;
    }
    input_string_len = *string - string_start - 2; /* length without quotes */
    return process_string(parser, string_start + 1, input_string_len, output_string_len);
}

static JSON_Value * parse_value(JSON_Parser const * parser, const char **string, size_t nesting) {
    if (nesting > MAX_NESTING) {
        return NULL;
    }
    SKIP_WHITESPACES(string);
    switch (**string) {
        case '{':
            return parse_object_value(parser, string, nesting + 1);
        case '[':
            return parse_array_value(parser, string, nesting + 1);
        case '\"':
            return parse_string_value(parser, string);
        case 'f': case 't':
            return parse_boolean_value(parser, string);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number_value(parser, string);
        case 'n':
            return parse_null_value(parser, string);
        default:
            return NULL;
    }
}

static JSON_Value * parse_object_value(JSON_Parser const * parser, const char **string, size_t nesting) {
    JSON_Status status = JSONFailure;
    JSON_Value *output_value = NULL, *new_value = NULL;
    JSON_Object *output_object = NULL;
    char *new_key = NULL;

    output_value = json_value_init_object(parser);
    if (output_value == NULL) {
        return NULL;
    }
    if (**string != '{') {
        json_value_free(parser, output_value);
        return NULL;
    }
    output_object = json_value_get_object(output_value);
    SKIP_CHAR(string);
    SKIP_WHITESPACES(string);
    if (**string == '}') { /* empty object */
        SKIP_CHAR(string);
        return output_value;
    }
    while (**string != '\0') {
        size_t key_len = 0;
        new_key = get_quoted_string(parser, string, &key_len);
        /* We do not support key names with embedded \0 chars */
        if (!new_key) {
            json_value_free(parser, output_value);
            return NULL;
        }
        if (key_len != strlen(new_key)) {
            parser->free_func(new_key, parser->malloc_userdata);
            json_value_free(parser, output_value);
            return NULL;
        }
        SKIP_WHITESPACES(string);
        if (**string != ':') {
            parser->free_func(new_key, parser->malloc_userdata);
            json_value_free(parser, output_value);
            return NULL;
        }
        SKIP_CHAR(string);
        new_value = parse_value(parser, string, nesting);
        if (new_value == NULL) {
            parser->free_func(new_key, parser->malloc_userdata);
            json_value_free(parser, output_value);
            return NULL;
        }
        status = json_object_add(parser, output_object, new_key, new_value);
        if (status != JSONSuccess) {
            parser->free_func(new_key, parser->malloc_userdata);
            json_value_free(parser, new_value);
            json_value_free(parser, output_value);
            return NULL;
        }
        SKIP_WHITESPACES(string);
        if (**string != ',') {
            break;
        }
        SKIP_CHAR(string);
        SKIP_WHITESPACES(string);
        if (**string == '}') {
            break;
        }
    }
    SKIP_WHITESPACES(string);
    if (**string != '}') {
        json_value_free(parser, output_value);
        return NULL;
    }
    SKIP_CHAR(string);
    return output_value;
}

static JSON_Value * parse_array_value(JSON_Parser const * parser, const char **string, size_t nesting) {
    JSON_Value *output_value = NULL, *new_array_value = NULL;
    JSON_Array *output_array = NULL;
    output_value = json_value_init_array(parser);
    if (output_value == NULL) {
        return NULL;
    }
    if (**string != '[') {
        json_value_free(parser, output_value);
        return NULL;
    }
    output_array = json_value_get_array(output_value);
    SKIP_CHAR(string);
    SKIP_WHITESPACES(string);
    if (**string == ']') { /* empty array */
        SKIP_CHAR(string);
        return output_value;
    }
    while (**string != '\0') {
        new_array_value = parse_value(parser, string, nesting);
        if (new_array_value == NULL) {
            json_value_free(parser, output_value);
            return NULL;
        }
        if (json_array_add(parser, output_array, new_array_value) != JSONSuccess) {
            json_value_free(parser, new_array_value);
            json_value_free(parser, output_value);
            return NULL;
        }
        SKIP_WHITESPACES(string);
        if (**string != ',') {
            break;
        }
        SKIP_CHAR(string);
        SKIP_WHITESPACES(string);
        if (**string == ']') {
            break;
        }
    }
    SKIP_WHITESPACES(string);
    if (**string != ']' || /* Trim array after parsing is over */
        json_array_resize(parser, output_array, json_array_get_count(output_array)) != JSONSuccess) {
            json_value_free(parser, output_value);
            return NULL;
    }
    SKIP_CHAR(string);
    return output_value;
}

static JSON_Value * parse_string_value(JSON_Parser const * parser, const char **string) {
    JSON_Value *value = NULL;
    size_t new_string_len = 0;
    char *new_string = get_quoted_string(parser, string, &new_string_len);
    if (new_string == NULL) {
        return NULL;
    }
    value = json_value_init_string_no_copy(parser, new_string, new_string_len);
    if (value == NULL) {
        parser->free_func(new_string, parser->malloc_userdata);
        return NULL;
    }
    return value;
}

static JSON_Value * parse_boolean_value(JSON_Parser const * parser, const char **string) {
    size_t true_token_size = SIZEOF_TOKEN("true");
    size_t false_token_size = SIZEOF_TOKEN("false");
    if (strncmp("true", *string, true_token_size) == 0) {
        *string += true_token_size;
        return json_value_init_boolean(parser, 1);
    } else if (strncmp("false", *string, false_token_size) == 0) {
        *string += false_token_size;
        return json_value_init_boolean(parser, 0);
    }
    return NULL;
}

static JSON_Value * parse_number_value(JSON_Parser const * parser, const char **string) {
    char *end;
    double number = 0;
    errno = 0;
    number = strtod(*string, &end);
    if (errno == ERANGE && (number <= -HUGE_VAL || number >= HUGE_VAL)) {
        return NULL;
    }
    if ((errno && errno != ERANGE) || !is_decimal(*string, end - *string)) {
        return NULL;
    }
    *string = end;
    return json_value_init_number(parser, number);
}

static JSON_Value * parse_null_value(JSON_Parser const * parser, const char **string) {
    size_t token_size = SIZEOF_TOKEN("null");
    if (strncmp("null", *string, token_size) == 0) {
        *string += token_size;
        return json_value_init_null(parser);
    }
    return NULL;
}

/* Serialization */

/*  APPEND_STRING() is only called on string literals.
    It's a bit hacky because it makes plenty of assumptions about the external state
    and should eventually be tidied up into a function (same goes for APPEND_INDENT)
 */
#define APPEND_STRING(str) do {\
                                written = SIZEOF_TOKEN((str));\
                                if (buf != NULL) {\
                                    memcpy(buf, (str), written);\
                                    buf[written] = '\0';\
                                    buf += written;\
                                }\
                                written_total += written;\
                            } while (0)

#define APPEND_INDENT(level) do {\
                                int level_i = 0;\
                                for (level_i = 0; level_i < (level); level_i++) {\
                                    APPEND_STRING(PARSON_INDENT_STR);\
                                }\
                            } while (0)

static int json_serialize_to_buffer_r(JSON_Parser const * parser, const JSON_Value *value, char *buf, int level, parson_bool_t is_pretty, char *num_buf)
{
    const char *key = NULL, *string = NULL;
    JSON_Value *temp_value = NULL;
    JSON_Array *array = NULL;
    JSON_Object *object = NULL;
    size_t i = 0, count = 0;
    double num = 0.0;
    int written = -1, written_total = 0;
    size_t len = 0;

    switch (json_value_get_type(value)) {
        case JSONArray:
            array = json_value_get_array(value);
            count = json_array_get_count(array);
            APPEND_STRING("[");
            if (count > 0 && is_pretty) {
                APPEND_STRING("\n");
            }
            for (i = 0; i < count; i++) {
                if (is_pretty) {
                    APPEND_INDENT(level+1);
                }
                temp_value = json_array_get_value(array, i);
                written = json_serialize_to_buffer_r(parser, temp_value, buf, level+1, is_pretty, num_buf);
                if (written < 0) {
                    return -1;
                }
                if (buf != NULL) {
                    buf += written;
                }
                written_total += written;
                if (i < (count - 1)) {
                    APPEND_STRING(",");
                }
                if (is_pretty) {
                    APPEND_STRING("\n");
                }
            }
            if (count > 0 && is_pretty) {
                APPEND_INDENT(level);
            }
            APPEND_STRING("]");
            return written_total;
        case JSONObject:
            object = json_value_get_object(value);
            count  = json_object_get_count(object);
            APPEND_STRING("{");
            if (count > 0 && is_pretty) {
                APPEND_STRING("\n");
            }
            for (i = 0; i < count; i++) {
                key = json_object_get_name(object, i);
                if (key == NULL) {
                    return -1;
                }
                if (is_pretty) {
                    APPEND_INDENT(level+1);
                }
                /* We do not support key names with embedded \0 chars */
                written = json_serialize_string(parser, key, strlen(key), buf);
                if (written < 0) {
                    return -1;
                }
                if (buf != NULL) {
                    buf += written;
                }
                written_total += written;
                APPEND_STRING(":");
                if (is_pretty) {
                    APPEND_STRING(" ");
                }
                temp_value = json_object_get_value_at(object, i);
                written = json_serialize_to_buffer_r(parser, temp_value, buf, level+1, is_pretty, num_buf);
                if (written < 0) {
                    return -1;
                }
                if (buf != NULL) {
                    buf += written;
                }
                written_total += written;
                if (i < (count - 1)) {
                    APPEND_STRING(",");
                }
                if (is_pretty) {
                    APPEND_STRING("\n");
                }
            }
            if (count > 0 && is_pretty) {
                APPEND_INDENT(level);
            }
            APPEND_STRING("}");
            return written_total;
        case JSONString:
            string = json_value_get_string(value);
            if (string == NULL) {
                return -1;
            }
            len = json_value_get_string_len(value);
            written = json_serialize_string(parser, string, len, buf);
            if (written < 0) {
                return -1;
            }
            if (buf != NULL) {
                buf += written;
            }
            written_total += written;
            return written_total;
        case JSONBoolean:
            if (json_value_get_boolean(value)) {
                APPEND_STRING("true");
            } else {
                APPEND_STRING("false");
            }
            return written_total;
        case JSONNumber:
            num = json_value_get_number(value);
            if (buf != NULL) {
                num_buf = buf;
            }
            if (parser->number_serialization_func) {
                written = parser->number_serialization_func(num, num_buf);
            } else if (parser->float_fmt_str) {
                written = sprintf(num_buf, parser->float_fmt_str, num);
            } else {
                written = sprintf(num_buf, PARSON_DEFAULT_FLOAT_FORMAT, num);
            }
            if (written < 0) {
                return -1;
            }
            if (buf != NULL) {
                buf += written;
            }
            written_total += written;
            return written_total;
        case JSONNull:
            APPEND_STRING("null");
            return written_total;
        case JSONError:
            return -1;
        default:
            return -1;
    }
}

static int json_serialize_string(JSON_Parser const * parser, const char *string, size_t len, char *buf) {
    size_t i = 0;
    char c = '\0';
    int written = -1, written_total = 0;
    APPEND_STRING("\"");
    for (i = 0; i < len; i++) {
        c = string[i];
        switch (c) {
            case '\"': APPEND_STRING("\\\""); break;
            case '\\': APPEND_STRING("\\\\"); break;
            case '\b': APPEND_STRING("\\b"); break;
            case '\f': APPEND_STRING("\\f"); break;
            case '\n': APPEND_STRING("\\n"); break;
            case '\r': APPEND_STRING("\\r"); break;
            case '\t': APPEND_STRING("\\t"); break;
            case '\x00': APPEND_STRING("\\u0000"); break;
            case '\x01': APPEND_STRING("\\u0001"); break;
            case '\x02': APPEND_STRING("\\u0002"); break;
            case '\x03': APPEND_STRING("\\u0003"); break;
            case '\x04': APPEND_STRING("\\u0004"); break;
            case '\x05': APPEND_STRING("\\u0005"); break;
            case '\x06': APPEND_STRING("\\u0006"); break;
            case '\x07': APPEND_STRING("\\u0007"); break;
            /* '\x08' duplicate: '\b' */
            /* '\x09' duplicate: '\t' */
            /* '\x0a' duplicate: '\n' */
            case '\x0b': APPEND_STRING("\\u000b"); break;
            /* '\x0c' duplicate: '\f' */
            /* '\x0d' duplicate: '\r' */
            case '\x0e': APPEND_STRING("\\u000e"); break;
            case '\x0f': APPEND_STRING("\\u000f"); break;
            case '\x10': APPEND_STRING("\\u0010"); break;
            case '\x11': APPEND_STRING("\\u0011"); break;
            case '\x12': APPEND_STRING("\\u0012"); break;
            case '\x13': APPEND_STRING("\\u0013"); break;
            case '\x14': APPEND_STRING("\\u0014"); break;
            case '\x15': APPEND_STRING("\\u0015"); break;
            case '\x16': APPEND_STRING("\\u0016"); break;
            case '\x17': APPEND_STRING("\\u0017"); break;
            case '\x18': APPEND_STRING("\\u0018"); break;
            case '\x19': APPEND_STRING("\\u0019"); break;
            case '\x1a': APPEND_STRING("\\u001a"); break;
            case '\x1b': APPEND_STRING("\\u001b"); break;
            case '\x1c': APPEND_STRING("\\u001c"); break;
            case '\x1d': APPEND_STRING("\\u001d"); break;
            case '\x1e': APPEND_STRING("\\u001e"); break;
            case '\x1f': APPEND_STRING("\\u001f"); break;
            case '/':
                if (parser->escape_slashes) {
                    APPEND_STRING("\\/");  /* to make json embeddable in xml\/html */
                } else {
                    APPEND_STRING("/");
                }
                break;
            default:
                if (buf != NULL) {
                    buf[0] = c;
                    buf += 1;
                }
                written_total += 1;
                break;
        }
    }
    APPEND_STRING("\"");
    return written_total;
}

#undef APPEND_STRING
#undef APPEND_INDENT

/* Parser API */
JSON_Value * json_parse_file(JSON_Parser const * parser, const char *filename) {
    char *file_contents = read_file(parser, filename);
    JSON_Value *output_value = NULL;
    if (file_contents == NULL) {
        return NULL;
    }
    output_value = json_parse_string(parser, file_contents);
    parser->free_func(file_contents, parser->malloc_userdata);
    return output_value;
}

JSON_Value * json_parse_file_with_comments(JSON_Parser const * parser, const char *filename) {
    char *file_contents = read_file(parser, filename);
    JSON_Value *output_value = NULL;
    if (file_contents == NULL) {
        return NULL;
    }
    output_value = json_parse_string_with_comments(parser, file_contents);
    parser->free_func(file_contents, parser->malloc_userdata);
    return output_value;
}

JSON_Value * json_parse_string(JSON_Parser const * parser, const char *string) {
    if (string == NULL) {
        return NULL;
    }
    if (string[0] == '\xEF' && string[1] == '\xBB' && string[2] == '\xBF') {
        string = string + 3; /* Support for UTF-8 BOM */
    }
    return parse_value(parser, (const char**)&string, 0);
}

JSON_Value * json_parse_string_with_comments(JSON_Parser const * parser, const char *string) {
    JSON_Value *result = NULL;
    char *string_mutable_copy = NULL, *string_mutable_copy_ptr = NULL;
    string_mutable_copy = parson_strdup(parser, string);
    if (string_mutable_copy == NULL) {
        return NULL;
    }
    remove_comments(string_mutable_copy, "/*", "*/");
    remove_comments(string_mutable_copy, "//", "\n");
    string_mutable_copy_ptr = string_mutable_copy;
    result = parse_value(parser, (const char**)&string_mutable_copy_ptr, 0);
    parser->free_func(string_mutable_copy, parser->malloc_userdata);
    return result;
}

/* JSON Object API */

JSON_Value * json_object_get_value(const JSON_Object *object, const char *name) {
    if (object == NULL || name == NULL) {
        return NULL;
    }
    return json_object_getn_value(object, name, strlen(name));
}

const char * json_object_get_string(const JSON_Object *object, const char *name) {
    return json_value_get_string(json_object_get_value(object, name));
}

size_t json_object_get_string_len(const JSON_Object *object, const char *name) {
    return json_value_get_string_len(json_object_get_value(object, name));
}

double json_object_get_number(const JSON_Object *object, const char *name) {
    return json_value_get_number(json_object_get_value(object, name));
}

JSON_Object * json_object_get_object(const JSON_Object *object, const char *name) {
    return json_value_get_object(json_object_get_value(object, name));
}

JSON_Array * json_object_get_array(const JSON_Object *object, const char *name) {
    return json_value_get_array(json_object_get_value(object, name));
}

int json_object_get_boolean(const JSON_Object *object, const char *name) {
    return json_value_get_boolean(json_object_get_value(object, name));
}

JSON_Value * json_object_dotget_value(const JSON_Object *object, const char *name) {
    const char *dot_position = strchr(name, '.');
    if (!dot_position) {
        return json_object_get_value(object, name);
    }
    object = json_value_get_object(json_object_getn_value(object, name, dot_position - name));
    return json_object_dotget_value(object, dot_position + 1);
}

const char * json_object_dotget_string(const JSON_Object *object, const char *name) {
    return json_value_get_string(json_object_dotget_value(object, name));
}

size_t json_object_dotget_string_len(const JSON_Object *object, const char *name) {
    return json_value_get_string_len(json_object_dotget_value(object, name));
}

double json_object_dotget_number(const JSON_Object *object, const char *name) {
    return json_value_get_number(json_object_dotget_value(object, name));
}

JSON_Object * json_object_dotget_object(const JSON_Object *object, const char *name) {
    return json_value_get_object(json_object_dotget_value(object, name));
}

JSON_Array * json_object_dotget_array(const JSON_Object *object, const char *name) {
    return json_value_get_array(json_object_dotget_value(object, name));
}

int json_object_dotget_boolean(const JSON_Object *object, const char *name) {
    return json_value_get_boolean(json_object_dotget_value(object, name));
}

size_t json_object_get_count(const JSON_Object *object) {
    return object ? object->count : 0;
}

const char * json_object_get_name(const JSON_Object *object, size_t index) {
    if (object == NULL || index >= json_object_get_count(object)) {
        return NULL;
    }
    return object->names[index];
}

JSON_Value * json_object_get_value_at(const JSON_Object *object, size_t index) {
    if (object == NULL || index >= json_object_get_count(object)) {
        return NULL;
    }
    return object->values[index];
}

JSON_Value *json_object_get_wrapping_value(const JSON_Object *object) {
    if (!object) {
        return NULL;
    }
    return object->wrapping_value;
}

int json_object_has_value (const JSON_Object *object, const char *name) {
    return json_object_get_value(object, name) != NULL;
}

int json_object_has_value_of_type(const JSON_Object *object, const char *name, JSON_Value_Type type) {
    JSON_Value *val = json_object_get_value(object, name);
    return val != NULL && json_value_get_type(val) == type;
}

int json_object_dothas_value (const JSON_Object *object, const char *name) {
    return json_object_dotget_value(object, name) != NULL;
}

int json_object_dothas_value_of_type(const JSON_Object *object, const char *name, JSON_Value_Type type) {
    JSON_Value *val = json_object_dotget_value(object, name);
    return val != NULL && json_value_get_type(val) == type;
}

/* JSON Array API */
JSON_Value * json_array_get_value(const JSON_Array *array, size_t index) {
    if (array == NULL || index >= json_array_get_count(array)) {
        return NULL;
    }
    return array->items[index];
}

const char * json_array_get_string(const JSON_Array *array, size_t index) {
    return json_value_get_string(json_array_get_value(array, index));
}

size_t json_array_get_string_len(const JSON_Array *array, size_t index) {
    return json_value_get_string_len(json_array_get_value(array, index));
}

double json_array_get_number(const JSON_Array *array, size_t index) {
    return json_value_get_number(json_array_get_value(array, index));
}

JSON_Object * json_array_get_object(const JSON_Array *array, size_t index) {
    return json_value_get_object(json_array_get_value(array, index));
}

JSON_Array * json_array_get_array(const JSON_Array *array, size_t index) {
    return json_value_get_array(json_array_get_value(array, index));
}

int json_array_get_boolean(const JSON_Array *array, size_t index) {
    return json_value_get_boolean(json_array_get_value(array, index));
}

size_t json_array_get_count(const JSON_Array *array) {
    return array ? array->count : 0;
}

JSON_Value * json_array_get_wrapping_value(const JSON_Array *array) {
    if (!array) {
        return NULL;
    }
    return array->wrapping_value;
}

/* JSON Value API */
JSON_Value_Type json_value_get_type(const JSON_Value *value) {
    return value ? value->type : JSONError;
}

JSON_Object * json_value_get_object(const JSON_Value *value) {
    return json_value_get_type(value) == JSONObject ? value->value.object : NULL;
}

JSON_Array * json_value_get_array(const JSON_Value *value) {
    return json_value_get_type(value) == JSONArray ? value->value.array : NULL;
}

static const JSON_String * json_value_get_string_desc(const JSON_Value *value) {
    return json_value_get_type(value) == JSONString ? &value->value.string : NULL;
}

const char * json_value_get_string(const JSON_Value *value) {
    const JSON_String *str = json_value_get_string_desc(value);
    return str ? str->chars : NULL;
}

size_t json_value_get_string_len(const JSON_Value *value) {
    const JSON_String *str = json_value_get_string_desc(value);
    return str ? str->length : 0;
}

double json_value_get_number(const JSON_Value *value) {
    return json_value_get_type(value) == JSONNumber ? value->value.number : 0;
}

int json_value_get_boolean(const JSON_Value *value) {
    return json_value_get_type(value) == JSONBoolean ? value->value.boolean : -1;
}

JSON_Value * json_value_get_parent (const JSON_Value *value) {
    return value ? value->parent : NULL;
}

void json_value_free(JSON_Parser const * parser, JSON_Value *value) {
    switch (json_value_get_type(value)) {
        case JSONObject:
            json_object_free(parser, value->value.object);
            break;
        case JSONString:
            parser->free_func(value->value.string.chars, parser->malloc_userdata);
            break;
        case JSONArray:
            json_array_free(parser, value->value.array);
            break;
        default:
            break;
    }
    parser->free_func(value, parser->malloc_userdata);
}

JSON_Value * json_value_init_object(JSON_Parser const * parser) {
    JSON_Value *new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (!new_value) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONObject;
    new_value->value.object = json_object_make(parser, new_value);
    if (!new_value->value.object) {
        parser->free_func(new_value, parser->malloc_userdata);
        return NULL;
    }
    return new_value;
}

JSON_Value * json_value_init_array(JSON_Parser const * parser) {
    JSON_Value *new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (!new_value) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONArray;
    new_value->value.array = json_array_make(parser, new_value);
    if (!new_value->value.array) {
        parser->free_func(new_value, parser->malloc_userdata);
        return NULL;
    }
    return new_value;
}

JSON_Value * json_value_init_string(JSON_Parser const * parser, const char *string) {
    if (string == NULL) {
        return NULL;
    }
    return json_value_init_string_with_len(parser, string, strlen(string));
}

JSON_Value * json_value_init_string_with_len(JSON_Parser const * parser, const char *string, size_t length) {
    char *copy = NULL;
    JSON_Value *value;
    if (string == NULL) {
        return NULL;
    }
    if (!is_valid_utf8(string, length)) {
        return NULL;
    }
    copy = parson_strndup(parser, string, length);
    if (copy == NULL) {
        return NULL;
    }
    value = json_value_init_string_no_copy(parser, copy, length);
    if (value == NULL) {
        parser->free_func(copy, parser->malloc_userdata);
    }
    return value;
}

JSON_Value * json_value_init_number(JSON_Parser const * parser, double number) {
    JSON_Value *new_value = NULL;
    if (IS_NUMBER_INVALID(number)) {
        return NULL;
    }
    new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (new_value == NULL) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONNumber;
    new_value->value.number = number;
    return new_value;
}

JSON_Value * json_value_init_boolean(JSON_Parser const * parser, int boolean) {
    JSON_Value *new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (!new_value) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONBoolean;
    new_value->value.boolean = boolean ? 1 : 0;
    return new_value;
}

JSON_Value * json_value_init_null(JSON_Parser const * parser) {
    JSON_Value *new_value = (JSON_Value*)parser->malloc_func(sizeof(JSON_Value), parser->malloc_userdata);
    if (!new_value) {
        return NULL;
    }
    new_value->parent = NULL;
    new_value->type = JSONNull;
    return new_value;
}

JSON_Value * json_value_deep_copy(JSON_Parser const * parser, const JSON_Value *value) {
    size_t i = 0;
    JSON_Value *return_value = NULL, *temp_value_copy = NULL, *temp_value = NULL;
    const JSON_String *temp_string = NULL;
    const char *temp_key = NULL;
    char *temp_string_copy = NULL;
    JSON_Array *temp_array = NULL, *temp_array_copy = NULL;
    JSON_Object *temp_object = NULL, *temp_object_copy = NULL;
    JSON_Status res = JSONFailure;
    char *key_copy = NULL;

    switch (json_value_get_type(value)) {
        case JSONArray:
            temp_array = json_value_get_array(value);
            return_value = json_value_init_array(parser);
            if (return_value == NULL) {
                return NULL;
            }
            temp_array_copy = json_value_get_array(return_value);
            for (i = 0; i < json_array_get_count(temp_array); i++) {
                temp_value = json_array_get_value(temp_array, i);
                temp_value_copy = json_value_deep_copy(parser, temp_value);
                if (temp_value_copy == NULL) {
                    json_value_free(parser, return_value);
                    return NULL;
                }
                if (json_array_add(parser, temp_array_copy, temp_value_copy) != JSONSuccess) {
                    json_value_free(parser, return_value);
                    json_value_free(parser, temp_value_copy);
                    return NULL;
                }
            }
            return return_value;
        case JSONObject:
            temp_object = json_value_get_object(value);
            return_value = json_value_init_object(parser);
            if (!return_value) {
                return NULL;
            }
            temp_object_copy = json_value_get_object(return_value);
            for (i = 0; i < json_object_get_count(temp_object); i++) {
                temp_key = json_object_get_name(temp_object, i);
                temp_value = json_object_get_value(temp_object, temp_key);
                temp_value_copy = json_value_deep_copy(parser, temp_value);
                if (!temp_value_copy) {
                    json_value_free(parser, return_value);
                    return NULL;
                }
                key_copy = parson_strdup(parser, temp_key);
                if (!key_copy) {
                    json_value_free(parser, temp_value_copy);
                    json_value_free(parser, return_value);
                    return NULL;
                }
                res = json_object_add(parser, temp_object_copy, key_copy, temp_value_copy);
                if (res != JSONSuccess) {
                    parser->free_func(key_copy, parser->malloc_userdata);
                    json_value_free(parser, temp_value_copy);
                    json_value_free(parser, return_value);
                    return NULL;
                }
            }
            return return_value;
        case JSONBoolean:
            return json_value_init_boolean(parser, json_value_get_boolean(value));
        case JSONNumber:
            return json_value_init_number(parser, json_value_get_number(value));
        case JSONString:
            temp_string = json_value_get_string_desc(value);
            if (temp_string == NULL) {
                return NULL;
            }
            temp_string_copy = parson_strndup(parser, temp_string->chars, temp_string->length);
            if (temp_string_copy == NULL) {
                return NULL;
            }
            return_value = json_value_init_string_no_copy(parser, temp_string_copy, temp_string->length);
            if (return_value == NULL) {
                parser->free_func(temp_string_copy, parser->malloc_userdata);
            }
            return return_value;
        case JSONNull:
            return json_value_init_null(parser);
        case JSONError:
            return NULL;
        default:
            return NULL;
    }
}

size_t json_serialization_size(JSON_Parser const * parser, const JSON_Value *value) {
    char num_buf[PARSON_NUM_BUF_SIZE]; /* recursively allocating buffer on stack is a bad idea, so let's do it only once */
    int res = json_serialize_to_buffer_r(parser, value, NULL, 0, PARSON_FALSE, num_buf);
    return res < 0 ? 0 : (size_t)(res) + 1;
}

JSON_Status json_serialize_to_buffer(JSON_Parser const * parser, const JSON_Value *value, char *buf, size_t buf_size_in_bytes) {
    int written = -1;
    size_t needed_size_in_bytes = json_serialization_size(parser, value);
    if (needed_size_in_bytes == 0 || buf_size_in_bytes < needed_size_in_bytes) {
        return JSONFailure;
    }
    written = json_serialize_to_buffer_r(parser, value, buf, 0, PARSON_FALSE, NULL);
    if (written < 0) {
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_serialize_to_file(JSON_Parser const * parser, const JSON_Value *value, const char *filename) {
    JSON_Status return_code = JSONSuccess;
    FILE *fp = NULL;
    char *serialized_string = json_serialize_to_string(parser, value);
    if (serialized_string == NULL) {
        return JSONFailure;
    }
    fp = fopen(filename, "w");
    if (fp == NULL) {
        json_free_serialized_string(parser, serialized_string);
        return JSONFailure;
    }
    if (fputs(serialized_string, fp) == EOF) {
        return_code = JSONFailure;
    }
    if (fclose(fp) == EOF) {
        return_code = JSONFailure;
    }
    json_free_serialized_string(parser, serialized_string);
    return return_code;
}

char * json_serialize_to_string(JSON_Parser const * parser, const JSON_Value *value) {
    JSON_Status serialization_result = JSONFailure;
    size_t buf_size_bytes = json_serialization_size(parser, value);
    char *buf = NULL;
    if (buf_size_bytes == 0) {
        return NULL;
    }
    buf = (char*)parser->malloc_func(buf_size_bytes, parser->malloc_userdata);
    if (buf == NULL) {
        return NULL;
    }
    serialization_result = json_serialize_to_buffer(parser, value, buf, buf_size_bytes);
    if (serialization_result != JSONSuccess) {
        json_free_serialized_string(parser, buf);
        return NULL;
    }
    return buf;
}

size_t json_serialization_size_pretty(JSON_Parser const * parser, const JSON_Value *value) {
    char num_buf[PARSON_NUM_BUF_SIZE]; /* recursively allocating buffer on stack is a bad idea, so let's do it only once */
    int res = json_serialize_to_buffer_r(parser, value, NULL, 0, PARSON_TRUE, num_buf);
    return res < 0 ? 0 : (size_t)(res) + 1;
}

JSON_Status json_serialize_to_buffer_pretty(JSON_Parser const * parser, const JSON_Value *value, char *buf, size_t buf_size_in_bytes) {
    int written = -1;
    size_t needed_size_in_bytes = json_serialization_size_pretty(parser, value);
    if (needed_size_in_bytes == 0 || buf_size_in_bytes < needed_size_in_bytes) {
        return JSONFailure;
    }
    written = json_serialize_to_buffer_r(parser, value, buf, 0, PARSON_TRUE, NULL);
    if (written < 0) {
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_serialize_to_file_pretty(JSON_Parser const * parser, const JSON_Value *value, const char *filename) {
    JSON_Status return_code = JSONSuccess;
    FILE *fp = NULL;
    char *serialized_string = json_serialize_to_string_pretty(parser, value);
    if (serialized_string == NULL) {
        return JSONFailure;
    }
    fp = fopen(filename, "w");
    if (fp == NULL) {
        json_free_serialized_string(parser, serialized_string);
        return JSONFailure;
    }
    if (fputs(serialized_string, fp) == EOF) {
        return_code = JSONFailure;
    }
    if (fclose(fp) == EOF) {
        return_code = JSONFailure;
    }
    json_free_serialized_string(parser, serialized_string);
    return return_code;
}

char * json_serialize_to_string_pretty(JSON_Parser const * parser, const JSON_Value *value) {
    JSON_Status serialization_result = JSONFailure;
    size_t buf_size_bytes = json_serialization_size_pretty(parser, value);
    char *buf = NULL;
    if (buf_size_bytes == 0) {
        return NULL;
    }
    buf = (char*)parser->malloc_func(buf_size_bytes, parser->malloc_userdata);
    if (buf == NULL) {
        return NULL;
    }
    serialization_result = json_serialize_to_buffer_pretty(parser, value, buf, buf_size_bytes);
    if (serialization_result != JSONSuccess) {
        json_free_serialized_string(parser, buf);
        return NULL;
    }
    return buf;
}

void json_free_serialized_string(JSON_Parser const * parser, char *string) {
    parser->free_func(string, parser->malloc_userdata);
}

JSON_Status json_array_set_reserve(JSON_Parser const * parser, JSON_Array *array, size_t size) {
    return json_array_resize(parser, array, size);
}

JSON_Status json_array_remove(JSON_Parser const * parser, JSON_Array *array, size_t ix) {
    size_t to_move_bytes = 0;
    if (array == NULL || ix >= json_array_get_count(array)) {
        return JSONFailure;
    }
    json_value_free(parser, json_array_get_value(array, ix));
    to_move_bytes = (json_array_get_count(array) - 1 - ix) * sizeof(JSON_Value*);
    memmove(array->items + ix, array->items + ix + 1, to_move_bytes);
    array->count -= 1;
    return JSONSuccess;
}

JSON_Status json_array_replace_value(JSON_Parser const * parser, JSON_Array *array, size_t ix, JSON_Value *value) {
    if (array == NULL || value == NULL || value->parent != NULL || ix >= json_array_get_count(array)) {
        return JSONFailure;
    }
    json_value_free(parser, json_array_get_value(array, ix));
    value->parent = json_array_get_wrapping_value(array);
    array->items[ix] = value;
    return JSONSuccess;
}

JSON_Status json_array_replace_string(JSON_Parser const * parser, JSON_Array *array, size_t i, const char* string) {
    JSON_Value *value = json_value_init_string(parser, string);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_replace_value(parser, array, i, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_replace_string_with_len(JSON_Parser const * parser, JSON_Array *array, size_t i, const char *string, size_t len) {
    JSON_Value *value = json_value_init_string_with_len(parser, string, len);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_replace_value(parser, array, i, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_replace_number(JSON_Parser const * parser, JSON_Array *array, size_t i, double number) {
    JSON_Value *value = json_value_init_number(parser, number);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_replace_value(parser, array, i, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_replace_boolean(JSON_Parser const * parser, JSON_Array *array, size_t i, int boolean) {
    JSON_Value *value = json_value_init_boolean(parser, boolean);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_replace_value(parser, array, i, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_replace_null(JSON_Parser const * parser, JSON_Array *array, size_t i) {
    JSON_Value *value = json_value_init_null(parser);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_replace_value(parser, array, i, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_clear(JSON_Parser const * parser, JSON_Array *array) {
    size_t i = 0;
    if (array == NULL) {
        return JSONFailure;
    }
    for (i = 0; i < json_array_get_count(array); i++) {
        json_value_free(parser, json_array_get_value(array, i));
    }
    array->count = 0;
    return JSONSuccess;
}

JSON_Status json_array_append_value(JSON_Parser const * parser, JSON_Array *array, JSON_Value *value) {
    if (array == NULL || value == NULL || value->parent != NULL) {
        return JSONFailure;
    }
    return json_array_add(parser, array, value);
}

JSON_Status json_array_append_string(JSON_Parser const * parser, JSON_Array *array, const char *string) {
    JSON_Value *value = json_value_init_string(parser, string);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_append_value(parser, array, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_append_string_with_len(JSON_Parser const * parser, JSON_Array *array, const char *string, size_t len) {
    JSON_Value *value = json_value_init_string_with_len(parser, string, len);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_append_value(parser, array, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_append_number(JSON_Parser const * parser, JSON_Array *array, double number) {
    JSON_Value *value = json_value_init_number(parser, number);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_append_value(parser, array, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_append_boolean(JSON_Parser const * parser, JSON_Array *array, int boolean) {
    JSON_Value *value = json_value_init_boolean(parser, boolean);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_append_value(parser, array, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_array_append_null(JSON_Parser const * parser, JSON_Array *array) {
    JSON_Value *value = json_value_init_null(parser);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_array_append_value(parser, array, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_set_value(JSON_Parser const * parser, JSON_Object *object, const char *name, JSON_Value *value) {
    unsigned long hash = 0;
    parson_bool_t found = PARSON_FALSE;
    size_t cell_ix = 0;
    size_t item_ix = 0;
    JSON_Value *old_value = NULL;
    char *key_copy = NULL;

    if (!object || !name || !value || value->parent) {
        return JSONFailure;
    }
    hash = hash_string(name, strlen(name));
    found = PARSON_FALSE;
    cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
    if (found) {
        item_ix = object->cells[cell_ix];
        old_value = object->values[item_ix];
        json_value_free(parser, old_value);
        object->values[item_ix] = value;
        value->parent = json_object_get_wrapping_value(object);
        return JSONSuccess;
    }
    if (object->count >= object->item_capacity) {
        JSON_Status res = json_object_grow_and_rehash(parser, object);
        if (res != JSONSuccess) {
            return JSONFailure;
        }
        cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
    }
    key_copy = parson_strdup(parser, name);
    if (!key_copy) {
        return JSONFailure;
    }
    object->names[object->count] = key_copy;
    object->cells[cell_ix] = object->count;
    object->values[object->count] = value;
    object->cell_ixs[object->count] = cell_ix;
    object->hashes[object->count] = hash;
    object->count++;
    value->parent = json_object_get_wrapping_value(object);
    return JSONSuccess;
}

JSON_Status json_object_set_string(JSON_Parser const * parser, JSON_Object *object, const char *name, const char *string) {
    JSON_Value *value = json_value_init_string(parser, string);
    JSON_Status status = json_object_set_value(parser, object, name, value);
    if (status != JSONSuccess) {
        json_value_free(parser, value);
    }
    return status;
}

JSON_Status json_object_set_string_with_len(JSON_Parser const * parser, JSON_Object *object, const char *name, const char *string, size_t len) {
    JSON_Value *value = json_value_init_string_with_len(parser, string, len);
    JSON_Status status = json_object_set_value(parser, object, name, value);
    if (status != JSONSuccess) {
        json_value_free(parser, value);
    }
    return status;
}

JSON_Status json_object_set_number(JSON_Parser const * parser, JSON_Object *object, const char *name, double number) {
    JSON_Value *value = json_value_init_number(parser, number);
    JSON_Status status = json_object_set_value(parser, object, name, value);
    if (status != JSONSuccess) {
        json_value_free(parser, value);
    }
    return status;
}

JSON_Status json_object_set_boolean(JSON_Parser const * parser, JSON_Object *object, const char *name, int boolean) {
    JSON_Value *value = json_value_init_boolean(parser, boolean);
    JSON_Status status = json_object_set_value(parser, object, name, value);
    if (status != JSONSuccess) {
        json_value_free(parser, value);
    }
    return status;
}

JSON_Status json_object_set_null(JSON_Parser const * parser, JSON_Object *object, const char *name) {
    JSON_Value *value = json_value_init_null(parser);
    JSON_Status status = json_object_set_value(parser, object, name, value);
    if (status != JSONSuccess) {
        json_value_free(parser, value);
    }
    return status;
}

JSON_Status json_object_dotset_value(JSON_Parser const * parser, JSON_Object *object, const char *name, JSON_Value *value) {
    const char *dot_pos = NULL;
    JSON_Value *temp_value = NULL, *new_value = NULL;
    JSON_Object *temp_object = NULL, *new_object = NULL;
    JSON_Status status = JSONFailure;
    size_t name_len = 0;
    char *name_copy = NULL;
    
    if (object == NULL || name == NULL || value == NULL) {
        return JSONFailure;
    }
    dot_pos = strchr(name, '.');
    if (dot_pos == NULL) {
        return json_object_set_value(parser, object, name, value);
    }
    name_len = dot_pos - name;
    temp_value = json_object_getn_value(object, name, name_len);
    if (temp_value) {
        /* Don't overwrite existing non-object (unlike json_object_set_value, but it shouldn't be changed at this point) */
        if (json_value_get_type(temp_value) != JSONObject) {
            return JSONFailure;
        }
        temp_object = json_value_get_object(temp_value);
        return json_object_dotset_value(parser, temp_object, dot_pos + 1, value);
    }
    new_value = json_value_init_object(parser);
    if (new_value == NULL) {
        return JSONFailure;
    }
    new_object = json_value_get_object(new_value);
    status = json_object_dotset_value(parser, new_object, dot_pos + 1, value);
    if (status != JSONSuccess) {
        json_value_free(parser, new_value);
        return JSONFailure;
    }
    name_copy = parson_strndup(parser, name, name_len);
    if (!name_copy) {
        json_object_dotremove_internal(parser, new_object, dot_pos + 1, 0);
        json_value_free(parser, new_value);
        return JSONFailure;
    }
    status = json_object_add(parser, object, name_copy, new_value);
    if (status != JSONSuccess) {
        parser->free_func(name_copy, parser->malloc_userdata);
        json_object_dotremove_internal(parser, new_object, dot_pos + 1, 0);
        json_value_free(parser, new_value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_dotset_string(JSON_Parser const * parser, JSON_Object *object, const char *name, const char *string) {
    JSON_Value *value = json_value_init_string(parser, string);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_object_dotset_value(parser, object, name, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_dotset_string_with_len(JSON_Parser const * parser, JSON_Object *object, const char *name, const char *string, size_t len) {
    JSON_Value *value = json_value_init_string_with_len(parser, string, len);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_object_dotset_value(parser, object, name, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_dotset_number(JSON_Parser const * parser, JSON_Object *object, const char *name, double number) {
    JSON_Value *value = json_value_init_number(parser, number);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_object_dotset_value(parser, object, name, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_dotset_boolean(JSON_Parser const * parser, JSON_Object *object, const char *name, int boolean) {
    JSON_Value *value = json_value_init_boolean(parser, boolean);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_object_dotset_value(parser, object, name, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_dotset_null(JSON_Parser const * parser, JSON_Object *object, const char *name) {
    JSON_Value *value = json_value_init_null(parser);
    if (value == NULL) {
        return JSONFailure;
    }
    if (json_object_dotset_value(parser, object, name, value) != JSONSuccess) {
        json_value_free(parser, value);
        return JSONFailure;
    }
    return JSONSuccess;
}

JSON_Status json_object_remove(JSON_Parser const * parser, JSON_Object *object, const char *name) {
    return json_object_remove_internal(parser, object, name, PARSON_TRUE);
}

JSON_Status json_object_dotremove(JSON_Parser const * parser, JSON_Object *object, const char *name) {
    return json_object_dotremove_internal(parser, object, name, PARSON_TRUE);
}

JSON_Status json_object_clear(JSON_Parser const * parser, JSON_Object *object) {
    size_t i = 0;
    if (object == NULL) {
        return JSONFailure;
    }
    for (i = 0; i < json_object_get_count(object); i++) {
        parser->free_func(object->names[i], parser->malloc_userdata);
        object->names[i] = NULL;
        
        json_value_free(parser, object->values[i]);
        object->values[i] = NULL;
    }
    object->count = 0;
    for (i = 0; i < object->cell_capacity; i++) {
        object->cells[i] = OBJECT_INVALID_IX;
    }
    return JSONSuccess;
}

JSON_Status json_validate(const JSON_Value *schema, const JSON_Value *value) {
    JSON_Value *temp_schema_value = NULL, *temp_value = NULL;
    JSON_Array *schema_array = NULL, *value_array = NULL;
    JSON_Object *schema_object = NULL, *value_object = NULL;
    JSON_Value_Type schema_type = JSONError, value_type = JSONError;
    const char *key = NULL;
    size_t i = 0, count = 0;
    if (schema == NULL || value == NULL) {
        return JSONFailure;
    }
    schema_type = json_value_get_type(schema);
    value_type = json_value_get_type(value);
    if (schema_type != value_type && schema_type != JSONNull) { /* null represents all values */
        return JSONFailure;
    }
    switch (schema_type) {
        case JSONArray:
            schema_array = json_value_get_array(schema);
            value_array = json_value_get_array(value);
            count = json_array_get_count(schema_array);
            if (count == 0) {
                return JSONSuccess; /* Empty array allows all types */
            }
            /* Get first value from array, rest is ignored */
            temp_schema_value = json_array_get_value(schema_array, 0);
            for (i = 0; i < json_array_get_count(value_array); i++) {
                temp_value = json_array_get_value(value_array, i);
                if (json_validate(temp_schema_value, temp_value) != JSONSuccess) {
                    return JSONFailure;
                }
            }
            return JSONSuccess;
        case JSONObject:
            schema_object = json_value_get_object(schema);
            value_object = json_value_get_object(value);
            count = json_object_get_count(schema_object);
            if (count == 0) {
                return JSONSuccess; /* Empty object allows all objects */
            } else if (json_object_get_count(value_object) < count) {
                return JSONFailure; /* Tested object mustn't have less name-value pairs than schema */
            }
            for (i = 0; i < count; i++) {
                key = json_object_get_name(schema_object, i);
                temp_schema_value = json_object_get_value(schema_object, key);
                temp_value = json_object_get_value(value_object, key);
                if (temp_value == NULL) {
                    return JSONFailure;
                }
                if (json_validate(temp_schema_value, temp_value) != JSONSuccess) {
                    return JSONFailure;
                }
            }
            return JSONSuccess;
        case JSONString: case JSONNumber: case JSONBoolean: case JSONNull:
            return JSONSuccess; /* equality already tested before switch */
        case JSONError: default:
            return JSONFailure;
    }
}

int json_value_equals(const JSON_Value *a, const JSON_Value *b) {
    JSON_Object *a_object = NULL, *b_object = NULL;
    JSON_Array *a_array = NULL, *b_array = NULL;
    const JSON_String *a_string = NULL, *b_string = NULL;
    const char *key = NULL;
    size_t a_count = 0, b_count = 0, i = 0;
    JSON_Value_Type a_type, b_type;
    a_type = json_value_get_type(a);
    b_type = json_value_get_type(b);
    if (a_type != b_type) {
        return PARSON_FALSE;
    }
    switch (a_type) {
        case JSONArray:
            a_array = json_value_get_array(a);
            b_array = json_value_get_array(b);
            a_count = json_array_get_count(a_array);
            b_count = json_array_get_count(b_array);
            if (a_count != b_count) {
                return PARSON_FALSE;
            }
            for (i = 0; i < a_count; i++) {
                if (!json_value_equals(json_array_get_value(a_array, i),
                                       json_array_get_value(b_array, i))) {
                    return PARSON_FALSE;
                }
            }
            return PARSON_TRUE;
        case JSONObject:
            a_object = json_value_get_object(a);
            b_object = json_value_get_object(b);
            a_count = json_object_get_count(a_object);
            b_count = json_object_get_count(b_object);
            if (a_count != b_count) {
                return PARSON_FALSE;
            }
            for (i = 0; i < a_count; i++) {
                key = json_object_get_name(a_object, i);
                if (!json_value_equals(json_object_get_value(a_object, key),
                                       json_object_get_value(b_object, key))) {
                    return PARSON_FALSE;
                }
            }
            return PARSON_TRUE;
        case JSONString:
            a_string = json_value_get_string_desc(a);
            b_string = json_value_get_string_desc(b);
            if (a_string == NULL || b_string == NULL) {
                return PARSON_FALSE; /* shouldn't happen */
            }
            return a_string->length == b_string->length &&
                   memcmp(a_string->chars, b_string->chars, a_string->length) == 0;
        case JSONBoolean:
            return json_value_get_boolean(a) == json_value_get_boolean(b);
        case JSONNumber:
            return fabs(json_value_get_number(a) - json_value_get_number(b)) < 0.000001; /* EPSILON */
        case JSONError:
            return PARSON_TRUE;
        case JSONNull:
            return PARSON_TRUE;
        default:
            return PARSON_TRUE;
    }
}

JSON_Value_Type json_type(const JSON_Value *value) {
    return json_value_get_type(value);
}

JSON_Object * json_object (const JSON_Value *value) {
    return json_value_get_object(value);
}

JSON_Array * json_array(const JSON_Value *value) {
    return json_value_get_array(value);
}

const char * json_string(const JSON_Value *value) {
    return json_value_get_string(value);
}

size_t json_string_len(const JSON_Value *value) {
    return json_value_get_string_len(value);
}

double json_number(const JSON_Value *value) {
    return json_value_get_number(value);
}

int json_boolean(const JSON_Value *value) {
    return json_value_get_boolean(value);
}

JSON_Parser json_get_default_parser() {
    JSON_Parser parser;
    parser.malloc_func = default_malloc;
    parser.free_func = default_free;
    parser.malloc_userdata = NULL;
    parser.float_fmt_str = PARSON_DEFAULT_FLOAT_FORMAT;
    parser.number_serialization_func = NULL;
    parser.escape_slashes = 1;
    return parser;
}
