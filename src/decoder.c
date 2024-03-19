/*
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @ingroup nanocbor
 * @{
 * @file
 * @brief   Minimalistic CBOR decoder implementation
 *
 * @author  Koen Zandberg <koen@bergzand.net>
 * @author  Mikolai Gütschow <mikolai.guetschow@tu-dresden.de>
 * @}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nanocbor/config.h"
#include "nanocbor/nanocbor.h"

#include NANOCBOR_BYTEORDER_HEADER

void nanocbor_decoder_init(nanocbor_value_t *value, const uint8_t *buf,
                           size_t len)
{
    value->cur = buf;
    value->end = buf + len;
    value->flags = 0;
    /* no need to initialize value->remaining since this is not a container */
#if NANOCBOR_DECODE_PACKED_ENABLED
    memset(value->shared_item_tables, 0, sizeof(value->shared_item_tables));
#endif
}

#if NANOCBOR_DECODE_PACKED_ENABLED
void nanocbor_decoder_init_packed(nanocbor_value_t *value, const uint8_t *buf,
                           size_t len)
{
    nanocbor_decoder_init(value, buf, len);
    value->flags = NANOCBOR_DECODER_FLAG_PACKED_SUPPORT;
    value->num_active_tables = 0;
}

void nanocbor_decoder_init_packed_table(nanocbor_value_t *value, const uint8_t *buf,
                           size_t len, const uint8_t *table_buf, size_t table_len)
{
    nanocbor_decoder_init_packed(value, buf, len);
    if (table_buf != NULL && table_len > 0) {
        // todo: maybe validate content to be CBOR array -> would need return code
        value->shared_item_tables[0].start = table_buf;
        value->shared_item_tables[0].len = table_len;
    }
    value->num_active_tables = 1;
}
#endif


static void _advance(nanocbor_value_t *cvalue, unsigned int res)
{
    cvalue->cur += res;
    cvalue->remaining--;
}

static int _advance_if(nanocbor_value_t *cvalue, int res)
{
    if (res > 0) {
        _advance(cvalue, (unsigned int)res);
    }
    return res;
}

static inline bool _over_end(const nanocbor_value_t *it)
{
    return it->cur >= it->end;
}

static int _value_match_exact(nanocbor_value_t *cvalue, uint8_t val)
{
    int res = NANOCBOR_ERR_INVALID_TYPE;

    if (_over_end(cvalue)) {
        res = NANOCBOR_ERR_END;
    }
    else if (*cvalue->cur == val) {
        _advance(cvalue, 1U);
        res = NANOCBOR_OK;
    }
    return res;
}

bool nanocbor_at_end(const nanocbor_value_t *it)
{
    bool end = false;
    /* The container is at the end when */
    if (_over_end(it) || /* Number of items exhausted */
        /* Indefinite container and the current item is the end marker */
        ((nanocbor_container_indefinite(it)
          && *it->cur
              == (NANOCBOR_TYPE_FLOAT << NANOCBOR_TYPE_OFFSET
                  | NANOCBOR_VALUE_MASK)))
        ||
        /* Or the remaining number of items is zero */
        (!nanocbor_container_indefinite(it) && nanocbor_in_container(it)
         && it->remaining == 0)) {
        end = true;
    }
    return end;
}

static inline int __get_type(const nanocbor_value_t *value)
{
    if (nanocbor_at_end(value)) {
        return NANOCBOR_ERR_END;
    }
    return ((*value->cur & NANOCBOR_TYPE_MASK) >> NANOCBOR_TYPE_OFFSET);
}

static int _get_uint64(const nanocbor_value_t *cvalue, uint64_t *value,
                       uint8_t max, int type)
{
    int ctype = __get_type(cvalue);

    if (ctype < 0) {
        return ctype;
    }

    if (type != ctype) {
        return NANOCBOR_ERR_INVALID_TYPE;
    }
    unsigned bytelen = *cvalue->cur & NANOCBOR_VALUE_MASK;

    if (bytelen < NANOCBOR_SIZE_BYTE) {
        *value = bytelen;
        /* Ptr should advance 1 pos */
        return 1;
    }
    if (bytelen > max) {
        return NANOCBOR_ERR_OVERFLOW;
    }

    unsigned bytes = 1U << (bytelen - NANOCBOR_SIZE_BYTE);

    if ((cvalue->cur + bytes) >= cvalue->end) {
        return NANOCBOR_ERR_END;
    }
    uint64_t tmp = 0;
    /* Copy the value from cbor to the least significant bytes */
    memcpy(((uint8_t *)&tmp) + sizeof(uint64_t) - bytes, cvalue->cur + 1U,
           bytes);
    /* NOLINTNEXTLINE: user supplied function */
    tmp = NANOCBOR_BE64TOH_FUNC(tmp);
    *value = 0;
    memcpy(value, &tmp, bytes);

    return (int)(1 + bytes);
}

#if NANOCBOR_DECODE_PACKED_ENABLED
// todo: backup and restore currently unused
static inline void _packed_backup(const nanocbor_value_t *value, const uint8_t **cur, uint64_t *remaining, uint8_t *num_active_tables)
{
    *cur = value->cur;
    *remaining = value->remaining;
    *num_active_tables = value->num_active_tables;
}
static inline void _packed_restore(nanocbor_value_t *value, const uint8_t **cur, const uint64_t *remaining, const uint8_t *num_active_tables)
{
    value->cur = *cur;
    value->remaining = *remaining;
    value->num_active_tables = *num_active_tables;
}

