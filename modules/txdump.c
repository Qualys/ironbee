/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee --- TxDump module
 *
 * @note This module can be disabled by configuring with the "--disable-devel"
 * option.
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/bytestr.h>
#include <ironbee/cfgmap.h>
#include <ironbee/context.h>
#include <ironbee/engine_state.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/flags.h>
#include <ironbee/list.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/release.h>
#include <ironbee/rule_engine.h>
#include <ironbee/site.h>
#include <ironbee/state_notify.h>
#include <ironbee/string.h>
#include <ironbee/strval.h>
#include <ironbee/util.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        txdump
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Several max constants
 */
static const size_t max_fmt = 128;          /**< Format buffer size */
static const size_t max_path_element = 64;  /**< Max size of a path element */

/**
 * TxDump enable flags
 */
#define TXDUMP_ENABLED (1 <<  0) /**< Enabled? */
#define TXDUMP_BASIC   (1 <<  1) /**< Dump basic TX info? */
#define TXDUMP_CONN    (1 <<  2) /**< Dump connection info? */
#define TXDUMP_CONTEXT (1 <<  3) /**< Dump context info? */
#define TXDUMP_REQLINE (1 <<  4) /**< Dump request line? */
#define TXDUMP_REQHDR  (1 <<  5) /**< Dump request header? */
#define TXDUMP_RESLINE (1 <<  6) /**< Dump response line? */
#define TXDUMP_RESHDR  (1 <<  7) /**< Dump response header? */
#define TXDUMP_FLAGS   (1 <<  8) /**< Dump TX flags? */
#define TXDUMP_ARGS    (1 <<  9) /**< Dump request args? */
#define TXDUMP_DATA    (1 << 10) /**< Dump TX Data? */
/** Default enable flags */
#define TXDUMP_DEFAULT                      \
    (                                       \
        TXDUMP_ENABLED |                    \
        TXDUMP_BASIC   |                    \
        TXDUMP_REQLINE |                    \
        TXDUMP_RESLINE                      \
    )
/** Headers enable flags */
#define TXDUMP_HEADERS                      \
    (                                       \
        TXDUMP_ENABLED |                    \
        TXDUMP_BASIC   |                    \
        TXDUMP_REQLINE |                    \
        TXDUMP_REQHDR  |                    \
        TXDUMP_RESLINE |                    \
        TXDUMP_RESHDR                       \
    )
/** All enable flags */
#define TXDUMP_ALL                          \
    (                                       \
        TXDUMP_ENABLED |                    \
        TXDUMP_BASIC   |                    \
        TXDUMP_CONTEXT |                    \
        TXDUMP_CONN    |                    \
        TXDUMP_REQLINE |                    \
        TXDUMP_REQHDR  |                    \
        TXDUMP_RESLINE |                    \
        TXDUMP_RESHDR  |                    \
        TXDUMP_FLAGS   |                    \
        TXDUMP_ARGS    |                    \
        TXDUMP_DATA                         \
    )

/** Transaction block flags */
#define TX_BLOCKED                                    \
    (                                                 \
        IB_TX_FBLOCK_ADVISORY |                       \
        IB_TX_FBLOCK_PHASE |                          \
        IB_TX_FBLOCK_IMMEDIATE                        \
    )

/** TxDump configuration */
typedef struct txdump_config_t txdump_config_t;

/**
 * Per-TxDump directive configuration
 */
typedef struct {
    ib_state_event_type_t  event;     /**< Event type */
    ib_state_hook_type_t   hook_type; /**< Hook type */
    const char            *name;      /**< Event name */
    ib_flags_t             flags;     /**< Flags defining what to txdump */
    ib_logger_level_t      level;     /**< IB Log level */
    FILE                  *fp;        /**< File pointer (or NULL) */
    const char            *dest;      /**< Copy of the destination string */
    txdump_config_t       *config;    /**< TxDump configuration data */
    const ib_module_t     *module;    /**< Pointer to module object */
} txdump_t;

/**
 * TxDump configuration
 */
struct txdump_config_t {
    ib_list_t *txdump_list;         /**< List of TxDump pointers */
};

/**
 * TxDump global configuration
 */
static txdump_config_t txdump_config = {
    .txdump_list = NULL
};

/**
 * Dump an item (variable args version)
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump Log parameters
 * @param[in] nspaces Number of leading spaces
 * @param[in] fmt printf-style format string
 * @param[in] ap Variable args list
 */
static void txdump_va(
    const ib_tx_t  *tx,
    const txdump_t *txdump,
    size_t          nspaces,
    const char     *fmt,
    va_list         ap
) VPRINTF_ATTRIBUTE(4);
static void txdump_va(
    const ib_tx_t  *tx,
    const txdump_t *txdump,
    size_t          nspaces,
    const char     *fmt,
    va_list         ap
)
{
    char fmtbuf[max_fmt+1];

    /* Limit # of leading spaces */
    if (nspaces > 32) {
        nspaces = 32;
    }

    /* Initialize the space buffer */
    for (size_t n = 0;  n < nspaces;  ++n) {
        fmtbuf[n] = ' ';
    }
    fmtbuf[nspaces] = '\0';
    strcat(fmtbuf, fmt);

    if (txdump->fp != NULL) {
        vfprintf(txdump->fp, fmtbuf, ap);
        fputs("\n", txdump->fp);
    }
    else {
        ib_log_tx_vex(tx, txdump->level, NULL, NULL, 0, fmtbuf, ap);
    }
}

/**
 * Dump an item
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump Log data
 * @param[in] nspaces Number of leading spaces to insert
 * @param[in] fmt printf-style format string
 */
static void txdump_v(
    const ib_tx_t  *tx,
    const txdump_t *txdump,
    size_t          nspaces,
    const char     *fmt, ...
) PRINTF_ATTRIBUTE(4, 5);
static void txdump_v(
    const ib_tx_t  *tx,
    const txdump_t *txdump,
    size_t          nspaces,
    const char     *fmt, ...
)
{
    va_list ap;

    va_start(ap, fmt);
    txdump_va(tx, txdump, nspaces, fmt, ap);
    va_end(ap);
}

