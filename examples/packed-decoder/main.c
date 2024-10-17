/*
 * SPDX-License-Identifier: CC0-1.0
 */

#include <argp.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

 // todo: important: config.h needs to come before nanocbor.h!
#include "nanocbor/config.h"
#include "nanocbor/nanocbor.h"

#define CBOR_READ_BUFFER_BYTES (1 << 18) // 256kB
#define MAX_DEPTH 20

static const struct argp_option cmdline_options[] = {
    { "input", 'i', "input", 0, "Input file, - for stdin", 0 },
    { "output", 'o', "output", 0, "Output file, - for stdout", 0 },
    { 0 },
};

struct arguments {
    char *input;
    char *output;
};

static struct arguments _args = { false, NULL };

static char buffer_in[CBOR_READ_BUFFER_BYTES];
static uint8_t buffer_out[CBOR_READ_BUFFER_BYTES];

static error_t _parse_opts(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;
    switch (key) {
    case 'i':
        arguments->input = arg;
        break;
    case 'o':
        arguments->output = arg;
        break;
    case ARGP_KEY_END:
        if (!arguments->input || !arguments->output) {
            argp_usage(state);
        }
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static int _parse_type(nanocbor_value_t *value, nanocbor_encoder_t *enc);

/* NOLINTNEXTLINE(misc-no-recursion) */
static void _parse_cbor(nanocbor_value_t *it, nanocbor_encoder_t *enc)
{
    while (!nanocbor_at_end(it)) {
        int res = _parse_type(it, enc);

        if (res < 0) {
            printf("Err: %d\n", res);
            break;
        }
    }
}

/* NOLINTNEXTLINE(misc-no-recursion) */
static int _print_enter_map(nanocbor_value_t *value, nanocbor_encoder_t *enc)
{
    nanocbor_value_t map;
    if (nanocbor_enter_map(value, &map) >= NANOCBOR_OK) {
        bool indefinite = nanocbor_container_indefinite(&map);
        if (indefinite) {
            // todo: all such invocations should check the return value!
            nanocbor_fmt_map_indefinite(enc);
        }
        else {
            nanocbor_fmt_map(enc, nanocbor_map_items_remaining(&map));
        }
        _parse_cbor(&map, enc);
        nanocbor_leave_container(value, &map);
        if (indefinite) {
            nanocbor_fmt_end_indefinite(enc);
        }
        return 0;
    }
    return -1;
}

/* NOLINTNEXTLINE(misc-no-recursion) */
static int _print_enter_array(nanocbor_value_t *value, nanocbor_encoder_t *enc)
{
    nanocbor_value_t arr;
    if (nanocbor_enter_array(value, &arr) >= 0) {
        bool indefinite = nanocbor_container_indefinite(&arr);
        if (indefinite) {
            nanocbor_fmt_array_indefinite(enc);
        }
        else {
            nanocbor_fmt_array(enc, nanocbor_array_items_remaining(&arr));
        }
        _parse_cbor(&arr, enc);
        nanocbor_leave_container(value, &arr);
        if (indefinite) {
            nanocbor_fmt_end_indefinite(enc);
        }
        return 0;
    }
    return -1;
}

static int _print_float(nanocbor_value_t *value, nanocbor_encoder_t *enc)
{
    bool test = false;
    uint8_t simple = 0;
    float fvalue = 0;
    double dvalue = 0;
    if (nanocbor_get_bool(value, &test) >= NANOCBOR_OK) {
        nanocbor_fmt_bool(enc, test);
    }
    else if (nanocbor_get_null(value) >= NANOCBOR_OK) {
        nanocbor_fmt_null(enc);
    }
    else if (nanocbor_get_undefined(value) >= NANOCBOR_OK) {
        nanocbor_fmt_undefined(enc);
    }
    else if (nanocbor_get_simple(value, &simple) >= NANOCBOR_OK) {
        nanocbor_fmt_simple(enc, simple);
    }
    else  if (nanocbor_get_float(value, &fvalue) >= 0) {
        nanocbor_fmt_float(enc, fvalue);
    }
    else if (nanocbor_get_double(value, &dvalue) >= 0) {
        nanocbor_fmt_double(enc, dvalue);
    }
    else {
        return -1;
    }
    return 0;
}

/* NOLINTNEXTLINE(misc-no-recursion, readability-function-cognitive-complexity) */
static int _parse_type(nanocbor_value_t *value, nanocbor_encoder_t *enc)
{
    uint8_t type = nanocbor_get_type(value);
    // if (indent > MAX_DEPTH) {
    //     return -2;
    // }
    // todo: currently no recursion limit
    int res = 0;
    switch (type) {
    case NANOCBOR_TYPE_UINT: {
        uint64_t uint = 0;
        res = nanocbor_get_uint64(value, &uint);
        if (res >= 0) {
            nanocbor_fmt_uint(enc, uint);
        }
    } break;
    case NANOCBOR_TYPE_NINT: {
        int64_t nint = 0;
        res = nanocbor_get_int64(value, &nint);
        if (res >= 0) {
            nanocbor_fmt_int(enc, nint);
        }
    } break;
    case NANOCBOR_TYPE_BSTR: {
        const uint8_t *buf = NULL;
        size_t len = 0;
        res = nanocbor_get_bstr(value, &buf, &len);
        if (res >= 0) {
            if (!buf) {
                return -1;
            }
            nanocbor_put_bstr(enc, buf, len);
        }
    } break;
    case NANOCBOR_TYPE_TSTR: {
        const uint8_t *buf = NULL;
        size_t len = 0;
        res = nanocbor_get_tstr(value, &buf, &len);
        if (res >= 0) {
            nanocbor_put_tstrn(enc, (const char *)buf, len);
        }
    } break;
    case NANOCBOR_TYPE_ARR: {
        res = _print_enter_array(value, enc);
    } break;
    case NANOCBOR_TYPE_MAP: {
        res = _print_enter_map(value, enc);
    } break;
    case NANOCBOR_TYPE_FLOAT: {
        res = _print_float(value, enc);
    } break;
    case NANOCBOR_TYPE_TAG: {
        uint32_t tag = 0;
        int res = nanocbor_get_tag(value, &tag);
        if (res >= NANOCBOR_OK) {
            nanocbor_fmt_tag(enc, tag);
            _parse_type(value, 0);
        }
        break;
    }
    default:
        printf("Unsupported type\n");
        return -1;
    }
    if (res < 0) {
        return -1;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    struct argp arg_parse
        = { cmdline_options, _parse_opts, NULL, NULL, NULL, NULL, NULL };
    argp_parse(&arg_parse, argc, argv, 0, 0, &_args);

    FILE *fp = stdin;

    if (_args.input == NULL || _args.output == NULL) {
        return -1;
    }

    if (strcmp(_args.input, "-") != 0) {
        fp = fopen(_args.input, "rbe");
    }

    size_t len = fread(buffer_in, 1, sizeof(buffer_in), fp);
    if (len == sizeof(buffer_in)) {
        puts("Error: file too big!");
        return -1;
    }

    fclose(fp);
    printf("Unpacking %lu bytes...\n", (long unsigned)len);

    nanocbor_value_t it;
    nanocbor_decoder_init(&it, (uint8_t *)buffer_in, len);
    while (!nanocbor_at_end(&it)) {
        if (nanocbor_skip(&it) < 0) {
            break;
        }
    }

    nanocbor_encoder_t enc;
    nanocbor_encoder_init(&enc, buffer_out, sizeof(buffer_out));
    nanocbor_decoder_init_packed(&it, (uint8_t *)buffer_in, len);

    _parse_cbor(&it, &enc);
    printf("Unpacked to %lu bytes\n", (long unsigned)nanocbor_encoded_len(&enc));

    fp = stdout;
    if (strcmp(_args.output, "-") != 0) {
        fp = fopen(_args.output, "wbe");
    }

    len = fwrite(buffer_out, 1, nanocbor_encoded_len(&enc), fp);
    fclose(fp);

    return 0;
}