/* forward declarations of functions used by packed CBOR handling */
static int _get_type(nanocbor_value_t *cvalue, uint8_t limit);
static int _enter_container(nanocbor_value_t *it,
                            nanocbor_value_t *container, uint8_t type, uint8_t limit);
static int _leave_container(nanocbor_value_t *it, nanocbor_value_t *container, uint8_t limit);
static int _skip_limited(nanocbor_value_t *it, uint8_t limit);
static int _packed_handle(nanocbor_value_t *cvalue, nanocbor_value_t *target, uint8_t limit);

/**
 * Check whether packed CBOR decoding is enabled for the given @p value.
 *
 * @param       value   decoder context
 *
 * @return      `true` if packed CBOR decoding is enabled, otherwise `false`
 */
static inline bool _packed_enabled(const nanocbor_value_t *value)
{
    return (value->flags & NANOCBOR_DECODER_FLAG_PACKED_SUPPORT) != 0;
}

static inline void _packed_set_enabled(nanocbor_value_t *value, bool enabled)
{
    if (enabled) {
        value->flags |= NANOCBOR_DECODER_FLAG_PACKED_SUPPORT;
    }
    else {
        value->flags &= ~NANOCBOR_DECODER_FLAG_PACKED_SUPPORT;
    }
}

/**
 * Copy active set of packing tables from @p src to @p dest.
 *
 * @param       dest   decoder context whose active set of packing tables will be overwritten
 * @param       dest   decoder context with the active set of packing tables to be copied
 */
static inline void _packed_copy_tables(nanocbor_value_t *dest, const nanocbor_value_t *src)
{
    memcpy(dest->shared_item_tables, src->shared_item_tables, sizeof(dest->shared_item_tables));
    dest->num_active_tables = src->num_active_tables;
}

// todo: consider rewind on error (backup curr, remaining, packed tables (max index stored in nanocbor_value_t?), restore on error)
// todo: maybe rename parameters on _packed_* to outer and inner, which are at the beginning just copies of cvalue & for recursive calls one of them could be NULL to make that fact explicit, on successful _packed_handle cvalue could be updated to outer 
/**
 * Macro to avoid code duplication in decoder function implementations.
 * This is supposed to be used as a first line in all the functions
 * that should transparently handle packed CBOR data items.
 *
 * It works as follows:
 * 1. if the recursion limit is reached, @ref NANOCBOR_ERR_RECURSION is returned
 * 2. if @p cvalue points to a supported packed CBOR data item, it is handled
 *   a. on success, the recursive function call @p func is executed
 *   b. on failure during packed CBOR handling, the failure code is returned
 * 3. if no supported packed CBOR data item was found, execution continues within the enclosing function
 *
 * Recursion is bounded as long as @p limit is decreased by one in the recursive function call.
 *
 * If the packed CBOR handling is aborted due to an error, the state of @p cvalue is undefined.
 *
 * @param       cvalue  decoder context
 * @param       limit   recursion limit
 * @param       func    recursive function call where @p cvalue should be replaced by `&followed` and @p limit decreased by one
 */
// todo: from `cvalue = &followed` on, cvalue actually does not refer to outer cvalue, but it also shouldn't need to
// todo: update / adapt docs
#define _PACKED_HANDLE(cvalue, followed, limit)                 \
    do {                                                        \
        if (limit == 0) {                                       \
            return NANOCBOR_ERR_RECURSION;                      \
        }                                                       \
        int res = _packed_handle(cvalue, followed, limit-1);    \
        if (res == NANOCBOR_OK) {                               \
            /* packed CBOR found and handled */                 \
            /* update cvalue for remaining function call */     \
            cvalue = followed;                                  \
        }                                                       \
        else if (res != NANOCBOR_NOT_FOUND) {                   \
            /* error during packed CBOR handling */             \
            return res;                                         \
        }                                                       \
        /* else: no packed CBOR found, simply continue */       \
    } while (0)

#define _PACKED_HANDLE_OUTER(cvalue, followed, limit) nanocbor_value_t followed; _PACKED_HANDLE(cvalue, &followed, limit)

#define _PACKED_HANDLE_LIMITED(cvalue) uint8_t limit = NANOCBOR_RECURSION_MAX; _PACKED_HANDLE_OUTER(cvalue, followed, limit)

/**
 * @brief Consume the content of a tag 113 packing table definition.
 *
 * This function updates @p target to point to the rump of the table definition
 * and adds the table definition to its active set of packing tables.
 * It also updates @p cvalue to point to the (potential) data item after tag 113.
 *
 * @note this is supposed to be called only from within @ref _packed_handle
 *
 * @param       cvalue  decoder context at the start of the content of tag 113, i.e., a two-element array
 * @param       target  decoder context that will be updated to point to the rump of the function table definition
 * @param       limit   recursion limit
 *
 * @return      NANOCBOR_OK on success
 * @return      NANOCBOR_ERR_PACKED_FORMAT if the tag content is not well-formed
 * @return      NANOCBOR_ERR_PACKED_MEMORY if the maximum number of active packing tables is exhausted
 *              (cf. @ref NANOCBOR_NANOCBOR_DECODE_PACKED_NESTED_TABLES_MAX)
 * @return      negative on error
 */