/**
 * Flush the file stream
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 *
 * @returns Status code
 */
static ib_status_t txdump_flush(
    const ib_tx_t  *tx,
    const txdump_t *txdump)
{
    assert(tx != NULL);
    assert(txdump != NULL);

    if (txdump->fp != NULL) {
        fflush(txdump->fp);
    }
    return IB_OK;
}

/**
 * Escape and format a bytestr
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump Log data
 * @param[in] bs Byte string to log
 * @param[in] quotes Add surrounding quotes?
 * @param[in] maxlen Maximum string length (min = 6)
 *
 * @returns Formatted buffer
 */
static const char *format_bs(
    const ib_tx_t      *tx,
    const txdump_t     *txdump,
    const ib_bytestr_t *bs,
    bool                quotes,
    size_t              maxlen
)
{
    assert(txdump != NULL);
    assert(bs != NULL);
    assert( (maxlen == 0) || (maxlen > 6) );

    ib_status_t    rc;
    const uint8_t *bsptr = NULL;
    char          *escaped;
    size_t         len;
    size_t         size;
    const char    *empty = quotes ? "\"\"" : "";
    char          *cur;

    /* Handle NULL bytestring */
    if (bs != NULL) {
        bsptr = ib_bytestr_const_ptr(bs);
    }

    /* If the data is NULL, no need to escape */
    if (bsptr == NULL) {
        return "<None>";
    }

    /* Escape the string */
    rc = ib_util_hex_escape_alloc(tx->mp,
                                  ib_bytestr_length(bs), 5,
                                  &escaped, &size);
    if (rc != IB_OK) {
        return empty;
    }
    cur = escaped;
    if (quotes) {
        *escaped = '\"';
        ++cur;
        *cur = '\0';
    }
    len = ib_util_hex_escape_buf(bsptr, ib_bytestr_length(bs), cur, size-5);
    cur += len;

    /* Handle zero length case */
    if (len == 0) {
        return empty;
    }

    /* Add '...' if we need to crop the buffer */
    if ( (maxlen > 0) && (len > maxlen) ) {
        cur = escaped + (maxlen - 3);
        strcpy(cur, "...");
    }
    if (quotes) {
        strcat(cur, "\"");
    }

    return escaped;
}

/**
 * Log a bytestr
 *
 * @param[in] tx IronBee transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] label Label string
 * @param[in] bs Byte string to log
 * @param[in] maxlen Maximum string length
 *
 * @returns void
 */
static void txdump_bs(
    const ib_tx_t      *tx,
    const txdump_t     *txdump,
    size_t              nspaces,
    const char         *label,
    const ib_bytestr_t *bs,
    size_t              maxlen
)
{
    assert(tx != NULL);
    assert(txdump != NULL);
    assert(label != NULL);
    assert(bs != NULL);

    const char *buf;

    buf = format_bs(tx, txdump, bs, true, maxlen);
    if (buf != NULL) {
        txdump_v(tx, txdump, nspaces, "%s = %s", label, buf);
    }
}

/**
 * Log a field.
 *
 * Logs a field name and value; handles various field types.
 *
 * @param[in] tx IronBee transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] label Label string
 * @param[in] field Field to log
 * @param[in] maxlen Maximum string length
 *
 * @returns void
 */
static void txdump_field(
    const ib_tx_t     *tx,
    const txdump_t    *txdump,
    size_t             nspaces,
    const char        *label,
    const ib_field_t  *field,
    size_t             maxlen
)
{
    ib_status_t rc;

    /* Check the field name
     * Note: field->name is not always a null ('\0') terminated string */
    if (field == NULL) {
        txdump_v(tx, txdump, nspaces, "%s = <NULL>", label);
        return;
    }

    switch (field->type) {

    case IB_FTYPE_GENERIC :      /**< Generic data */
    {
        void *v;
        rc = ib_field_value(field, ib_ftype_generic_out(&v));
        if (rc == IB_OK) {
            txdump_v(tx, txdump, nspaces, "%s = %p", label, v);
        }
        break;
    }

    case IB_FTYPE_NUM :          /**< Numeric value */
    {
        ib_num_t n;
        rc = ib_field_value(field, ib_ftype_num_out(&n));
        if (rc == IB_OK) {
            txdump_v(tx, txdump, nspaces, "%s = %"PRId64"", label, n);
        }
        break;
    }

    case IB_FTYPE_NULSTR :       /**< NUL terminated string value */
    {
        const char *s;
        rc = ib_field_value(field, ib_ftype_nulstr_out(&s));
        if (rc == IB_OK) {
            if (maxlen > 0) {
                txdump_v(tx, txdump, nspaces,
                         "%s = \"%.*s...\"", label, (int)maxlen, s);
            }
            else {
                txdump_v(tx, txdump, nspaces,
                         "%s = \"%s\"", label, s);
            }
        }
        break;
    }

    case IB_FTYPE_BYTESTR :      /**< Byte string value */
    {
        const ib_bytestr_t *bs;
        rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
        if (rc == IB_OK) {
            txdump_bs(tx, txdump, nspaces, label, bs, maxlen);
        }
        break;
    }

    case IB_FTYPE_LIST :         /**< List */
    {
        const ib_list_t *lst;
        rc = ib_field_value(field, ib_ftype_list_out(&lst));
        if (rc == IB_OK) {
            size_t len = IB_LIST_ELEMENTS(lst);
            txdump_v(tx, txdump, nspaces,
                     "%s = [%zd]", label, len);
        }
        break;
    }

    case IB_FTYPE_SBUFFER :
        txdump_v(tx, txdump, nspaces, "%s = sbuffer", label);
        break;

    default:
        txdump_v(tx, txdump, nspaces,
                 "Unknown field type (%d)", field->type);
    }
}

/**
 * Log a header
 *
 * @param[in] tx IronBee transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] label Label string
 * @param[in] header Header to log
 *
 * @returns void
 */
static void txdump_header(
    const ib_tx_t             *tx,
    const txdump_t            *txdump,
    size_t                     nspaces,
    const char                *label,
    const ib_parsed_headers_t *header
)
{
    assert(tx != NULL);
    assert(txdump != NULL);
    assert(label != NULL);

    const ib_parsed_header_t *node;

    if (header == NULL) {
        txdump_v(tx, txdump, nspaces, "%s unavailable", label);
        return;
    }

    txdump_v(tx, txdump, nspaces, "%s", label);
    for (node = header->head; node != NULL; node = node->next) {
        const char *name  = format_bs(tx, txdump, node->name, false, 24);
        const char *value = format_bs(tx, txdump, node->value, true, 64);
        txdump_v(tx, txdump, nspaces+2, "%s = %s", name, value);
    }
}

/**
 * Build a path by appending the field name to an existing path.
 *
 * @param[in] tx IronBee transaction
 * @param[in] path Base path
 * @param[in] field Field whose name to append
 *
 * @returns Pointer to newly allocated path string
 */
static const char *build_path(
    const ib_tx_t      *tx,
    const char         *path,
    const ib_field_t   *field
)
{
    size_t   pathlen;
    size_t   fullpath_len;
    size_t   tmplen;
    char    *fullpath;
    ssize_t  nlen = (ssize_t)field->nlen;
    bool     truncated = false;

    if ( (nlen <= 0) || (field->name == NULL) ) {
        nlen = 0;
    }
    else if (nlen > (ssize_t)max_path_element) {
        size_t i;
        const char *p;
        for (i = 0, p=field->name; isprint(*p) && (i < max_path_element); ++i) {
            /* Do nothing */
        }
        nlen = i;
        truncated = true;
    }

    /* Special case */
    if ( (nlen == 0) || (field->name == NULL) ) {
        return path;
    }

    /* Allocate a path buffer */
    pathlen = strlen(path);
    fullpath_len = pathlen + (pathlen > 0 ? 2 : 1) + nlen + (truncated ? 3 : 0);
    fullpath = (char *)ib_mpool_alloc(tx->mp, fullpath_len);
    assert(fullpath != NULL);

    /* Copy in the base path */
    strcpy(fullpath, path);
    if (pathlen > 0) {
        strcat(fullpath, ":");
    }

    /* Append the field's name */
    tmplen = pathlen+(pathlen > 0 ? 1 : 0);
    memcpy(fullpath+tmplen, field->name, nlen);
    if (truncated) {
        strcpy(fullpath+tmplen+nlen, "...");
    }
    else {
        fullpath[fullpath_len-1] = '\0';
    }
    return fullpath;
}

/**
 * Dump a list
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] path Base path
 * @param[in] lst List to log
 *
 * @returns Status code
 */
static ib_status_t txdump_list(
    const ib_tx_t    *tx,
    const txdump_t   *txdump,
    size_t            nspaces,
    const char       *path,
    const ib_list_t  *lst
)
{
    ib_status_t rc;
    const ib_list_node_t *node = NULL;

    /* Loop through the list & log everything */
    IB_LIST_LOOP_CONST(lst, node) {
        const ib_field_t *field = (const ib_field_t *)node->data;
        const char *fullpath = NULL;

        switch (field->type) {
        case IB_FTYPE_GENERIC:
        case IB_FTYPE_NUM:
        case IB_FTYPE_NULSTR:
        case IB_FTYPE_BYTESTR:
            fullpath = build_path(tx, path, field);
            txdump_field(tx, txdump, nspaces, fullpath, field, 0);
            break;

        case IB_FTYPE_LIST:
        {
            const ib_list_t *v;
            // @todo Remove mutable once list is const correct.
            rc = ib_field_value(field, ib_ftype_list_out(&v));
            if (rc != IB_OK) {
                return rc;
            }

            fullpath = build_path(tx, path, field);
            txdump_field(tx, txdump, nspaces, fullpath, field, 0);
            txdump_list(tx, txdump, nspaces+2, fullpath, v);
            break;
        }

        default :
            break;
        }
    }

    /* Done */
    return IB_OK;
}

/**
 * Dump a context
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] context Context to dump
 *
 * @returns Status code
 */
static ib_status_t txdump_context(
    const ib_tx_t      *tx,
    const txdump_t     *txdump,
    size_t              nspaces,
    const ib_context_t *context
)
{
    const ib_site_t          *site = NULL;
    const ib_site_location_t *location = NULL;

    txdump_v(tx, txdump, nspaces, "Context");
    txdump_v(tx, txdump, nspaces+2, "Name = %s",
             ib_context_full_get(context) );

    ib_context_site_get(context, &site);
    if (site != NULL) {
        txdump_v(tx, txdump, nspaces+2, "Site name = %s", site->name);
        txdump_v(tx, txdump, nspaces+2, "Site ID = %s", site->id);
    }
    ib_context_location_get(context, &location);
    if (location != NULL) {
        txdump_v(tx, txdump, nspaces+2, "Location path = %s",
                 location->path);
    }

    return IB_OK;
}

/**
 * Dump a request line
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] line Request line to dump
 */
static void txdump_reqline(
    const ib_tx_t              *tx,
    const txdump_t             *txdump,
    size_t                      nspaces,
    const ib_parsed_req_line_t *line
)
{
    assert(tx != NULL);
    assert(txdump != NULL);

    if (line == NULL) {
        txdump_v(tx, txdump, nspaces, "Request line unavailable");
        return;
    }
    txdump_v(tx, txdump, nspaces, "Request line:");
    txdump_bs(tx, txdump, nspaces+2, "Raw", line->raw, 256);
    txdump_bs(tx, txdump, nspaces+2, "Method", line->method, 32);
    txdump_bs(tx, txdump, nspaces+2, "URI", line->uri, 256);
    txdump_bs(tx, txdump, nspaces+2, "Protocol", line->protocol, 32);
}

/**
 * Dump a response line
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 * @param[in] nspaces Number of leading spaces
 * @param[in] line Response line to dump
 */