static int _packed_consume_table(nanocbor_value_t *cvalue, nanocbor_value_t *target, uint8_t limit)
{
    nanocbor_value_t arr;
    // todo: could we instead call _packed_handle here proactively and then use low-level container / somehow get rid of extra arr value
    /* same as nanocbor_enter_array(cvalue, &arr), but passing down limit */
    int ret = _enter_container(cvalue, &arr, NANOCBOR_TYPE_ARR, limit-1);
    if (ret < 0) {
        return ret == NANOCBOR_ERR_RECURSION ? ret : NANOCBOR_ERR_PACKED_FORMAT;
    }
    nanocbor_decoder_init_packed(target, arr.cur, arr.end - arr.cur);
    _packed_copy_tables(target, &arr);
    if (target->num_active_tables >= NANOCBOR_DECODE_PACKED_NESTED_TABLES_MAX) {
        return NANOCBOR_ERR_PACKED_MEMORY;
    }

    struct nanocbor_packed_table *t = &target->shared_item_tables[target->num_active_tables];
    // todo: double-check if nested packing handling _get_type really needed -> not for current set of tests at least
    // needed for inner, itself packed table definition as in 113([[[undefined]], 113([simple(0), simple(0)])])
    // but in that case, following the reference directly, aka storing the actual table instead of the reference to the table would be better anyhow
    // could we just use _packed_handle from here recursively?
    // todo: continue here - add test that should fail as is and not if changed back, then try with _packed_handle
    ret = __get_type(&arr);
    // ret = _get_type(&arr, limit-1);
    if (ret != NANOCBOR_TYPE_ARR) {
        return ret == NANOCBOR_ERR_RECURSION ? ret : NANOCBOR_ERR_PACKED_FORMAT;
    }
    t->start = arr.cur;
    ret = _skip_limited(&arr, limit-1);
    if (ret != NANOCBOR_OK) {
        return ret;
    }
    t->len = arr.cur - t->start;
    /* set target to rump */
    target->cur = arr.cur;
    ret = _skip_limited(&arr, limit-1);
    if (ret != NANOCBOR_OK) {
        return ret;
    }
    target->end = arr.cur;
    // todo: very ugly hack, instead somehow explicitely tell function that it should only work on target and not update cvalue
    // but in some circumstances, this is actually needed to advance cvalue properly...
    if (cvalue != target) {
        ret = _leave_container(cvalue, &arr, limit-1);
        if (ret != NANOCBOR_OK) {
            return ret == NANOCBOR_ERR_RECURSION ? ret : NANOCBOR_ERR_PACKED_FORMAT;
        }
    }
    target->num_active_tables++;
    return NANOCBOR_OK;
}

/**
 * @brief Follow a packed CBOR shared item reference.
 *
 * This function updates @p target to point to the start of the referenced data item,
 * which resides in one of the active packing tables.
 *
 * @note this is supposed to be called only from within @ref _packed_handle
 *
 * @param       cvalue  decoder context containing the active set of packing tables
 * @param       target  decoder context that will be updated to point to the start of the referenced data item
 * @param       idx     index of the requested reference
 * @param       limit   recursion limit
 *
 * @return      NANOCBOR_OK on success
 * @return      NANOCBOR_ERR_PACKED_FORMAT if one of the packing table definitions is not well-formed
 * @return      NANOCBOR_ERR_PACKED_UNDEFINED_REFERENCE if the reference is undefined,
 *              i.e., the index larger than the sum of the sizes of all active packing tables
 * @return      negative on error
 */
static int _packed_follow_reference(const nanocbor_value_t *cvalue, nanocbor_value_t *target, uint64_t idx, uint8_t limit)
{
    const size_t num = cvalue->num_active_tables;
    for (size_t i=0; i<num; i++) {
        const struct nanocbor_packed_table *t = &cvalue->shared_item_tables[num-1-i];
        if (t->start == NULL) {
            return NANOCBOR_ERR_PACKED_FORMAT; // todo: actually internal error
        }
        nanocbor_value_t table;
        nanocbor_decoder_init_packed(&table, t->start, t->len);
        _packed_copy_tables(&table, cvalue);

        int res;
        // todo: could we instead call _packed_handle here proactively and then use low-level container access / somehow get rid of extra table value
        /* same as nanocbor_enter_array(&table, target), but passing down limit */
        res = _enter_container(&table, target, NANOCBOR_TYPE_ARR, limit-1);
        if (res < 0) {
            return res == NANOCBOR_ERR_RECURSION ? res : NANOCBOR_ERR_PACKED_FORMAT;
        }

        uint32_t table_size = nanocbor_container_indefinite(target) ? UINT32_MAX : nanocbor_array_items_remaining(target);
        if (idx < table_size) {
            size_t j=0;
            while (j<idx && !nanocbor_at_end(target)) {
                res = _skip_limited(target, limit);
                if (res < 0) return res;
                j++;
            }
            if (nanocbor_at_end(target)) {
                /* for indefinite length tables, this means that j now contains the actual table size */
                table_size = j;
                goto next_table;
            }
            /* copy all common tables, i.e., ones that were defined up to and including i */
            _packed_copy_tables(target, cvalue);
            target->num_active_tables = num-i;
            return NANOCBOR_OK;
        } else {
next_table:
            idx -= table_size;
        }
    }
    /* idx not within current tables */
    return NANOCBOR_ERR_PACKED_UNDEFINED_REFERENCE;
}

/**
 * @brief Check for and handle a supported packed CBOR data item, if present at the current decoder position.
 *
 * If @p cvalue has support for packed CBOR handling enabled and
 * points to a supported packed CBOR data item,
 * @p target is updated to point to the start of the reconstructed data item
 * or the rump of a table definition, and @ref NANOCBOR_OK is returned.
 * Otherwise, @ref NANOCBOR_NOT_FOUND is returned.
 *
 * @param       cvalue  decoder context containing the current active set of packing tables
 * @param       target  decoder context that will point to
 * @param       limit   recursion limit
 *
 * @return      NANOCBOR_OK if a supported packed CBOR data item was found and handled
 * @return      NANOCBOR_NOT_FOUND if no supported packed CBOR data item was found
 * @return      NANOCBOR_ERR_PACKED_FORMAT if one of the packed CBOR data items is not well-formed
 * @return      NANOCBOR_ERR_PACKED_MEMORY if the maximum number of active packing tables is exhausted
 * @return      NANOCBOR_ERR_PACKED_UNDEFINED_REFERENCE if an undefined reference is encountered
 * @return      negative on error
 */
static int _packed_handle(nanocbor_value_t *cvalue, nanocbor_value_t *target, uint8_t limit)
{
    if (!_packed_enabled(cvalue)) {
        return NANOCBOR_NOT_FOUND;
    }
    if (limit == 0) {
        return NANOCBOR_ERR_RECURSION;
    }

    int ret = NANOCBOR_NOT_FOUND;
    /* cannot use nanocbor_get_tag / nanocbor_get_simple instead to avoid infinite recursion */
    int ctype = __get_type(cvalue);
    if (ctype == NANOCBOR_TYPE_TAG) {
        uint64_t tag = 0;
        ret = _get_uint64(cvalue, &tag, NANOCBOR_SIZE_WORD, NANOCBOR_TYPE_TAG);
        if (ret < 0) {
            return NANOCBOR_NOT_FOUND;
        }
        if (tag == NANOCBOR_TAG_PACKED_TABLE) {
            cvalue->cur += ret;
            ret = _packed_consume_table(cvalue, target, limit);
        }
        else if (tag == NANOCBOR_TAG_PACKED_REF_SHARED) {
            uint64_t n;
            cvalue->cur += ret;

            // todo: would it be possible to explicitely call _packed_handle from here recursively instead of passing down limit like this? > probably easier here than for _enter_container and friends
            // ret = _packed_handle(cvalue, target, limit-1);
            // if (ret == NANOCBOR_OK) {
            //     cvalue = target;
            // }
            // else if (ret != NANOCBOR_NOT_FOUND && ret < 0) {
            //     return ret == NANOCBOR_ERR_RECURSION ? ret : NANOCBOR_ERR_PACKED_FORMAT;
            // }
            // todo: maybe make use of _PACKED_HANDLE instead -> split into one macro that defines followed and one that does not??
            // todo: this hides recursive call -> instead have Macro that only handles return value?
            _PACKED_HANDLE(cvalue, target, limit);

            int ctype = __get_type(cvalue);
            if (ctype != NANOCBOR_TYPE_UINT && ctype != NANOCBOR_TYPE_NINT) {
                // todo: needs to be changed to allow for argument referencing
                return NANOCBOR_ERR_PACKED_FORMAT;
            }
            ret = _get_uint64(cvalue, &n, NANOCBOR_SIZE_LONG, ctype);
            if (ret < 0) return NANOCBOR_ERR_PACKED_FORMAT; // todo: or pass on ret?
            _advance(cvalue, ret);
            // todo: check for overflow
            uint64_t idx = 16 + (ctype == NANOCBOR_TYPE_UINT ? 2*n : 2*n+1); // n' = -n-1 -> -2*n'-1 = -2*(-n-1)-1 = 2n+2-1 = 2n+1

            /* same as nanocbor_get_int64(cvalue, &n), but passing down limit */
            // ret = _get_and_advance_int64(cvalue, &n, NANOCBOR_SIZE_LONG, INT64_MAX, limit);
            // if (ret < 0)
            //     return ret == NANOCBOR_ERR_RECURSION ? ret : NANOCBOR_ERR_PACKED_FORMAT;
            // uint64_t idx = 16 + (n >= 0 ? 2*n : -2*n-1);
            ret = _packed_follow_reference(cvalue, target, idx, limit);
        }
        else {
            return NANOCBOR_NOT_FOUND;
        }
    }
    else if (ctype == NANOCBOR_TYPE_FLOAT) {
        uint8_t simple = *cvalue->cur & NANOCBOR_VALUE_MASK;
        if (simple < 16) {
            _advance(cvalue, 1);
            ret = _packed_follow_reference(cvalue, target, simple, limit);
        }
        else {
            return NANOCBOR_NOT_FOUND;
        }
    }
    
    if (ret == NANOCBOR_OK) {
        // todo: how to change logic to make clear cvalue is actually not needed to and also _should not_ be updated further?!
        ret = _packed_handle(target, target, limit-1);
        return (ret < 0 && ret != NANOCBOR_NOT_FOUND) ? ret : NANOCBOR_OK;
    }
    return ret;
}