static void txdump_resline(
    const ib_tx_t               *tx,
    const txdump_t              *txdump,
    size_t                       nspaces,
    const ib_parsed_resp_line_t *line
)
{
    assert(tx != NULL);
    assert(txdump != NULL);

    if (line == NULL) {
        txdump_v(tx, txdump, nspaces, "Response line unavailable");
        return;
    }

    txdump_v(tx, txdump, nspaces, "Response line:");
    txdump_bs(tx, txdump, nspaces+2, "Raw", line->raw, 256);
    txdump_bs(tx, txdump, nspaces+2, "Protocol", line->protocol, 32);
    txdump_bs(tx, txdump, nspaces+2, "Status", line->status, 32);
    txdump_bs(tx, txdump, nspaces+2, "Message", line->msg, 256);
}

/**
 * Log transaction details.
 *
 * Extract details from the transaction & dump them
 *
 * @param[in] ib IronBee object
 * @param[in] tx Transaction object
 * @param[in] txdump TxDump object
 *
 * @returns Status code
 */
static ib_status_t txdump_tx(
    const ib_engine_t *ib,
    const ib_tx_t     *tx,
    const txdump_t    *txdump)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(txdump != NULL);

    ib_status_t rc;

    /* No flags set: do nothing */
    if (!ib_flags_any(txdump->flags, TXDUMP_ENABLED) ) {
        return IB_OK;
    }

    /* Basic */
    if (ib_flags_all(txdump->flags, TXDUMP_BASIC) ) {
        char        buf[30];
        const char *id;

        ib_clock_timestamp(buf, &tx->tv_created);
        txdump_v(tx, txdump, 2, "IronBee Version = %s", IB_VERSION);

        /* Dump the engine's instance and sensor IDs */
        id = ib_engine_instance_id(ib);
        if (id != NULL) {
            txdump_v(tx, txdump, 2, "IronBee Instance ID = %s", id);
        }
        id = ib_engine_sensor_id(ib);
        if (id != NULL) {
            txdump_v(tx, txdump, 2, "Sensor ID = %s", id);
        }
        txdump_v(tx, txdump, 2, "Started = %s", buf);
        txdump_v(tx, txdump, 2, "Hostname = %s", tx->hostname);
        txdump_v(tx, txdump, 2, "Effective IP = %s", tx->remote_ipstr);
        txdump_v(tx, txdump, 2, "Path = %s", tx->path);
        txdump_v(tx, txdump, 2, "Blocking mode = %s",
                 ib_flags_any(tx->flags, IB_TX_FBLOCKING_MODE) ? "On" : "Off");

        if (ib_flags_any(tx->flags, TX_BLOCKED)) {
            txdump_v(tx, txdump, 2, "Block Code = %" PRId64, tx->block_status);
            if (ib_flags_any(tx->flags, IB_TX_FBLOCK_ADVISORY) ) {
                txdump_v(tx, txdump, 2, "Block: Advisory");
            }
            if (ib_flags_any(tx->flags, IB_TX_FBLOCK_PHASE) ) {
                txdump_v(tx, txdump, 2, " Block: Phase");
            }
            if (ib_flags_any(tx->flags, IB_TX_FBLOCK_IMMEDIATE) ) {
                txdump_v(tx, txdump, 2, "Block: Immediate");
            }
        }
    }

    /* Context info */
    if (ib_flags_all(txdump->flags, TXDUMP_CONTEXT) ) {
        txdump_context(tx, txdump, 2, tx->ctx);
    }

    /* Connection */
    if (ib_flags_all(txdump->flags, TXDUMP_CONN) ) {
        char buf[30];
        ib_clock_timestamp(buf, &tx->conn->tv_created);
        txdump_v(tx, txdump, 2, "Connection");
        txdump_v(tx, txdump, 4, "ID = %s", tx->conn->id);
        txdump_v(tx, txdump, 4, "Created = %s", buf);
        txdump_v(tx, txdump, 4, "Remote = %s:%d",
                        tx->conn->remote_ipstr, tx->conn->remote_port);
        txdump_v(tx, txdump, 4, "Local = %s:%d",
                        tx->conn->local_ipstr, tx->conn->local_port);
        if (ib_flags_all(txdump->flags, TXDUMP_CONTEXT) ) {
            txdump_context(tx, txdump, 4, tx->conn->ctx);
        }
    }

    /* Request Line */
    if (ib_flags_all(txdump->flags, TXDUMP_REQLINE) ) {
        txdump_reqline(tx, txdump, 2, tx->request_line);
    }

    /* Request Header */
    if (ib_flags_all(txdump->flags, TXDUMP_REQHDR) ) {
        txdump_header(tx, txdump, 2,
                      "Request Header", tx->request_header);
    }

    /* Response Line */
    if (ib_flags_all(txdump->flags, TXDUMP_RESLINE) ) {
        txdump_resline(tx, txdump, 2, tx->response_line);
    }

    /* Response Header */
    if (ib_flags_all(txdump->flags, TXDUMP_RESHDR) ) {
        txdump_header(tx, txdump, 2,
                      "Response Header", tx->response_header);
    }

    /* Flags */
    if (ib_flags_all(txdump->flags, TXDUMP_FLAGS) ) {
        const ib_strval_t *rec;

        txdump_v(tx, txdump, 2,
                 "Flags = %010lx", (unsigned long)tx->flags);
        IB_STRVAL_LOOP(ib_tx_flags_strval_first(), rec) {
            bool on = ib_flags_any(tx->flags, rec->val);
            txdump_v(tx, txdump, 4, "%010lx \"%s\" = %s",
                     (unsigned long)rec->val, rec->str,
                     on ? "On" : "Off");
        }
    }

    /* If the transaction never started, do nothing */
    if (! ib_flags_all(tx->flags, IB_TX_FREQ_STARTED) ) {
        return IB_OK;
    }

    /* ARGS */
    if (ib_flags_all(txdump->flags, TXDUMP_ARGS) ) {
        const ib_list_t *lst;
        ib_field_t *field;
        ib_var_source_t *source;
        txdump_v(tx, txdump, 2, "ARGS:");
        rc = ib_var_source_acquire(
            &source,
            tx->mp,
            ib_engine_var_config_get_const(ib),
            IB_S2SL("ARGS")
        );
        if (rc == IB_OK) {
            rc = ib_var_source_get(source, &field, tx->var_store);
        }
        if (rc == IB_OK) {
            txdump_field(tx, txdump, 4, "ARGS", field, 0);

            rc = ib_field_value(field, ib_ftype_list_out(&lst));
            if ( (rc != IB_OK) || (lst == NULL) ) {
                return rc;
            }
            txdump_list(tx, txdump, 4, "ARGS", lst);
        }
        else {
            ib_log_debug_tx(tx, "log_tx: Failed to get ARGS: %s",
                            ib_status_to_string(rc));
        }
    }

    /* All data fields */
    if (ib_flags_all(txdump->flags, TXDUMP_DATA) ) {
        ib_list_t *lst;
        txdump_v(tx, txdump, 2, "Data:");

        /* Build the list */
        rc = ib_list_create(&lst, tx->mp);
        if (rc != IB_OK) {
            ib_log_debug_tx(tx, "log_tx: Failed to create tx list: %s",
                            ib_status_to_string(rc));
            return IB_EUNKNOWN;
        }

        /* Extract the request headers field from the provider instance */
        ib_var_store_export(tx->var_store, lst);

        /* Log it all */
        rc = txdump_list(tx, txdump, 4, "", lst);
        if (rc != IB_OK) {
            ib_log_debug_tx(tx, "log_tx: Failed logging headers: %s",
                            ib_status_to_string(rc));
            return rc;
        }
    }

    /* Done */
    txdump_flush(tx, txdump);
    return IB_OK;
}