#else /* !NANOCBOR_DECODE_PACKED_ENABLED */
#define _packed_enabled(v) (false)
#define _packed_copy_tables(d, s)
#define _PACKED_HANDLE(cvalue, limit, func)
// todo: needs to be adapted to contain all macros that are used later
#endif

#if NANOCBOR_DECODE_PACKED_ENABLED
static int _get_type(nanocbor_value_t *cvalue, uint8_t limit)
{
    _PACKED_HANDLE_OUTER(cvalue, followed, limit);

    return __get_type(cvalue);
}
#endif

int nanocbor_get_type(const nanocbor_value_t *value)
{
#if NANOCBOR_DECODE_PACKED_ENABLED
    /* cpy needed to keep *value const */
    nanocbor_value_t cpy = *value;
    return _get_type(&cpy, NANOCBOR_RECURSION_MAX-1);
#else
    return __get_type(value);
#endif
}

static int _get_and_advance_uint8(nanocbor_value_t *cvalue, uint8_t *value,
                                  int type)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    uint64_t tmp = 0;
    int res = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_BYTE, type);
    *value = (uint8_t)tmp;

    return _advance_if(cvalue, res);
}

int nanocbor_get_uint8(nanocbor_value_t *cvalue, uint8_t *value)
{
    return _get_and_advance_uint8(cvalue, value, NANOCBOR_TYPE_UINT);
}

static int _get_and_advance_uint64(nanocbor_value_t *cvalue, uint64_t *value,
                                  uint8_t max)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    int res = _get_uint64(cvalue, value, max, NANOCBOR_TYPE_UINT);
    return _advance_if(cvalue, res);
}

int nanocbor_get_uint16(nanocbor_value_t *cvalue, uint16_t *value)
{
    uint64_t tmp = 0;
    int res
        = _get_and_advance_uint64(cvalue, &tmp, NANOCBOR_SIZE_SHORT);

    *value = (uint8_t)tmp;

    return res;
}

int nanocbor_get_uint32(nanocbor_value_t *cvalue, uint32_t *value)
{
    uint64_t tmp = 0;
    int res
        = _get_and_advance_uint64(cvalue, &tmp, NANOCBOR_SIZE_WORD);

    *value = (uint8_t)tmp;

    return res;
}

int nanocbor_get_uint64(nanocbor_value_t *cvalue, uint64_t *value)
{
    uint64_t tmp = 0;
    int res
        = _get_and_advance_uint64(cvalue, &tmp, NANOCBOR_SIZE_LONG);

    *value = (uint8_t)tmp;

    return res;
}

static int _get_and_advance_int64(nanocbor_value_t *cvalue, int64_t *value,
                                  uint8_t max, uint64_t bound)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    int type = __get_type(cvalue);
    if (type < 0) {
        return type;
    }
    int res = NANOCBOR_ERR_INVALID_TYPE;
    if (type == NANOCBOR_TYPE_NINT || type == NANOCBOR_TYPE_UINT) {
        uint64_t intermediate = 0;
        res = _get_uint64(cvalue, &intermediate, max, type);
        if (intermediate > bound) {
            res = NANOCBOR_ERR_OVERFLOW;
        }
        if (type == NANOCBOR_TYPE_NINT) {
            *value = (-(int64_t)intermediate) - 1;
        }
        else {
            *value = (int64_t)intermediate;
        }
    }
    return _advance_if(cvalue, res);
}

int nanocbor_get_int8(nanocbor_value_t *cvalue, int8_t *value)
{
    int64_t tmp = 0;
    int res
        = _get_and_advance_int64(cvalue, &tmp, NANOCBOR_SIZE_BYTE, INT8_MAX);

    *value = (int8_t)tmp;

    return res;
}

int nanocbor_get_int16(nanocbor_value_t *cvalue, int16_t *value)
{
    int64_t tmp = 0;
    int res
        = _get_and_advance_int64(cvalue, &tmp, NANOCBOR_SIZE_SHORT, INT16_MAX);

    *value = (int16_t)tmp;

    return res;
}

int nanocbor_get_int32(nanocbor_value_t *cvalue, int32_t *value)
{
    int64_t tmp = 0;
    int res
        = _get_and_advance_int64(cvalue, &tmp, NANOCBOR_SIZE_WORD, INT32_MAX);

    *value = (int32_t)tmp;

    return res;
}

int nanocbor_get_int64(nanocbor_value_t *cvalue, int64_t *value)
{
    return _get_and_advance_int64(cvalue, value, NANOCBOR_SIZE_LONG, INT64_MAX);
}

int nanocbor_get_tag(nanocbor_value_t *cvalue, uint32_t *tag)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    uint64_t tmp = 0;
    int res = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_WORD, NANOCBOR_TYPE_TAG);

    if (res >= 0) {
        cvalue->cur += res;
        res = NANOCBOR_OK;
    }
    *tag = (uint32_t)tmp;

    return res;
}

int nanocbor_get_decimal_frac(nanocbor_value_t *cvalue, int32_t *e, int32_t *m)
{
    int res = NANOCBOR_NOT_FOUND;
    uint32_t tag = UINT32_MAX;
    if (nanocbor_get_tag(cvalue, &tag) == NANOCBOR_OK) {
        if (tag == NANOCBOR_TAG_DEC_FRAC) {
            nanocbor_value_t arr;
            if (nanocbor_enter_array(cvalue, &arr) == NANOCBOR_OK) {
                res = nanocbor_get_int32(&arr, e);
                if (res >= 0) {
                    res = nanocbor_get_int32(&arr, m);
                    if (res >= 0) {
                        res = NANOCBOR_OK;
                    }
                }
                nanocbor_leave_container(cvalue, &arr);
            }
        }
    }

    return res;
}

// todo: get rid of limit argument for all functions that are actually not called recursively! 
static int _get_str(nanocbor_value_t *cvalue, const uint8_t **buf, size_t *len,
                    uint8_t type)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    uint64_t tmp = 0;
    int res = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_SIZET, type);
    *len = tmp;

    if (cvalue->end - cvalue->cur < 0
        || (size_t)(cvalue->end - cvalue->cur) < *len) {
        return NANOCBOR_ERR_END;
    }
    if (res >= 0) {
        *buf = (cvalue->cur) + res;
        _advance(cvalue, (unsigned int)((size_t)res + *len));
        res = NANOCBOR_OK;
    }
    return res;
}

int nanocbor_get_bstr(nanocbor_value_t *cvalue, const uint8_t **buf,
                      size_t *len)
{
    return _get_str(cvalue, buf, len, NANOCBOR_TYPE_BSTR);
}

int nanocbor_get_tstr(nanocbor_value_t *cvalue, const uint8_t **buf,
                      size_t *len)
{
    return _get_str(cvalue, buf, len, NANOCBOR_TYPE_TSTR);
}

static int _get_simple_exact(nanocbor_value_t *cvalue, uint8_t val)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    return _value_match_exact(cvalue, val);
}

int nanocbor_get_null(nanocbor_value_t *cvalue)
{
    return _get_simple_exact(cvalue,
                            NANOCBOR_MASK_FLOAT | NANOCBOR_SIMPLE_NULL);
}

int nanocbor_get_bool(nanocbor_value_t *cvalue, bool *value)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    int res = _value_match_exact(cvalue,
                                 NANOCBOR_MASK_FLOAT | NANOCBOR_SIMPLE_FALSE);
    if (res >= NANOCBOR_OK) {
        *value = false;
    } else {
        res = _value_match_exact(cvalue,
                                 NANOCBOR_MASK_FLOAT | NANOCBOR_SIMPLE_TRUE);
        if (res >= NANOCBOR_OK) {
            *value = true;
        }
    }

    return res;
}

int nanocbor_get_undefined(nanocbor_value_t *cvalue)
{
    return _get_simple_exact(cvalue,
                            NANOCBOR_MASK_FLOAT | NANOCBOR_SIMPLE_UNDEF);
}

int nanocbor_get_simple(nanocbor_value_t *cvalue, uint8_t *value)
{
    int res = _get_and_advance_uint8(cvalue, value, NANOCBOR_TYPE_FLOAT);
    if (res == NANOCBOR_ERR_OVERFLOW) {
        res = NANOCBOR_ERR_INVALID_TYPE;
    }
    return res > 0 ? NANOCBOR_OK : res;
}

/* float bit mask related defines */
#define FLOAT_EXP_OFFSET (127U)
#define FLOAT_SIZE (32U)
#define FLOAT_EXP_POS (23U)
#define FLOAT_EXP_MASK ((uint32_t)0xFFU)
#define FLOAT_SIGN_POS (31U)
#define FLOAT_FRAC_MASK (0x7FFFFFU)
#define FLOAT_SIGN_MASK ((uint32_t)1U << FLOAT_SIGN_POS)
#define FLOAT_EXP_IS_NAN (0xFFU)
#define FLOAT_IS_ZERO (~(FLOAT_SIGN_MASK))
/* Part where a float to halffloat leads to precision loss */
#define FLOAT_HALF_LOSS (0x1FFFU)

/* halffloat bit mask related defines */
#define HALF_EXP_OFFSET (15U)
#define HALF_SIZE (16U)
#define HALF_EXP_POS (10U)
#define HALF_EXP_MASK (0x1FU)
#define HALF_SIGN_POS (15U)
#define HALF_FRAC_MASK (0x3FFU)
#define HALF_SIGN_MASK ((uint16_t)(1U << HALF_SIGN_POS))
#define HALF_MASK_HALF (0xFFU)

#define HALF_FLOAT_EXP_DIFF ((uint16_t)(FLOAT_EXP_OFFSET - HALF_EXP_OFFSET))
#define HALF_FLOAT_EXP_POS_DIFF ((uint16_t)(FLOAT_EXP_POS - HALF_EXP_POS))
#define HALF_EXP_TO_FLOAT (HALF_FLOAT_EXP_DIFF << HALF_EXP_POS)