/**
 * Check if this TX should be dumped by this TxDump
 *
 * @param[in] tx IronBee Transaction
 * @param[in] txdump TxDump data
 *
 * @returns True if this TX should be dumped
 */

static bool txdump_check_tx(
    const ib_tx_t  *tx,
    const txdump_t *txdump
)
{
    ib_status_t           rc;
    const ib_list_node_t *node;
    txdump_config_t      *config;

    /* Get my module configuration */
    rc = ib_context_module_config(tx->ctx, txdump->module, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to get %s module configuration: %s",
                        txdump->module->name, ib_status_to_string(rc));
        return false;
    }

    /* Loop through the TX's context configuration, see if this TxDump
     * is in the list.  Do nothing if there is no list. */
    if (config->txdump_list == NULL) {
        return false;
    }
    IB_LIST_LOOP_CONST(config->txdump_list, node) {
        const txdump_t *tmp = ib_list_node_data_const(node);
        if (tmp == txdump) {
            return true;
        }
    }

    /* This TxDump is not in the context configuration's list */
    return false;
}


/**
 * Handle a TX event for TxDump
 *
 * @param[in] ib IronBee object
 * @param[in] tx Transaction object
 * @param[in] event Event type
 * @param[in] cbdata Callback data (TxDump object)
 *
 * @returns Status code
 */
static ib_status_t txdump_tx_event(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(cbdata != NULL);

    const txdump_t *txdump = (const txdump_t *)cbdata;
    ib_status_t     rc;

    assert(txdump->event == event);
    if (!txdump_check_tx(tx, txdump)) {
        return IB_OK;
    }

    txdump_v(tx, txdump, 0, "[TX %s @ %s]", tx->id, txdump->name);

    rc = txdump_tx(ib, tx, txdump);
    txdump_flush(tx, txdump);
    return rc;
}

/**
 * Handle a Request Line event for TxDump
 *
 * @param[in] ib IronBee object
 * @param[in] tx Transaction object
 * @param[in] event Event type
 * @param[in] line Parsed request line
 * @param[in] cbdata Callback data (TxDump object)
 *
 * @returns Status code
 */
static ib_status_t txdump_reqline_event(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    ib_parsed_req_line_t  *line,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(cbdata != NULL);

    const txdump_t *txdump = (const txdump_t *)cbdata;

    assert(txdump->event == event);
    if (!txdump_check_tx(tx, txdump)) {
        return IB_OK;
    }

    txdump_v(tx, txdump, 0, "[TX %s @ %s]", tx->id, txdump->name);
    txdump_reqline(tx, txdump, 2, line);
    txdump_flush(tx, txdump);
    return IB_OK;
}

/**
 * Handle a TX event for TxDump
 *
 * @param[in] ib IronBee object
 * @param[in] tx Transaction object
 * @param[in] event Event type
 * @param[in] line Parsed response line
 * @param[in] cbdata Callback data (TxDump object)
 *
 * @returns Status code
 */
static ib_status_t txdump_resline_event(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    ib_parsed_resp_line_t *line,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(cbdata != NULL);

    const txdump_t *txdump = (const txdump_t *)cbdata;

    assert(txdump->event == event);
    if (!txdump_check_tx(tx, txdump)) {
        return IB_OK;
    }

    txdump_v(tx, txdump, 0, "[TX %s @ %s]", tx->id, txdump->name);
    txdump_resline(tx, txdump, 2, line);
    txdump_flush(tx, txdump);
    return IB_OK;
}

/**
 * Execute function for the "TxDump" action
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data C-style string to log
 * @param[in] cbdata Callback data (TxDump object)
 *
 * @returns Status code
 */
static ib_status_t txdump_act_execute(
    const ib_rule_exec_t *rule_exec,
    void                 *data,
    void                 *cbdata
)
{
    const txdump_t *txdump = (const txdump_t *)data;
    ib_status_t     rc;
    const ib_tx_t  *tx = rule_exec->tx;

    txdump_v(tx, txdump, 0, "[TX %s @ Rule %s]",
             tx->id, ib_rule_id(rule_exec->rule));

    rc = txdump_tx(rule_exec->ib, tx, txdump);
    txdump_flush(tx, txdump);
    return rc;
}

/**
 * TxDump event data
 */