static int _decode_half_float(nanocbor_value_t *cvalue, float *value)
{
    uint64_t tmp = 0;
    int res
        = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_SHORT, NANOCBOR_TYPE_FLOAT);
    if (res == 1 + sizeof(uint16_t)) {
        uint32_t *ifloat = (uint32_t *)value;
        *ifloat = (tmp & HALF_SIGN_MASK) << (FLOAT_SIGN_POS - HALF_SIGN_POS);

        uint32_t significant = tmp & HALF_FRAC_MASK;
        uint32_t exponent = tmp & (HALF_EXP_MASK << HALF_EXP_POS);

        static const uint32_t magic = ((uint32_t)FLOAT_EXP_OFFSET - 1)
            << FLOAT_EXP_POS;
        static const float *fmagic = (float *)&magic;

        if (exponent == 0) {
            *ifloat = magic + significant;
            *value -= *fmagic;
        }
        else {
            if (exponent == (HALF_EXP_MASK << HALF_EXP_POS)) {
                /* Set exponent to max value */
                exponent
                    = ((FLOAT_EXP_MASK - HALF_FLOAT_EXP_DIFF) << HALF_EXP_POS);
            }
            *ifloat
                |= ((exponent + HALF_EXP_TO_FLOAT) << HALF_FLOAT_EXP_POS_DIFF)
                | (significant << HALF_FLOAT_EXP_POS_DIFF);
        }
        return _advance_if(cvalue, res);
    }
    return res > 0 ? NANOCBOR_ERR_INVALID_TYPE : res;
}

static int _decode_float(nanocbor_value_t *cvalue, float *value)
{
    uint64_t tmp = 0;
    int res
        = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_WORD, NANOCBOR_TYPE_FLOAT);
    if (res == 1 + sizeof(uint32_t)) {
        uint32_t ifloat = tmp;
        memcpy(value, &ifloat, sizeof(uint32_t));
        return _advance_if(cvalue, res);
    }
    return res > 0 ? NANOCBOR_ERR_INVALID_TYPE : res;
}

static int _decode_double(nanocbor_value_t *cvalue, double *value)
{
    uint64_t tmp = 0;
    int res
        = _get_uint64(cvalue, &tmp, NANOCBOR_SIZE_LONG, NANOCBOR_TYPE_FLOAT);
    if (res == 1 + sizeof(uint64_t)) {
        uint64_t ifloat = tmp;
        memcpy(value, &ifloat, sizeof(uint64_t));
        return _advance_if(cvalue, res);
    }
    return res > 0 ? NANOCBOR_ERR_INVALID_TYPE : res;
}

int nanocbor_get_float(nanocbor_value_t *cvalue, float *value)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    int res = _decode_half_float(cvalue, value);
    if (res < 0) {
        res = _decode_float(cvalue, value);
    }
    return res;
}

int nanocbor_get_double(nanocbor_value_t *cvalue, double *value)
{
    _PACKED_HANDLE_LIMITED(cvalue);

    float tmp = 0;
    int res = nanocbor_get_float(cvalue, &tmp);
    if (res >= NANOCBOR_OK) {
        *value = tmp;
        return res;
    }
    return _decode_double(cvalue, value);
}

static int _enter_container(nanocbor_value_t *it,
                            nanocbor_value_t *container, uint8_t type, uint8_t limit)
{
    if (_packed_enabled(it)) {
        _PACKED_HANDLE_OUTER(it, followed, limit);
        _packed_copy_tables(container, it);
        /* mark container as being top-level shared item if _enter_container has been called recursively */
        container->flags = NANOCBOR_DECODER_FLAG_PACKED_SUPPORT | (it == &followed ? NANOCBOR_DECODER_FLAG_SHARED : 0);
    }
    else {
        container->flags = 0;
    }
    container->end = it->end;
    container->remaining = 0;

    uint8_t value_match = (uint8_t)(((unsigned)type << NANOCBOR_TYPE_OFFSET)
                                    | NANOCBOR_SIZE_INDEFINITE);

    /* Not using _value_match_exact here to keep *it const */
    if (!_over_end(it) && *it->cur == value_match) {
        container->flags |= NANOCBOR_DECODER_FLAG_INDEFINITE
            | NANOCBOR_DECODER_FLAG_CONTAINER;
        container->cur = it->cur + 1;
        return NANOCBOR_OK;
    }

    int res = _get_uint64(it, &container->remaining, NANOCBOR_SIZE_LONG, type);
    if (res < 0) {
        return res;
    }
    container->flags |= NANOCBOR_DECODER_FLAG_CONTAINER;
    container->cur = it->cur + res;
    return NANOCBOR_OK;
}

int nanocbor_enter_array(const nanocbor_value_t *it, nanocbor_value_t *array)
{
#if NANOCBOR_DECODE_PACKED_ENABLED
    /* cpy needed to keep *it const */
    nanocbor_value_t cpy = *it;
    return _enter_container(&cpy, array, NANOCBOR_TYPE_ARR, NANOCBOR_RECURSION_MAX-1);
#else
    /* if packed CBOR is disabled, *it will stay const */
    return _enter_container((nanocbor_value_t *)it, array, NANOCBOR_TYPE_ARR, NANOCBOR_RECURSION_MAX);
#endif
}