typedef struct {
    ib_state_event_type_t event;      /**< Event type */
    ib_state_hook_type_t  hook_type;  /**< Hook type */
} txdump_event_t;

/**
 * TxDump event parsing mapping data
 */
typedef struct {
    const char           *str;        /** String< "key" */
    const txdump_event_t  data;       /** Data portion */
} txdump_strval_event_t;

static IB_STRVAL_DATA_MAP(txdump_strval_event_t, event_map) = {
    IB_STRVAL_DATA_PAIR("PostProcess",
                        handle_postprocess_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("Logging",
                        handle_logging_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("RequestStart",
                        request_started_event,
                        IB_STATE_HOOK_REQLINE),
    IB_STRVAL_DATA_PAIR("RequestHeaderFinished",
                        request_header_finished_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("RequestHeader",
                        request_header_finished_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("Request",
                        handle_request_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("ResponseStart",
                        response_started_event,
                        IB_STATE_HOOK_RESPLINE),
    IB_STRVAL_DATA_PAIR("ResponseHeaderFinished",
                        response_header_finished_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("ResponseHeader",
                        response_header_finished_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("TxStarted",
                        tx_started_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("TxContext",
                        handle_context_tx_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("TxProcess",
                        tx_process_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR("TxFinished",
                        tx_finished_event,
                        IB_STATE_HOOK_TX),
    IB_STRVAL_DATA_PAIR_LAST((ib_state_event_type_t)-1,
                             (ib_state_hook_type_t)-1),
};

/**
 * Parse the event for a TxDump directive
 *
 * @param[in] ib IronBee engine
 * @param[in] label Label for logging
 * @param[in] param Parameter string
 * @param[in,out] txdump TxDump object to set the event in
 *
 * @returns: Status code
 */
static ib_status_t txdump_parse_event(
    ib_engine_t  *ib,
    const char   *label,
    const char   *param,
    txdump_t     *txdump
)
{
    assert(ib != NULL);
    assert(label != NULL);
    assert(param != NULL);
    assert(txdump != NULL);

    ib_status_t           rc;
    const txdump_event_t *value;

    rc = IB_STRVAL_DATA_LOOKUP(event_map, txdump_strval_event_t, param, &value);
    if (rc != IB_OK) {
        ib_log_error(ib, "Invalid event parameter \"%s\" for %s.", param, label);
        return rc;
    }

#ifndef __clang_analyzer__
    txdump->event = value->event;
    txdump->hook_type = value->hook_type;
#endif
    txdump->name = ib_state_event_name(txdump->event);

    return IB_OK;
}

/**
 * Parse the destination for a TxDump directive or action
 *
 * @param[in] ib IronBee engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] label Label for logging
 * @param[in] param Parameter string
 * @param[in,out] txdump TxDump object to set the event in
 *
 * @returns: Status code
 */
static ib_status_t txdump_parse_dest(
    ib_engine_t *ib,
    ib_mpool_t  *mp,
    const char  *label,
    const char  *param,
    txdump_t    *txdump
)
{
    assert(ib != NULL);
    assert(label != NULL);
    assert(param != NULL);
    assert(txdump != NULL);

    txdump->dest = ib_mpool_strdup(mp, param);
    if (strcasecmp(param, "StdOut") == 0) {
        txdump->fp = ib_util_fdup(stdout, "a");
        if (txdump->fp == NULL) {
            return IB_EUNKNOWN;
        }
    }
    else if (strcasecmp(param, "StdErr") == 0) {
        txdump->fp = ib_util_fdup(stderr, "a");
        if (txdump->fp == NULL) {
            return IB_EUNKNOWN;
        }
    }
    else if (strncasecmp(param, "file://", 7) == 0) {
        const char *mode;
        char       *fname;
        char       *end;
        size_t      len;

        /* Make a copy of the file name */
        fname = ib_mpool_strdup(mp, param + 7);
        if (fname == NULL) {
            return IB_EALLOC;
        }
        len = strlen(fname);
        if (len <= 1) {
            ib_log_error(ib, "Missing file name for %s.", label);
            return IB_EINVAL;
        }

        /* If the last character is a '+', open in append mode */
        end = fname + len - 1;
        if (*end == '+') {
            mode = "a";
            *end = '\0';
        }
        else {
            mode = "w";
        }
        txdump->fp = fopen(fname, mode);
        if (txdump->fp == NULL) {
            ib_log_error(ib, "Failed to open \"%s\" for %s: %s",
                         fname, label, strerror(errno));
            return IB_EINVAL;
        }
    }
    else if (strcasecmp(param, "ib") == 0) {
        txdump->level = IB_LOG_DEBUG;
    }
    else {
        ib_log_error(ib, "Invalid destination \"%s\" for %s.", param, label);
        return IB_EINVAL;
    }
    return IB_OK;
}

static IB_STRVAL_MAP(flags_map) = {
    IB_STRVAL_PAIR("default", TXDUMP_DEFAULT),
    IB_STRVAL_PAIR("basic", TXDUMP_BASIC),
    IB_STRVAL_PAIR("context", TXDUMP_CONTEXT),
    IB_STRVAL_PAIR("connection", TXDUMP_CONN),
    IB_STRVAL_PAIR("reqline", TXDUMP_REQLINE),
    IB_STRVAL_PAIR("reqhdr", TXDUMP_REQHDR),
    IB_STRVAL_PAIR("resline", TXDUMP_RESLINE),
    IB_STRVAL_PAIR("reshdr", TXDUMP_RESHDR),
    IB_STRVAL_PAIR("headers", TXDUMP_HEADERS),
    IB_STRVAL_PAIR("flags", TXDUMP_FLAGS),
    IB_STRVAL_PAIR("args", TXDUMP_ARGS),
    IB_STRVAL_PAIR("data", TXDUMP_DATA),
    IB_STRVAL_PAIR("all", TXDUMP_ALL),
    IB_STRVAL_PAIR_LAST,
};

/**
 * Handle the TxDump directive
 *
 * @param[in] cp Config parser
 * @param[in] directive Directive name
 * @param[in] params List of directive parameters
 * @param[in] cbdata Callback data (module)
 *
 * @returns Status code
 *
 * @par usage: TxDump @<event@> @<dest@> [@<enable@>]
 * @par @<event@> is one of:
 *  - <tt>TxStarted</tt>
 *  - <tt>TxProcess</tt>
 *  - <tt>TxContext</tt>
 *  - <tt>RequestStart</tt>
 *  - <tt>RequestHeader</tt>
 *  - <tt>Request</tt>
 *  - <tt>ResponseStart</tt>
 *  - <tt>ResponseHeader</tt>
 *  - <tt>Response</tt>
 *  - <tt>TxFinished</tt>
 *  - <tt>Logging</tt>
 *  - <tt>PostProcess</tt>
 * @par @<dest@> is of the form (stderr|stdout|ib|file://@<path@>[+])
 *  - The '+' flag means append
 * @par @<enable@> is of the form @<flag@> [[+-]@<flag@>]>
 * @par @<flag@> is one of:
 *  - <tt>Basic</tt>: Dump basic TX info
 *  - <tt>Context</tt>: Dump context info
 *  - <tt>Connection</tt>: Dump connection info
 *  - <tt>ReqLine</tt>: Dump request line
 *  - <tt>ReqHdr</tt>: Dump request header
 *  - <tt>ResLine</tt>: Dump response line
 *  - <tt>ResHdr</tt>: Dump response header
 *  - <tt>Flags</tt>: Dump TX flags
 *  - <tt>Args</tt>: Dump request args
 *  - <tt>Data</tt>: Dump TX Data
 *  - <tt>Default</tt>: Default flags (Basic, ReqLine, ResLine)
 *  - <tt>Headers</tt>: All headers (Basic, ReqLine, ReqHdr, ResLine, ResHdr)
 *  - <tt>All</tt>: Dump all TX information
 *
 * @par Examples:
 *  - <tt>TxDump TxContext ib Basic +Context</tt>
 *  - <tt>TxDump PostProcess file:///tmp/tx.txt All</tt>
 *  - <tt>TxDump Logging file:///var/log/ib/all.txt+ All</tt>
 *  - <tt>TxDump PostProcess StdOut All</tt>
 */
static ib_status_t txdump_handler(
    ib_cfgparser_t  *cp,
    const char      *directive,
    const ib_list_t *params,
    void            *cbdata
)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);
    assert(directive != NULL);
    assert(params != NULL);
    assert(cbdata != NULL);

    ib_status_t           rc;
    ib_mpool_t           *mp = cp->mp;
    const ib_module_t    *module = cbdata;
    ib_context_t         *context;
    txdump_config_t      *config;
    txdump_t              txdump;
    txdump_t             *ptxdump;
    const ib_list_node_t *node;
    const char           *param;
    static const char    *label = "TxDump directive";
    int                   flagno = 0;
    ib_flags_t            flags = 0;
    ib_flags_t            mask = 0;

    /* Get my configuration context */
    rc = ib_cfgparser_context_current(cp, &context);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Txdump: Failed to get current context: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Get my module configuration */
    rc = ib_context_module_config(context, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the txdump object */
    memset(&txdump, 0, sizeof(txdump));
    txdump.config = config;
    txdump.module = module;

    /* First parameter is event type */
    node = ib_list_first_const(params);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp,
                         "Missing event type for %s.", label);
        return IB_EINVAL;
    }
    param = (const char *)node->data;
    rc = txdump_parse_event(cp->ib, label, param, &txdump);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error parsing event for %s.", label);
        return rc;
    }

    /* Second parameter is the destination */
    node = ib_list_node_next_const(node);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp, "Missing destination for %s.", label);
        return IB_EINVAL;
    }
    param = (const char *)node->data;
    rc = txdump_parse_dest(cp->ib, mp, label, param, &txdump);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error parsing destination for %s: %s",
                         label, ib_status_to_string(rc));
        return rc;
    }

    /* Parse the remainder of the parameters a enables / disables */
    while( (node = ib_list_node_next_const(node)) != NULL) {
        assert(node->data != NULL);
        param = (const char *)node->data;
        rc = ib_flags_string(flags_map, param, flagno++, &flags, &mask);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error parsing enable for %s: %s",
                             label, ib_status_to_string(rc));
            return rc;
        }
    }
    txdump.flags = ib_flags_merge(TXDUMP_DEFAULT, flags, mask);
    if (txdump.flags != 0) {
        txdump.flags |= TXDUMP_ENABLED;
    }

    /* Create the txdump entry */
    ptxdump = ib_mpool_memdup(mp, &txdump, sizeof(txdump));
    if (ptxdump == NULL) {
        return IB_EALLOC;
    }

    /* Create the list if required */
    if (config->txdump_list == NULL) {
        rc = ib_list_create(&config->txdump_list, mp);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error creating TxDump list for %s",
                             label);
            return rc;
        }
    }

    /* Add it to the list */
    rc = ib_list_push(config->txdump_list, ptxdump);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error adding TxDump object to list for %s: %s",
                         label, ib_status_to_string(rc));
        return rc;
    }

    /* Finally, register the callback */
    switch(txdump.hook_type) {
    case IB_STATE_HOOK_TX:
        rc = ib_hook_tx_register(
            cp->ib,
            txdump.event,
            txdump_tx_event,
            ptxdump);
        break;
    case IB_STATE_HOOK_REQLINE:
        rc = ib_hook_parsed_req_line_register(
            cp->ib,
            txdump.event,
            txdump_reqline_event,
            ptxdump);
        break;
    case IB_STATE_HOOK_RESPLINE:
        rc = ib_hook_parsed_resp_line_register(
            cp->ib,
            txdump.event,
            txdump_resline_event,
            ptxdump);
        break;
    default:
        ib_cfg_log_error(cp, "No handler for hook type %d.", txdump.hook_type);
        return IB_EINVAL;
    }
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to register handler for hook type %d: %s",
                         txdump.hook_type, ib_status_to_string(rc));
    }

    return IB_OK;
}