int nanocbor_enter_map(const nanocbor_value_t *it, nanocbor_value_t *map)
{
    int res;
#if NANOCBOR_DECODE_PACKED_ENABLED
    /* cpy needed to keep *it const */
    nanocbor_value_t cpy = *it;
    res = _enter_container(&cpy, map, NANOCBOR_TYPE_MAP, NANOCBOR_RECURSION_MAX-1);
#else
    /* if packed CBOR is disabled, *it will stay const */
    res = _enter_container((nanocbor_value_t *)it, map, NANOCBOR_TYPE_MAP, NANOCBOR_RECURSION_MAX);
#endif

    if (map->remaining > UINT64_MAX / 2) {
        return NANOCBOR_ERR_OVERFLOW;
    }
    map->remaining = map->remaining * 2;
    return res;
}

static inline int _leave_container(nanocbor_value_t *it, nanocbor_value_t *container, uint8_t limit)
{
    /* check `container` to be a valid, fully consumed container that is plausible to have been entered from `it` */
    if (!nanocbor_in_container(container) || !nanocbor_at_end(container)) {
        return NANOCBOR_ERR_INVALID_TYPE;
    }
#if NANOCBOR_DECODE_PACKED_ENABLED
    if (container->flags & NANOCBOR_DECODER_FLAG_SHARED) {
        return _skip_limited(it, limit);
    }
#endif
    if (container->cur <= it->cur || container->cur > it->end) {
        return NANOCBOR_ERR_INVALID_TYPE;
    }
    if (it->remaining) {
        it->remaining--;
    }
    if (nanocbor_container_indefinite(container)) {
        it->cur = container->cur + 1;
    }
    else {
        it->cur = container->cur;
    }
    return NANOCBOR_OK;
}

int nanocbor_leave_container(nanocbor_value_t *it, nanocbor_value_t *container)
{
    return _leave_container(it, container, NANOCBOR_RECURSION_MAX);
}

static int _skip_simple(nanocbor_value_t *it)
{
    int type = __get_type(it);
    uint64_t tmp = 0;
    if (type == NANOCBOR_TYPE_BSTR || type == NANOCBOR_TYPE_TSTR) {
        const uint8_t *tmp = NULL;
        size_t len = 0;
        return _get_str(it, &tmp, &len, (uint8_t)type);
    }
    int res = _get_uint64(it, &tmp, NANOCBOR_SIZE_LONG, type);
    res = _advance_if(it, res);
    return res > 0 ? NANOCBOR_OK : res;
}

int nanocbor_get_subcbor(nanocbor_value_t *it, const uint8_t **start,
                         size_t *len)
{
    *start = it->cur;
    int res = nanocbor_skip(it);
    *len = (size_t)(it->cur - *start);
    return res;
}

int nanocbor_skip_simple(nanocbor_value_t *it)
{
    return _skip_simple(it);
}

/* NOLINTNEXTLINE(misc-no-recursion): Recursion is limited by design */
static int _skip_limited(nanocbor_value_t *it, uint8_t limit)
{
    if (limit == 0) {
        return NANOCBOR_ERR_RECURSION;
    }
    int type = __get_type(it);
    int res = type;

    /* map or array */
    if (type == NANOCBOR_TYPE_ARR || type == NANOCBOR_TYPE_MAP) {
        nanocbor_value_t recurse;
        /* no need for passing down limit to _enter_* since we already know it is a real container */
        res = (type == NANOCBOR_TYPE_MAP ? nanocbor_enter_map(it, &recurse)
                                         : nanocbor_enter_array(it, &recurse));
        if (res == NANOCBOR_OK) {
            while (!nanocbor_at_end(&recurse)) {
                res = _skip_limited(&recurse, limit - 1);
                if (res < 0) {
                    break;
                }
            }
            nanocbor_leave_container(it, &recurse);
        }
    }
    /* tag */
    else if (type == NANOCBOR_TYPE_TAG) {
        uint64_t tmp = 0;
        int res = _get_uint64(it, &tmp, NANOCBOR_SIZE_WORD, NANOCBOR_TYPE_TAG);
        if (res >= 0) {
            it->cur += res;
            res = _skip_limited(it, limit - 1);
        }
    }
    /* other basic types */
    else if (type >= 0) {
        res = _skip_simple(it);
    }
    return res < 0 ? res : NANOCBOR_OK;
}

int nanocbor_skip(nanocbor_value_t *it)
{
    return _skip_limited(it, NANOCBOR_RECURSION_MAX);
}

// todo: start could be const?
int nanocbor_get_key_tstr(nanocbor_value_t *start, const char *key,
                          nanocbor_value_t *value)
{
    int res = NANOCBOR_NOT_FOUND;
    size_t len = strlen(key);
    *value = *start;

    while (!nanocbor_at_end(value)) {
        const uint8_t *s = NULL;
        size_t s_len = 0;

        res = nanocbor_get_tstr(value, &s, &s_len);
        if (res < 0) {
            break;
        }

        if (s_len == len && !strncmp(key, (const char *)s, len)) {
            res = NANOCBOR_OK;
            break;
        }

        res = nanocbor_skip(value);
        if (res < 0) {
            break;
        }
    }

    return res;
}