/**
 * Create function for the TxDump action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Callback data (unused)
 *
 * @returns Status code
 *
 * @par usage: TxDump:@<dest@>,[@<enable@>]
 * @par @<dest@> is of the form (stderr|stdout|ib|file://@<path@>[+])
 *  - The '+' flag means append
 * @par @<enable@> is of the form @<flag@> [[+-]@<flag@>]>
 * @par @<flag@> is one of:
 *  - <tt>Basic</tt>: Dump basic TX info
 *  - <tt>Context</tt>: Dump context info
 *  - <tt>Connection</tt>: Dump connection info
 *  - <tt>ReqLine</tt>: Dump request line
 *  - <tt>ReqHdr</tt>: Dump request header
 *  - <tt>ResLine</tt>: Dump response line
 *  - <tt>ResHdr</tt>: Dump response header
 *  - <tt>Flags</tt>: Dump TX flags
 *  - <tt>Args</tt>: Dump request args
 *  - <tt>Data</tt>: Dump TX Data
 *  - <tt>Default</tt>: Default flags (Basic, ReqLine, ResLine)
 *  - <tt>Headers</tt>: All headers (Basic, ReqLine, ReqHdr, ResLine, ResHdr)
 *  - <tt>All</tt>: Dump all TX information
 *
 * @par Examples:
 *  - <tt>TxDump:ib,Basic,+Context</tt>
 *  - <tt>TxDump:file:///tmp/tx.txt,All</tt>
 *  - <tt>TxDump:file:///var/log/ib/all.txt+,All</tt>
 *  - <tt>TxDump:StdOut,All</tt>
 */
static ib_status_t txdump_act_create(
    ib_engine_t      *ib,
    const char       *parameters,
    ib_action_inst_t *inst,
    void             *cbdata
)
{
    assert(ib != NULL);
    assert(inst != NULL);

    ib_status_t        rc;
    txdump_t           txdump;
    txdump_t          *ptxdump;
    char              *pcopy;
    char              *param;
    static const char *label = "TxDump action";
    int                flagno = 0;
    ib_flags_t         flags = 0;
    ib_flags_t         mask = 0;
    ib_mpool_t        *mp = ib_engine_pool_main_get(ib);

    assert(mp != NULL);

    if (parameters == NULL) {
        return IB_EINVAL;
    }

    /* Initialize the txdump object */
    memset(&txdump, 0, sizeof(txdump));
    txdump.name = "Action";

    /* Make a copy of the parameters that we can use for strtok() */
    pcopy = ib_mpool_strdup(ib_engine_pool_temp_get(ib), parameters);
    if (pcopy == NULL) {
        return IB_EALLOC;
    }

    /* First parameter is the destination */
    param = strtok(pcopy, ",");
    if (param == NULL) {
        ib_log_error(ib, "Missing destination for %s.", label);
        return IB_EINVAL;
    }
    rc = txdump_parse_dest(ib, mp, label, param, &txdump);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error parsing destination for %s.", label);
        return rc;
    }

    /* Parse the remainder of the parameters a enables / disables */
    while ((param = strtok(NULL, ",")) != NULL) {
        rc = ib_flags_string(flags_map, param, flagno++, &flags, &mask);
        if (rc != IB_OK) {
            ib_log_error(ib, "Error parsing enable for %s.", label);
            return rc;
        }
    }
    txdump.flags = ib_flags_merge(TXDUMP_DEFAULT, flags, mask);
    if (txdump.flags != 0) {
        txdump.flags |= TXDUMP_ENABLED;
    }

    /* Create the txdump entry */
    ptxdump = ib_mpool_memdup(mp, &txdump, sizeof(txdump));
    if (ptxdump == NULL) {
        ib_log_error(ib, "Error allocating TxDump object for %s.", label);
        return IB_EALLOC;
    }

    /* Done */
    inst->data = ptxdump;
    return IB_OK;
}

/**
 * Handle copying configuration data for the TxDump module
 *
 * @param[in] ib     Engine handle
 * @param[in] module Module
 * @param[in] dst    Destination of data.
 * @param[in] src    Source of data.
 * @param[in] length Length of data.
 * @param[in] cbdata Callback data
 *
 * @returns Status code
 */
static ib_status_t txdump_config_copy(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *dst,
    const void  *src,
    size_t       length,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);
    assert(dst != NULL);
    assert(src != NULL);
    assert(length == sizeof(txdump_config));

    ib_status_t            rc;
    txdump_config_t       *dst_config = dst;
    const txdump_config_t *src_config = src;
    ib_mpool_t            *mp = ib_engine_pool_main_get(ib);

    /* If there is no source list, do nothing. */
    if (src_config->txdump_list == NULL) {
        return IB_OK;
    }

    /* Otherwise, copy nodes from the source list */
    rc = ib_list_copy(src_config->txdump_list, mp, &dst_config->txdump_list);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Initialize the txdump module.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t txdump_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);
    ib_status_t rc;

    /* Register the TxDump directive */
    rc = ib_config_register_directive(ib,
                                      "TxDump",
                                      IB_DIRTYPE_LIST,
                                      (ib_void_fn_t)txdump_handler,
                                      NULL,
                                      module,
                                      NULL,
                                      NULL);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register TxDump directive: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the TxDump action */
    rc = ib_action_register(ib,
                            "TxDump",
                            txdump_act_create, NULL,
                            NULL, NULL, /* no destroy function */
                            txdump_act_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Module structure.
 *
 * This structure defines some metadata, config data and various functions.
 */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,               /* Default metadata */
    MODULE_NAME_STR,                         /* Module name */
    &txdump_config, sizeof(txdump_config),   /* Global config data */
    txdump_config_copy, NULL,                /* Config copy function */
    NULL,                                    /* Module config map */
    NULL,                                    /* Module directive map */
    txdump_init,                             /* Initialize function */
    NULL,                                    /* Callback data */
    NULL,                                    /* Finish function */
    NULL,                                    /* Callback data */
);
