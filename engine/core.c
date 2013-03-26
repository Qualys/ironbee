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
 * @brief IronBee --- Core Module
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/core.h>

#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE
#endif

#include "core_private.h"
#include "core_audit_private.h"
#include "engine_private.h"
#include "rule_engine_private.h"
#include "managed_collection_private.h"

#include <ironbee/bytestr.h>
#include <ironbee/cfgmap.h>
#include <ironbee/clock.h>
#include <ironbee/context_selection.h>
#include <ironbee/engine_types.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/json.h>
#include <ironbee/logevent.h>
#include <ironbee/collection_manager.h>
#include <ironbee/mpool.h>
#include <ironbee/provider.h>
#include <ironbee/rule_defs.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#if defined(__cplusplus) && !defined(__STDC_FORMAT_MACROS)
/* C99 requires that inttypes.h only exposes PRI* macros
 * for C++ implementations if this is defined: */
#define __STDC_FORMAT_MACROS
#endif
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#define MODULE_NAME        core
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

IB_MODULE_DECLARE();

/* The default UUID value */
static const char * const ib_uuid_default_str = "00000000-0000-0000-0000-000000000000";

#ifndef MODULE_BASE_PATH
/* Always define a module base path. */
#define MODULE_BASE_PATH /usr/local/ironbee/lib
#endif

#ifndef RULE_BASE_PATH
/* Always define a rule base path. */
#define RULE_BASE_PATH /usr/local/ironbee/lib
#endif

/// @todo Fix this:
#ifndef X_MODULE_BASE_PATH
#define X_MODULE_BASE_PATH IB_XSTRINGIFY(MODULE_BASE_PATH) "/"
#endif

/// @todo Fix this:
#ifndef X_RULE_BASE_PATH
#define X_RULE_BASE_PATH IB_XSTRINGIFY(RULE_BASE_PATH) "/"
#endif


/* Instantiate a module global configuration. */
static ib_core_cfg_t core_global_cfg;


#define IB_ALPART_HEADER                  (1<< 0)
#define IB_ALPART_EVENTS                  (1<< 1)
#define IB_ALPART_HTTP_REQUEST_METADATA   (1<< 2)
#define IB_ALPART_HTTP_REQUEST_HEADER     (1<< 3)
#define IB_ALPART_HTTP_REQUEST_BODY       (1<< 4)
#define IB_ALPART_HTTP_REQUEST_TRAILER    (1<< 5)
#define IB_ALPART_HTTP_RESPONSE_METADATA  (1<< 6)
#define IB_ALPART_HTTP_RESPONSE_HEADER    (1<< 7)
#define IB_ALPART_HTTP_RESPONSE_BODY      (1<< 8)
#define IB_ALPART_HTTP_RESPONSE_TRAILER   (1<< 9)
#define IB_ALPART_DEBUG_FIELDS            (1<<10)

/* NOTE: Make sure to add new parts from above to any groups below. */

#define IB_ALPARTS_ALL \
    IB_ALPART_HEADER|IB_ALPART_EVENTS| \
    IB_ALPART_HTTP_REQUEST_METADATA|IB_ALPART_HTTP_REQUEST_HEADER |\
    IB_ALPART_HTTP_REQUEST_BODY|IB_ALPART_HTTP_REQUEST_TRAILER | \
    IB_ALPART_HTTP_RESPONSE_METADATA|IB_ALPART_HTTP_RESPONSE_HEADER | \
    IB_ALPART_HTTP_RESPONSE_BODY|IB_ALPART_HTTP_RESPONSE_TRAILER | \
    IB_ALPART_DEBUG_FIELDS

#define IB_ALPARTS_DEFAULT \
    IB_ALPART_HEADER|IB_ALPART_EVENTS| \
    IB_ALPART_HTTP_REQUEST_METADATA|IB_ALPART_HTTP_REQUEST_HEADER |\
    IB_ALPART_HTTP_REQUEST_TRAILER | \
    IB_ALPART_HTTP_RESPONSE_METADATA|IB_ALPART_HTTP_RESPONSE_HEADER | \
    IB_ALPART_HTTP_RESPONSE_TRAILER

#define IB_ALPARTS_REQUEST \
    IB_ALPART_HTTP_REQUEST_METADATA|IB_ALPART_HTTP_REQUEST_HEADER |\
    IB_ALPART_HTTP_REQUEST_BODY|IB_ALPART_HTTP_REQUEST_TRAILER

#define IB_ALPARTS_RESPONSE \
    IB_ALPART_HTTP_RESPONSE_METADATA|IB_ALPART_HTTP_RESPONSE_HEADER | \
    IB_ALPART_HTTP_RESPONSE_BODY|IB_ALPART_HTTP_RESPONSE_TRAILER


/* Rule log parts amalgamation */
#define IB_RULE_LOG_FLAGS_REQUEST                               \
    ( IB_RULE_LOG_FLAG_REQ_LINE |                               \
      IB_RULE_LOG_FLAG_REQ_HEADER |                             \
      IB_RULE_LOG_FLAG_REQ_BODY )
#define IB_RULE_LOG_FLAGS_RESPONSE                   \
    ( IB_RULE_LOG_FLAG_RSP_LINE |                    \
      IB_RULE_LOG_FLAG_RSP_HEADER |                  \
      IB_RULE_LOG_FLAG_RSP_BODY )
#define IB_RULE_LOG_FLAGS_EXEC                         \
    ( IB_RULE_LOG_FLAG_PHASE |                         \
      IB_RULE_LOG_FLAG_RULE |                          \
      IB_RULE_LOG_FLAG_TARGET |                        \
      IB_RULE_LOG_FLAG_TFN |                           \
      IB_RULE_LOG_FLAG_OPERATOR |                      \
      IB_RULE_LOG_FLAG_ACTION |                        \
      IB_RULE_LOG_FILT_ACTIONABLE )
#define IB_RULE_LOG_FLAGS_ALL                                \
    ( IB_RULE_LOG_FLAG_TX |                                  \
      IB_RULE_LOG_FLAG_REQ_LINE |                            \
      IB_RULE_LOG_FLAG_REQ_HEADER |                          \
      IB_RULE_LOG_FLAG_REQ_BODY |                            \
      IB_RULE_LOG_FLAG_RSP_LINE |                            \
      IB_RULE_LOG_FLAG_RSP_HEADER |                          \
      IB_RULE_LOG_FLAG_RSP_BODY |                            \
      IB_RULE_LOG_FLAG_PHASE |                               \
      IB_RULE_LOG_FLAG_RULE |                                \
      IB_RULE_LOG_FLAG_TARGET |                              \
      IB_RULE_LOG_FLAG_TFN |                                 \
      IB_RULE_LOG_FLAG_OPERATOR |                            \
      IB_RULE_LOG_FLAG_ACTION |                              \
      IB_RULE_LOG_FLAG_EVENT |                               \
      IB_RULE_LOG_FLAG_AUDIT )


/* Inspection Engine Options */
#define IB_IEOPT_REQUEST_HEADER           IB_TX_FINSPECT_REQHDR
#define IB_IEOPT_REQUEST_BODY             IB_TX_FINSPECT_REQBODY
#define IB_IEOPT_RESPONSE_HEADER          IB_TX_FINSPECT_RSPHDR
#define IB_IEOPT_RESPONSE_BODY            IB_TX_FINSPECT_RSPBODY

/* NOTE: Make sure to add new options from above to any groups below. */
#define IB_IEOPT_DEFAULT \
    ( IB_IEOPT_REQUEST_HEADER )
#define IB_IEOPT_ALL \
    ( IB_IEOPT_REQUEST_HEADER | \
      IB_IEOPT_REQUEST_BODY | \
      IB_IEOPT_RESPONSE_HEADER | \
      IB_IEOPT_RESPONSE_BODY )
#define IB_IEOPT_REQUEST \
    ( IB_IEOPT_REQUEST_HEADER | \
      IB_IEOPT_REQUEST_BODY )
#define IB_IEOPT_RESPONSE \
    ( IB_IEOPT_RESPONSE_HEADER | \
      IB_IEOPT_RESPONSE_BODY )


/* -- Utilities -- */

/**
 * Unescape a value using ib_util_unescape_string.
 *
 * It is guaranteed that @a dst will not be populated with a string
 * containing a premature EOL.
 *
 * @param[in,out] ib The ib->mp will be used to allocated @a *dst. Logging
 *                will also be done through this.
 * @param[out] dst The resultant unescaped string will be stored at @a *dst.
 * @param[in] src The source string to be escaped
 * @return IB_OK, IB_EALLOC on malloc failures, or IB_EINVAL or IB_ETRUNC on
 *         unescaping failures.
 */
static ib_status_t core_unescape(ib_engine_t *ib, char **dst, const char *src)
{
    size_t src_len = strlen(src);
    char *dst_tmp = ib_mpool_alloc(ib->mp, src_len+1);
    size_t dst_len;
    ib_status_t rc;

    if ( dst_tmp == NULL ) {
        ib_log_debug(ib, "Failed to allocate memory for unescaping.");
        return IB_EALLOC;
    }

    rc = ib_util_unescape_string(dst_tmp,
                                 &dst_len,
                                 src,
                                 src_len,
                                 ( IB_UTIL_UNESCAPE_NULTERMINATE |
                                   IB_UTIL_UNESCAPE_NONULL) );

    if (rc != IB_OK) {
        const char *msg = (rc == IB_EBADVAL) ?
            "Failed to unescape string \"%s\" because resultant unescaped "
                "string contains a NULL character." :
            "Failed to unescape string \"%s\"";
        ib_log_debug(ib, msg, src);
        return rc;
    }

    /* Success! */
    *dst = dst_tmp;

    return IB_OK;
}

/// @todo Make this public
static ib_status_t ib_auditlog_part_add(ib_auditlog_t *log,
                                        const char *name,
                                        const char *type,
                                        void *data,
                                        ib_auditlog_part_gen_fn_t generator,
                                        void *gen_data)
{
    ib_status_t rc;

    ib_auditlog_part_t *part =
        (ib_auditlog_part_t *)ib_mpool_alloc(log->mp, sizeof(*part));

    if (part == NULL) {
        return IB_EALLOC;
    }

    part->log = log;
    part->name = name;
    part->content_type = type;
    part->part_data = data;
    part->fn_gen = generator;
    part->gen_data = gen_data;

    rc = ib_list_push(log->parts, part);

    return rc;
}

static IB_PROVIDER_IFACE_TYPE(audit) core_audit_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,
    core_audit_open,
    core_audit_write_header,
    core_audit_write_part,
    core_audit_write_footer,
    core_audit_close
};

/* -- Logger API Implementations -- */

/**
 * Get the main core configuration
 *
 * @param[in] ib IronBee engine
 * @param[in] global Force global configuration
 *
 * @returns Main core configuration
 */
static ib_core_cfg_t *core_get_main_config(const ib_engine_t *ib,
                                           bool global)
{
    assert(ib != NULL);

    ib_core_cfg_t *config = NULL;
    ib_context_t  *main_ctx = NULL;

    if (global) {
        return &core_global_cfg;
    }

    /* Get the core context core configuration. */
    main_ctx = ib_context_main(ib);
    if (main_ctx != NULL) {
        ib_status_t rc;

        rc = ib_context_module_config(main_ctx,
                                      ib_core_module(),
                                      (void *)&config);

        /* When a module fails to find its context, use the global one. */
        if (rc != IB_OK) {
            config = &core_global_cfg;
        }
    }
    else {
        /* If there is no main context, use the global one. */
        config = &core_global_cfg;
    }
    return config;
}

/**
 * Open the configured log file
 *
 * @param[in] ib IronBee engine
 * @param[in,out] config Core configuration
 */
static void core_log_file_open(const ib_engine_t *ib,
                               ib_core_cfg_t *config)
{
    assert(ib != NULL);
    assert(config != NULL);

    /* Do we need to open the file? */
    if ( (config->log_fp == NULL) &&
         (config->log_uri != NULL) &&
         (*config->log_uri != '\0') )
    {
        /* If the URI looks like a file, try to open it. */
        if (strncmp(config->log_uri, "file://", 7) == 0) {
            const char *path = config->log_uri + 7;
            config->log_fp = fopen(path, "a");
            if (config->log_fp == NULL) {
                fprintf(stderr,
                        "Failed to open log file '%s' for writing: %s\n",
                        path, strerror(errno));
            }
        }
        else {
            fprintf(stderr, "Only file:// log URIs current supported.");
        }
    }

    /* Finally, use stderr as a fallback. */
    if (config->log_fp == NULL) {
        config->log_fp = ib_util_fdup(stderr, "a");
        if (config->log_fp == NULL) {
            config->log_fp = stderr;       /* Last resort */
        }
        config->log_uri = "stderr";
    }
}

/**
 * Open the configuration's log file
 *
 * @param[in] ib IronBee engine
 * @param[in,out] config Core configuration
 */
static void core_log_file_close(const ib_engine_t *ib,
                                ib_core_cfg_t *config)
{
    assert(ib != NULL);
    assert(config != NULL);

    if ( (config->log_fp != NULL) && (config->log_fp != stderr) ) {
        fclose(config->log_fp);
        config->log_fp = stderr;
    }
}

/**
 * Core to log data via va_list args to file pointer.
 *
 * @param ib IronBee engine
 * @param log_fp File pointer to log to
 * @param log_level Configured log level
 * @param level Message log level
 * @param file Source code filename (typically __FILE__)
 * @param line Source code line number (typically __LINE__)
 * @param fmt Printf like format string
 * @param ap Variable length parameter list
 *
 * @returns Status code
 */
static void core_vlogmsg_fp(
    const ib_engine_t   *ib,
    FILE                *log_fp,
    ib_log_level_t       log_level,
    ib_log_level_t       level,
    const char          *file,
    int                  line,
    const char          *fmt,
    va_list              ap)
{
    static const size_t c_line_info_length = 35;
    char line_info[c_line_info_length];
    size_t new_fmt_length = 0;
    char *new_fmt = NULL;
    const char *which_fmt;
    char time_info[30];

    ib_clock_timestamp(time_info, NULL);

    line_info[0] = '\0';
    if ( (file != NULL) && (line > 0) && (log_level >= IB_LOG_DEBUG)) {
        size_t flen;
        while (strncmp(file, "../", 3) == 0) {
            file += 3;
        }
        flen = strlen(file);
        if (flen > 23) {
            file += (flen - 23);
        }

        snprintf(line_info, c_line_info_length, "(%23s:%-5d)", file, line);
    }

    new_fmt_length = (strlen(line_info) +
                      strlen(time_info) +
                      strlen(fmt) +
                      110);
    new_fmt = (char *)malloc(new_fmt_length);
    if (new_fmt == NULL) {
        which_fmt = fmt;
    }
    else {
        snprintf(new_fmt, new_fmt_length,
                 "%s %-10s- %s [%d] %s\n",
                 time_info, ib_log_level_to_string(level),
                 line_info, getpid(), fmt);
        which_fmt = new_fmt;
    }

    vfprintf(log_fp, which_fmt, ap);
    fflush(log_fp);

    if (new_fmt != NULL) {
        free(new_fmt);
    }
}

/**
 * Core implementation to log data via va_list args.
 *
 * @param ib IronBee engine
 * @param level Log level
 * @param file Source code filename (typically __FILE__)
 * @param line Source code line number (typically __LINE__)
 * @param fmt Printf like format string
 * @param ap Variable length parameter list
 * @param cbdata Callback data.
 *
 * @returns Status code
 */
static void core_vlogmsg(
    const ib_engine_t *ib,
    ib_log_level_t     level,
    const char        *file,
    int                line,
    const char        *fmt,
    va_list            ap,
    void              *cbdata)
{
    ib_log_level_t log_level;
    ib_core_cfg_t *config;

    config = core_get_main_config(ib, false);
    if (config->log_fp == NULL) {
        core_log_file_open(ib, config);
    }
    log_level = ib_log_get_level(ib);

    core_vlogmsg_fp(ib, config->log_fp, log_level, level, file, line, fmt, ap);
}

/**
 * Fetch the log level.
 *
 * @param[in] ib     IronBee engine.
 * @param[in] cbdata Callback data; ignored.
 * @returns Log level.
 */
static ib_log_level_t core_loglevel(
    const ib_engine_t *ib,
    void              *cbdata)
{
    ib_core_cfg_t *config = NULL;

    config = core_get_main_config(ib, false);

    return config->log_level;
}

/* -- Audit API Implementations -- */

/**
 * Write an audit log.
 *
 * @param lpi Audit provider
 *
 * @returns Status code
 */
static ib_status_t audit_api_write_log(ib_provider_inst_t *lpi)
{
    IB_PROVIDER_IFACE_TYPE(audit) *iface =
        (IB_PROVIDER_IFACE_TYPE(audit) *)lpi->pr->iface;
    ib_auditlog_t *log = (ib_auditlog_t *)lpi->data;
    ib_list_node_t *node;
    ib_status_t rc;

    if (ib_list_elements(log->parts) == 0) {
        ib_log_error(lpi->pr->ib,  "No parts to write to audit log");
        return IB_EINVAL;
    }

    /* Open the log if required. This is thread safe. */
    if (iface->open != NULL) {
        rc = iface->open(lpi, log);
        if (rc != IB_OK) {
            if (log->ctx->auditlog->index != NULL) {
                ib_lock_unlock(&log->ctx->auditlog->index_fp_lock);
            }
            return rc;
        }
    }

    /* Lock to write. */
    if (log->ctx->auditlog->index != NULL) {
        rc = ib_lock_lock(&log->ctx->auditlog->index_fp_lock);
        if (rc != IB_OK) {
            ib_log_error(lpi->pr->ib, "Cannot lock %s for write.",
                         log->ctx->auditlog->index);
            return rc;
        }
    }

    /* Write the header if required. */
    if (iface->write_header != NULL) {
        rc = iface->write_header(lpi, log);
        if (rc != IB_OK) {
            ib_lock_unlock(&log->ctx->auditlog->index_fp_lock);
            return rc;
        }
    }

    /* Write the parts. */
    IB_LIST_LOOP(log->parts, node) {
        ib_auditlog_part_t *part =
            (ib_auditlog_part_t *)ib_list_node_data(node);
        rc = iface->write_part(lpi, part);
        if (rc != IB_OK) {
            ib_log_error(log->ib,  "Failed to write audit log part: %s",
                         part->name);
        }
    }

    /* Write the footer if required. */
    if (iface->write_footer != NULL) {
        rc = iface->write_footer(lpi, log);
        if (rc != IB_OK) {
            if (log->ctx->auditlog->index != NULL) {
                ib_lock_unlock(&log->ctx->auditlog->index_fp_lock);
            }
            return rc;
        }
    }

    /* Writing is done. Unlock. Close is thread-safe. */
    if (log->ctx->auditlog->index != NULL) {
        ib_lock_unlock(&log->ctx->auditlog->index_fp_lock);
    }

    /* Close the log if required. */
    if (iface->close != NULL) {
        rc = iface->close(lpi, log);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Audit provider registration function.
 *
 * This just does a version and sanity check on a registered provider.
 *
 * @param ib Engine
 * @param lpr Audit provider
 *
 * @returns Status code
 */
static ib_status_t audit_register(ib_engine_t *ib,
                                  ib_provider_t *lpr)
{
    IB_PROVIDER_IFACE_TYPE(audit) *iface =
        (IB_PROVIDER_IFACE_TYPE(audit) *)lpr->iface;

    /* Check that versions match. */
    if (iface->version != IB_PROVIDER_VERSION_AUDIT) {
        return IB_EINCOMPAT;
    }

    /* Verify that required interface functions are implemented. */
    if (iface->write_part == NULL) {
        ib_log_alert(ib, "The write_part function "
                     "MUST be implemented by a audit provider");
        return IB_EINVAL;
    }

    return IB_OK;
}

/**
 * Audit provider API mapping for core module.
 */
static IB_PROVIDER_API_TYPE(audit) audit_api = {
    audit_api_write_log
};

static size_t ib_auditlog_gen_raw_stream(ib_auditlog_part_t *part,
                                         const uint8_t **chunk)
{
    ib_sdata_t *sdata;
    size_t dlen;

    if (part->gen_data == NULL) {
        ib_stream_t *stream = (ib_stream_t *)part->part_data;

        /* No data. */
        if (stream->slen == 0) {
            *chunk = NULL;
            part->gen_data = (void *)-1;
            return 0;
        }

        sdata = (ib_sdata_t *)IB_LIST_FIRST(stream);
        dlen = sdata->dlen;
        *chunk = (const uint8_t *)sdata->data;

        sdata = IB_LIST_NODE_NEXT(sdata);
        if (sdata != NULL) {
            part->gen_data = sdata;
        }
        else {
            part->gen_data = (void *)-1;
        }

        return dlen;
    }
    else if (part->gen_data == (void *)-1) {
        part->gen_data = NULL;
        return 0;
    }

    sdata = (ib_sdata_t *)part->gen_data;
    dlen = sdata->dlen;
    *chunk = (const uint8_t *)sdata->data;

    sdata = IB_LIST_NODE_NEXT(sdata);
    if (sdata != NULL) {
        part->gen_data = sdata;
    }
    else {
        part->gen_data = (void *)-1;
    }

    return dlen;
}

static size_t ib_auditlog_gen_json_flist(ib_auditlog_part_t *part,
                                         const uint8_t **chunk)
{
    ib_engine_t *ib = part->log->ib;
    ib_field_t *f;
    uint8_t *rec;
    ib_status_t rc;

#define CORE_JSON_MAX_FIELD_LEN 256

    /* The gen_data field is used to store the current state. NULL
     * means the part has not started yet and a -1 value
     * means it is done. Anything else is a node in the event list.
     */
    if (part->gen_data == NULL) {
        ib_list_t *list = (ib_list_t *)part->part_data;

        /* No data. */
        if (ib_list_elements(list) == 0) {
            ib_log_info(ib, "No data in audit log part: %s", part->name);
            *chunk = (const uint8_t *)"{}";
            part->gen_data = (void *)-1;
            return strlen(*(const char **)chunk);
        }

        *chunk = (const uint8_t *)"{\r\n";
        part->gen_data = ib_list_first(list);
        return strlen(*(const char **)chunk);
    }
    else if (part->gen_data == (void *)-1) {
        part->gen_data = NULL;
        return 0;
    }

    f = (ib_field_t *)ib_list_node_data((ib_list_node_t *)part->gen_data);
    if (f != NULL) {
        const char *comma;
        size_t rlen;

        rec = (uint8_t *)ib_mpool_alloc(part->log->mp, CORE_JSON_MAX_FIELD_LEN);

        /* Error. */
        if (rec == NULL) {
            *chunk = (const uint8_t *)"}";
            return strlen(*(const char **)chunk);
        }

        /* Next is used to determine if there is a trailing comma. */
        comma = ib_list_node_next((ib_list_node_t *)part->gen_data) ? "," : "";

        /// @todo Quote values
        switch(f->type) {
        case IB_FTYPE_NULSTR:
        {
            const char *ns;
            rc = ib_field_value(f, ib_ftype_nulstr_out(&ns));
            if (rc != IB_OK) {
                return 0;
            }

            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": \"%s\"%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            (ns?ns:""),
                            comma);
            break;
        }
        case IB_FTYPE_BYTESTR:
        {
            const ib_bytestr_t *bs;
            rc = ib_field_value(f, ib_ftype_bytestr_out(&bs));
            if (rc != IB_OK) {
                return 0;
            }

            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": "
                            "\"%" IB_BYTESTR_FMT "\"%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            IB_BYTESTR_FMT_PARAM(bs),
                            comma);
            break;
        }
        case IB_FTYPE_NUM:
        {
            ib_num_t n;
            rc = ib_field_value(f, ib_ftype_num_out(&n));
            if (rc != IB_OK) {
                return 0;
            }

            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": "
                            "%" PRId64 "%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            n,
                            comma);
            break;
        }
        case IB_FTYPE_LIST:
        /* Iterate over field values, adding the NULSTR values to the json list. */
        {
            const ib_list_t* flist;
            ib_list_t *list;
            const ib_list_node_t *node;
            char list_data[128] = "";

            rc = ib_list_create(&list, part->log->mp);
            if (rc != IB_OK) {
                goto listerror;
            }

            rc = ib_field_value(f, ib_ftype_list_out(&flist));
            if (rc != IB_OK) {
                goto listerror;
            }

            IB_LIST_LOOP_CONST(flist, node) {
                const char *val = NULL;
                const ib_field_t *field =
                    (const ib_field_t *)ib_list_node_data_const(node);

                /* NOTE: This currently only works for NULSTR fields. */
                if ((field == NULL) || (field->type != IB_FTYPE_NULSTR)) {
                    goto listerror;
                }

                rc = ib_field_value(field, ib_ftype_nulstr_out(&val));
                if (rc != IB_OK) {
                    goto listerror;
                }

                ib_list_push(list, (void *)val);
            }

            rc = ib_strlist_escape_json_buf(list, true, ", ",
                                            list_data, sizeof(list_data),
                                            NULL, NULL);
            if (rc != IB_OK) {
                goto listerror;
            }

            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": [%s]%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            list_data, comma);
            break;

listerror:
            ib_log_notice(part->log->ib, "Failed to generate JSON list.");
            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": []%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            comma);
            break;
        }
        default:
            rlen = snprintf((char *)rec, CORE_JSON_MAX_FIELD_LEN,
                            "  \"%" IB_BYTESTR_FMT "\": \"-\"%s\r\n",
                            IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                            comma);
            break;
        }

        /* Verify size. */
        if (rlen >= CORE_JSON_MAX_FIELD_LEN) {
            ib_log_notice(ib, "Item too large to log in part %s: %zd",
                          part->name, rlen);
            *chunk = (const uint8_t *)"\r\n";
            part->gen_data = (void *)-1;
            return strlen(*(const char **)chunk);
        }

        *chunk = rec;
    }
    else {
        ib_log_error(ib, "NULL field in part: %s", part->name);
        *chunk = (const uint8_t *)"\r\n";
        part->gen_data = (void *)-1;
        return strlen(*(const char **)chunk);
    }
    part->gen_data = ib_list_node_next((ib_list_node_t *)part->gen_data);

    /* Close the json structure. */
    if (part->gen_data == NULL) {
        size_t clen = strlen(*(const char **)chunk);
        (*(uint8_t **)chunk)[clen] = '}';
        part->gen_data = (void *)-1;
        return clen + 1;
    }

    return strlen(*(const char **)chunk);
}

static size_t ib_auditlog_gen_header_flist(ib_auditlog_part_t *part,
                                           const uint8_t **chunk)
{
    ib_engine_t *ib = part->log->ib;
    ib_field_t *f;
    uint8_t *rec;
    size_t rlen;
    ib_status_t rc;

#define CORE_HEADER_MAX_FIELD_LEN 8192

    /* The gen_data field is used to store the current state. NULL
     * means the part has not started yet and a -1 value
     * means it is done. Anything else is a node in the event list.
     */
    if (part->gen_data == NULL) {
        ib_list_t *list = (ib_list_t *)part->part_data;

        /* No data. */
        if (ib_list_elements(list) == 0) {
            ib_log_info(ib, "No data in audit log part: %s", part->name);
            part->gen_data = NULL;
            return 0;
        }

        /* First should be a request/response line. */
        part->gen_data = ib_list_first(list);
        f = (ib_field_t *)ib_list_node_data((ib_list_node_t *)part->gen_data);
        if ((f != NULL) && (f->type == IB_FTYPE_BYTESTR)) {
            const ib_bytestr_t *bs;
            rec = (uint8_t *)ib_mpool_alloc(part->log->mp,
                                            CORE_HEADER_MAX_FIELD_LEN);

            rc = ib_field_value(f, ib_ftype_bytestr_out(&bs));
            if (rc != IB_OK) {
                return 0;
            }

            rlen = snprintf((char *)rec, CORE_HEADER_MAX_FIELD_LEN,
                            "%" IB_BYTESTR_FMT "\r\n",
                            IB_BYTESTR_FMT_PARAM(bs));

            /* Verify size. */
            if (rlen >= CORE_HEADER_MAX_FIELD_LEN) {
                ib_log_notice(ib, "Item too large to log in part %s: %zd",
                              part->name, rlen);
                *chunk = (const uint8_t *)"\r\n";
                part->gen_data = (void *)-1;
                return strlen(*(const char **)chunk);
            }

            *chunk = rec;

            part->gen_data =
                ib_list_node_next((ib_list_node_t *)part->gen_data);
            if (part->gen_data == NULL) {
                part->gen_data = (void *)-1;
            }

            return strlen(*(const char **)chunk);
        }
    }
    else if (part->gen_data == (void *)-1) {
        part->gen_data = NULL;
        *chunk = (const uint8_t *)"";
        return 0;
    }

    /* Header Lines */
    f = (ib_field_t *)ib_list_node_data((ib_list_node_t *)part->gen_data);
    if (f == NULL) {
        ib_log_error(ib, "NULL field in part: %s", part->name);
        *chunk = (const uint8_t *)"\r\n";
        part->gen_data = (void *)-1;
        return strlen(*(const char **)chunk);
    }

    rec = (uint8_t *)ib_mpool_alloc(part->log->mp, CORE_HEADER_MAX_FIELD_LEN);
    if (rec == NULL) {
        *chunk = NULL;
        return 0;
    }

    /// @todo Quote values
    switch(f->type) {
    case IB_FTYPE_NULSTR:
    {
        const char *s;
        rc = ib_field_value(f, ib_ftype_nulstr_out(&s));
        if (rc != IB_OK) {
            return 0;
        }

        rlen = snprintf((char *)rec, CORE_HEADER_MAX_FIELD_LEN,
                        "%" IB_BYTESTR_FMT ": %s\r\n",
                        IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                        s);
        break;
    }
    case IB_FTYPE_BYTESTR:
    {
        const ib_bytestr_t *bs;
        rc = ib_field_value(f, ib_ftype_bytestr_out(&bs));
        if (rc != IB_OK) {
            return 0;
        }
        rlen = snprintf((char *)rec, CORE_HEADER_MAX_FIELD_LEN,
                        "%" IB_BYTESTR_FMT ": "
                        "%" IB_BYTESTR_FMT "\r\n",
                        IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                        IB_BYTESTR_FMT_PARAM(bs));
        break;
    }
    default:
        rlen = snprintf((char *)rec, CORE_HEADER_MAX_FIELD_LEN,
                        "%" IB_BYTESTR_FMT ": IronBeeError - unhandled header type %d\r\n",
                        IB_BYTESTRSL_FMT_PARAM(f->name, f->nlen),
                        f->type);
        break;
    }

    /* Verify size. */
    if (rlen >= CORE_HEADER_MAX_FIELD_LEN) {
        ib_log_error(ib, "Item too large to log in part %s: %zd",
                     part->name, rlen);
        *chunk = (const uint8_t *)"\r\n";
        part->gen_data = (void *)-1;
        return strlen(*(const char **)chunk);
    }

    *chunk = rec;

    /* Stage the next chunk of data (header). */
    part->gen_data = ib_list_node_next((ib_list_node_t *)part->gen_data);

    /* Close the structure if there is no more data. */
    if (part->gen_data == NULL) {
        part->gen_data = (void *)-1;
    }

    return strlen(*(const char **)chunk);
}

static size_t ib_auditlog_gen_json_events(ib_auditlog_part_t *part,
                                          const uint8_t **chunk)
{
    ib_engine_t *ib = part->log->ib;
    ib_list_t *list = (ib_list_t *)part->part_data;
    void *list_first;
    ib_logevent_t *e;
    uint8_t *rec;

#define CORE_JSON_MAX_REC_LEN 1024

    /* The gen_data field is used to store the current state. NULL
     * means the part has not started yet and a -1 value
     * means it is done. Anything else is a node in the event list.
     */
    if (part->gen_data == NULL) {
        /* No events. */
        if (ib_list_elements(list) == 0) {
            ib_log_error(ib, "No events in audit log");
            *chunk = (const uint8_t *)"{}";
            part->gen_data = (void *)-1;
            return strlen(*(const char **)chunk);
        }

        *chunk = (const uint8_t *)"{\r\n  \"events\": [\r\n";
        part->gen_data = ib_list_first(list);
        return strlen(*(const char **)chunk);
    }
    else if (part->gen_data == (void *)-1) {
        part->gen_data = NULL;
        return 0;
    }

    /* Used to detect the first event. */
    list_first = ib_list_first(list);

    e = (ib_logevent_t *)ib_list_node_data((ib_list_node_t *)part->gen_data);
    if (e != NULL) {
        size_t rlen;

        /* Turn tag list into JSON list, limiting the size. */
        char tags[128] = "\0";
        char fields[128] = "\0";
        char ruleid[128] = "\0";
        const char *logdata = "";
        ib_status_t rc;

        rc = ib_strlist_escape_json_buf(e->tags, true, ", ",
                                        tags, sizeof(tags),
                                        NULL, NULL);
        if (rc != IB_OK) {
            ib_log_error_tx(part->log->tx,
                            "Failed to escape tags for audit log: %s",
                            ib_status_to_string(rc));
        }

        rec = (uint8_t *)ib_mpool_alloc(part->log->mp, CORE_JSON_MAX_REC_LEN);

        /* Error. */
        if (rec == NULL) {
            *chunk = (const uint8_t *)"  ]\r\n}";
            return strlen(*(const char **)chunk);
        }

        if (e->fields != NULL) {
            const ib_list_node_t *field_node;
            field_node = ib_list_first_const(e->fields);
            if (field_node != NULL) {
                const char *field_name = (const char *)field_node->data;
                ib_flags_t rslt;

                rc = ib_string_escape_json_buf(field_name, true,
                                               fields, sizeof(fields), NULL,
                                               &rslt);
                if (rc != IB_OK) {
                    ib_log_error_tx(part->log->tx,
                                    "Failed to escape field name \"%s\": %s",
                                    field_name, ib_status_to_string(rc));
                    *fields = '\0';
                }
            }
        }

        if (e->data != NULL) {
            char *escaped;
            ib_flags_t rslt;

            /* Note: Log data is expanded in act_event_execute() */
            rc = ib_string_escape_json_ex(part->log->mp,
                                          e->data, e->data_len,
                                          true, false,
                                          &escaped, NULL,
                                          &rslt);
            if (rc != IB_OK) {
                ib_log_error_tx(part->log->tx,
                                "Failed to escape log data \"%.*s\": %s",
                                (int)e->data_len, (const char *)e->data,
                                ib_status_to_string(rc));
                escaped = (char *)"";
            }
            logdata = escaped;
        }

        if (e->rule_id != NULL) {
            ib_flags_t rslt;

            rc = ib_string_escape_json_buf(e->rule_id, true,
                                           ruleid, sizeof(ruleid), NULL,
                                           &rslt);
            if (rc != IB_OK) {
                ib_log_error_tx(part->log->tx,
                                "Failed to escape rule ID \"%s\": %s",
                                e->rule_id, ib_status_to_string(rc));
                *ruleid = '\0';
            }
        }

        rlen = snprintf((char *)rec, CORE_JSON_MAX_REC_LEN,
                        "%s"
                        "    {\r\n"
                        "      \"event-id\": %" PRIu32 ",\r\n"
                        "      \"rule-id\": %s,\r\n"
                        "      \"type\": \"%s\",\r\n"
                        "      \"suppress\": \"%s\",\r\n"
                        "      \"rec-action\": \"%s\",\r\n"
                        "      \"confidence\": %u,\r\n"
                        "      \"severity\": %u,\r\n"
                        "      \"tags\": [%s],\r\n"
                        "      \"fields\": [%s],\r\n"
                        "      \"msg\": \"%s\",\r\n"
                        "      \"data\": \"%s\"\r\n"
                        "    }",
                        (list_first == part->gen_data ? "" : ",\r\n"),
                        e->event_id,
                        ruleid,
                        ib_logevent_type_name(e->type),
                        ib_logevent_suppress_name(e->suppress),
                        ib_logevent_action_name(e->rec_action),
                        e->confidence,
                        e->severity,
                        tags,
                        fields,
                        e->msg ? e->msg : "-",
                        logdata);

        /* Verify size. */
        if (rlen >= CORE_JSON_MAX_REC_LEN) {
            ib_log_error(ib, "Event too large to log: %zd", rlen);
            *chunk = (const uint8_t *)"    {}";
            part->gen_data = (void *)-1;
            return strlen(*(const char **)chunk);
        }

        *chunk = rec;
    }
    else {
        ib_log_error(ib, "NULL event");
        *chunk = (const uint8_t *)"    {}";
        part->gen_data = (void *)-1;
        return strlen(*(const char **)chunk);
    }
    part->gen_data = ib_list_node_next((ib_list_node_t *)part->gen_data);

    /* Close the json structure. */
    if (part->gen_data == NULL) {
        size_t clen = strlen(*(const char **)chunk);

        part->gen_data = (void *)-1;

        if (clen+8 > CORE_JSON_MAX_REC_LEN) {
            if (clen+2 > CORE_JSON_MAX_REC_LEN) {
                ib_log_error(ib, "Event too large to fit in buffer");
                *chunk = (const uint8_t *)"    {}\r\n  ]\r\n}";
            }
            memcpy(*(uint8_t **)chunk + clen, "]}", 2);
            return clen + 2;
        }
        memcpy(*(uint8_t **)chunk + clen, "\r\n  ]\r\n}", 8);
        return clen + 8;
    }

    return strlen(*(const char **)chunk);
}

#define CORE_AUDITLOG_FORMAT "http-message/1"

static ib_status_t ib_auditlog_add_part_header(ib_auditlog_t *log)
{
    assert(log != NULL);
    assert(log->tx != NULL);

    core_audit_cfg_t *cfg = (core_audit_cfg_t *)log->cfg_data;
    ib_engine_t *ib = log->ib;
    ib_tx_t *tx = log->tx;
    ib_num_t tx_num = tx->conn->tx_count;
    const ib_site_t *site;
    ib_mpool_t *pool = log->mp;
    ib_field_t *f;
    ib_list_t *list;
    ib_num_t tx_time = 0;
    char *tstamp;
    char *log_format;
    ib_status_t rc;

    /* Timestamp */
    tstamp = (char *)ib_mpool_alloc(pool, 30);
    if (tstamp == NULL) {
        return IB_EALLOC;
    }
    ib_clock_relative_timestamp(tstamp, &log->tx->tv_created,
                                (log->tx->t.logtime - log->tx->t.started));

    /*
     * Transaction time depends on where processing stopped.
     */
    if (tx->t.response_finished > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.response_finished - tx->t.request_started);
    }
    else if (tx->t.response_body > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.response_body - tx->t.request_started);
    }
    else if (tx->t.response_header > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.response_header - tx->t.request_started);
    }
    else if (tx->t.response_started > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.response_started - tx->t.request_started);
    }
    else if (tx->t.request_finished > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.request_finished - tx->t.request_started);
    }
    else if (tx->t.request_body > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.request_body - tx->t.request_started);
    }
    else if (tx->t.request_header > tx->t.request_started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.request_header - tx->t.request_started);
    }
    else if (tx->t.request_started > tx->t.started) {
        tx_time = IB_CLOCK_USEC_TO_MSEC(tx->t.request_started - tx->t.started);
    }


    /* Log Format */
    log_format = ib_mpool_strdup(pool, CORE_AUDITLOG_FORMAT);
    if (log_format == NULL) {
        return IB_EALLOC;
    }

    /* Generate a list of fields in this part. */
    rc = ib_list_create(&list, pool);
    if (rc != IB_OK) {
        return rc;
    }

    ib_field_create(&f, pool,
                    IB_FIELD_NAME("tx-num"),
                    IB_FTYPE_NUM,
                    ib_ftype_num_in(&tx_num));
    ib_list_push(list, f);

    ib_field_create(&f, pool,
                    IB_FIELD_NAME("tx-time"),
                    IB_FTYPE_NUM,
                    ib_ftype_num_in(&tx_time));
    ib_list_push(list, f);

    if (tx != NULL) {
        ib_list_t *events;

        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("tx-id"),
                                      (uint8_t *)tx->id,
                                      strlen(tx->id));
        ib_list_push(list, f);

        /* Add all unsuppressed alert event tags as well
         * as the last alert message and action. */
        rc = ib_logevent_get_all(tx, &events);
        if (rc == IB_OK) {
            ib_list_node_t *enode;
            ib_field_t *tx_action;
            ib_field_t *tx_msg;
            ib_field_t *tx_tags;
            ib_field_t *tx_threat_level;
            ib_num_t threat_level = 0;
            bool do_threat_calc = true;
            int num_events = 0;

            ib_field_create(&tx_action, pool,
                            IB_FIELD_NAME("tx-action"),
                            IB_FTYPE_NULSTR,
                            NULL);

            ib_field_create(&tx_msg, pool,
                            IB_FIELD_NAME("tx-msg"),
                            IB_FTYPE_NULSTR,
                            NULL);

            ib_field_create(&tx_threat_level, pool,
                            IB_FIELD_NAME("tx-threatlevel"),
                            IB_FTYPE_NUM,
                            NULL);

            ib_field_create(&tx_tags, pool,
                            IB_FIELD_NAME("tx-tags"),
                            IB_FTYPE_LIST,
                            NULL);

            /* Determine transaction action (block/log) via flags. */
            if (ib_tx_flags_isset(tx, IB_TX_BLOCK_PHASE|IB_TX_BLOCK_IMMEDIATE)) {
                ib_field_setv(tx_action, ib_ftype_nulstr_in(
                    ib_logevent_action_name(IB_LEVENT_ACTION_BLOCK))
                );
            }
            else {
                ib_field_setv(tx_action, ib_ftype_nulstr_in(
                    ib_logevent_action_name(IB_LEVENT_ACTION_LOG))
                );
            }

            /* Check if THREAT_LEVEL is available, or if we need to calculate
             * it here.
             */
            rc = ib_data_get_ex(tx->data, IB_S2SL("THREAT_LEVEL"), &f);
            if ((rc == IB_OK) && (f->type == IB_FTYPE_NUM)) {
                rc = ib_field_value(f, ib_ftype_num_out(&threat_level));
                if (rc == IB_OK) {
                    ib_log_debug_tx(tx, "Using THREAT_LEVEL as threat level value.");
                    do_threat_calc = false;
                }
                else {
                    ib_log_debug_tx(tx, "No numeric THREAT_LEVEL to use as threat level value.");
                }
            }
            else {
                ib_log_debug_tx(tx, "No THREAT_LEVEL to use as threat level value.");
            }

            /* It is more important to write out what is possible
             * than to fail here. So, some error codes are ignored.
             *
             * TODO: Simplify buy not using collections
             */
            IB_LIST_LOOP(events, enode) {
                ib_logevent_t *e = (ib_logevent_t *)ib_list_node_data(enode);
                ib_list_node_t *tnode;

                /* Only unsuppressed. */
                if (   (e == NULL)
                    || (e->suppress != IB_LEVENT_SUPPRESS_NONE))
                {
                    continue;
                }

                if (do_threat_calc) {
                    /* The threat_level is average severity. */
                    if (e->severity > 0) {
                        threat_level += e->severity;
                        ++num_events;
                    }
                }

                /* Only alerts. */
                if (e->type != IB_LEVENT_TYPE_ALERT) {
                    continue;
                }

                ib_field_setv(tx_msg, ib_ftype_nulstr_in(e->msg));

                IB_LIST_LOOP(e->tags, tnode) {
                    char *tag = (char *)ib_list_node_data(tnode);

                    if (tag != NULL) {
                        ib_field_create(&f, pool,
                                        IB_FIELD_NAME("tag"),
                                        IB_FTYPE_NULSTR,
                                        ib_ftype_nulstr_in(tag));
                        ib_field_list_add(tx_tags, f);
                    }
                }
            }

            /* Use the average threat level. */
            if ((do_threat_calc) && (num_events > 0)) {
                threat_level /= num_events;
            }
            ib_field_setv(tx_threat_level, ib_ftype_num_in(&threat_level));

            ib_list_push(list, tx_action);
            ib_list_push(list, tx_msg);
            ib_list_push(list, tx_tags);
            ib_list_push(list, tx_threat_level);
        }
    }

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("log-timestamp"),
                                  (uint8_t *)tstamp,
                                  strlen(tstamp));
    ib_list_push(list, f);

    /* TODO: This probably will be removed in the near future. */
    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("log-format"),
                                  (uint8_t *)log_format,
                                  strlen(log_format));
    ib_list_push(list, f);

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("log-id"),
                                  (uint8_t *)cfg->boundary,
                                  strlen(cfg->boundary));
    ib_list_push(list, f);

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("sensor-id"),
                                  (uint8_t *)ib->sensor_id_str,
                                  strlen(ib->sensor_id_str));
    ib_list_push(list, f);

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("sensor-name"),
                                  (uint8_t *)ib->sensor_name,
                                  strlen(ib->sensor_name));
    ib_list_push(list, f);

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("sensor-version"),
                                  (uint8_t *)ib->sensor_version,
                                  strlen(ib->sensor_version));
    ib_list_push(list, f);

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("sensor-hostname"),
                                  (uint8_t *)ib->sensor_hostname,
                                  strlen(ib->sensor_hostname));
    ib_list_push(list, f);

    rc = ib_context_site_get(log->ctx, &site);
    if (rc != IB_OK) {
        return rc;
    }
    if (site != NULL) {
        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("site-id"),
                                      (uint8_t *)site->id_str,
                                      strlen(site->id_str));
        ib_list_push(list, f);

        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("site-name"),
                                      (uint8_t *)site->name,
                                      strlen(site->name));
        ib_list_push(list, f);
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "header",
                              "application/json",
                              list,
                              ib_auditlog_gen_json_flist,
                              NULL);

    return rc;
}

static ib_status_t ib_auditlog_add_part_events(ib_auditlog_t *log)
{
    ib_list_t *list;
    ib_status_t rc;

    /* Get the list of events. */
    rc = ib_logevent_get_all(log->tx, &list);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "events",
                              "application/json",
                              list,
                              ib_auditlog_gen_json_events,
                              NULL);

    return rc;
}

static ib_status_t ib_auditlog_add_part_http_request_meta(ib_auditlog_t *log)
{
    ib_tx_t *tx = log->tx;
    ib_mpool_t *pool = log->mp;
    ib_field_t *f;
    ib_list_t *list;
    char *tstamp;
    ib_status_t rc;

    /* Generate a list of fields in this part. */
    rc = ib_list_create(&list, pool);
    if (rc != IB_OK) {
        return rc;
    }

    if (tx != NULL) {
        ib_num_t num;

        /* Timestamp */
        tstamp = (char *)ib_mpool_alloc(pool, 30);
        if (tstamp == NULL) {
            return IB_EALLOC;
        }
        ib_clock_relative_timestamp(tstamp, &tx->tv_created,
                                    (tx->t.request_started - tx->t.started));

        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("request-timestamp"),
                                      (uint8_t *)tstamp,
                                      strlen(tstamp));
        ib_list_push(list, f);

        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("remote-addr"),
                                      (uint8_t *)tx->er_ipstr,
                                      strlen(tx->er_ipstr));
        ib_list_push(list, f);

        num = tx->conn->remote_port;
        ib_field_create(&f, pool,
                        IB_FIELD_NAME("remote-port"),
                        IB_FTYPE_NUM,
                        ib_ftype_num_in(&num));
        ib_list_push(list, f);

        ib_field_create_bytestr_alias(&f, pool,
                                      IB_FIELD_NAME("local-addr"),
                                      (uint8_t *)tx->conn->local_ipstr,
                                      strlen(tx->conn->local_ipstr));
        ib_list_push(list, f);

        num = tx->conn->local_port;
        ib_field_create(&f, pool,
                        IB_FIELD_NAME("local-port"),
                        IB_FTYPE_NUM,
                        ib_ftype_num_in(&num));
        ib_list_push(list, f);

        /// @todo If this is NULL, parser failed - what to do???
        if (tx->path != NULL) {
            ib_field_create_bytestr_alias(&f, pool,
                                          IB_FIELD_NAME("request-uri-path"),
                                          (uint8_t *)tx->path,
                                          strlen(tx->path));
            ib_list_push(list, f);
        }

        rc = ib_data_get_ex(tx->data, IB_S2SL("request_protocol"), &f);
        if (rc == IB_OK) {
            ib_list_push(list, f);
        }
        else {
            ib_log_error_tx(tx, "Failed to get request_protocol: %s",
                            ib_status_to_string(rc));
        }

        rc = ib_data_get_ex(tx->data, IB_S2SL("request_method"), &f);
        if (rc == IB_OK) {
            ib_list_push(list, f);
        }
        else {
            ib_log_error_tx(tx, "Failed to get request_method: %s",
                            ib_status_to_string(rc));
        }

        /// @todo If this is NULL, parser failed - what to do???
        if (tx->hostname != NULL) {
            ib_field_create_bytestr_alias(&f, pool,
                                          IB_FIELD_NAME("request-hostname"),
                                          (uint8_t *)tx->hostname,
                                          strlen(tx->hostname));
            ib_list_push(list, f);
        }
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "http-request-metadata",
                              "application/json",
                              list,
                              ib_auditlog_gen_json_flist,
                              NULL);

    return rc;
}

static ib_status_t ib_auditlog_add_part_http_response_meta(ib_auditlog_t *log)
{
    ib_tx_t *tx = log->tx;
    ib_mpool_t *pool = log->mp;
    ib_field_t *f;
    ib_list_t *list;
    char *tstamp;
    ib_status_t rc;

    /* Timestamp */
    tstamp = (char *)ib_mpool_alloc(pool, 30);
    if (tstamp == NULL) {
        return IB_EALLOC;
    }
    ib_clock_relative_timestamp(tstamp, &tx->tv_created,
                                (tx->t.response_started - tx->t.started));

    /* Generate a list of fields in this part. */
    rc = ib_list_create(&list, pool);
    if (rc != IB_OK) {
        return rc;
    }

    ib_field_create_bytestr_alias(&f, pool,
                                  IB_FIELD_NAME("response-timestamp"),
                                  (uint8_t *)tstamp,
                                  strlen(tstamp));
    ib_list_push(list, f);

    rc = ib_data_get_ex(tx->data, IB_S2SL("response_status"), &f);
    if (rc == IB_OK) {
        ib_list_push(list, f);
    }
    else {
        ib_log_error_tx(tx, "Failed to get response_status: %s",
                        ib_status_to_string(rc));
    }

    rc = ib_data_get_ex(tx->data, IB_S2SL("response_protocol"), &f);
    if (rc == IB_OK) {
        ib_list_push(list, f);
    }
    else {
        ib_log_error_tx(tx, "Failed to get response_protocol: %s",
                        ib_status_to_string(rc));
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "http-response-metadata",
                              "application/json",
                              list,
                              ib_auditlog_gen_json_flist,
                              NULL);

    return rc;
}

/**
 * Add request/response header fields to the audit log
 *
 * @param[in] tx Transaction
 * @param[in] mpool Memory pool to user for allocations
 * @param[in,out] list List to add the fields to
 * @param[in] label Label string ("request"/"response")
 * @param[in] header  Parsed header fields data
 *
 * @return Status code
 */
static ib_status_t ib_auditlog_add_part_http_head_fields(
    ib_tx_t *tx,
    ib_mpool_t *mpool,
    ib_list_t *list,
    const char *label,
    ib_parsed_header_wrapper_t *header )
{
    ib_parsed_name_value_pair_list_t *nvpair;
    ib_status_t rc;
    ib_field_t *f;

    /* Loop through all of the header name/value pairs */
    for (nvpair = header ->head;
         nvpair != NULL;
         nvpair = nvpair->next)
    {
        /* Create a field to hold the name/value pair. */
        rc = ib_field_create(&f, mpool,
                             (char *)ib_bytestr_const_ptr(nvpair->name),
                             ib_bytestr_length(nvpair->name),
                             IB_FTYPE_BYTESTR,
                             ib_ftype_bytestr_mutable_in(nvpair->value));
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to create %s header field: %s",
                            label, ib_status_to_string(rc));
            return rc;
        }

        /* Add the new field to the list */
        rc = ib_list_push(list, f);
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Failed to add %s field '%.*s': %s",
                            label,
                            (int) ib_bytestr_length(nvpair->name),
                            ib_bytestr_ptr(nvpair->name),
                            ib_status_to_string(rc));
            return rc;
        }
    }
    return IB_OK;
}

/**
 * Add request header to the audit log
 *
 * @param[in,out] log Audit log to log to
 *
 * @return Status code
 */
static ib_status_t ib_auditlog_add_part_http_request_head(ib_auditlog_t *log)
{
    ib_mpool_t *mpool = log->mp;
    ib_tx_t *tx = log->tx;
    ib_list_t *list;
    ib_field_t *f;
    ib_status_t rc;

    /* Generate a list of fields in this part. */
    rc = ib_list_create(&list, mpool);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add the raw request line */
    // FIXME: Why would this be NULL?  Should this ever happen?
    if (tx->request_line != NULL) {
        rc = ib_field_create(&f, mpool,
                             IB_FIELD_NAME("request_line"),
                             IB_FTYPE_BYTESTR,
                             tx->request_line->raw);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to create request line field: %s",
                            ib_status_to_string(rc));
            return rc;
        }

        rc = ib_list_push(list, f);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to add request line field: %s",
                            ib_status_to_string(rc));
            return rc;
        }
    }

    /* Add the request header fields */
    if (tx->request_header != NULL) {
        rc = ib_auditlog_add_part_http_head_fields(tx, mpool,
                                                   list, "request",
                                                   tx->request_header);
        if (rc != IB_OK) {
            return rc;
        }
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "http-request-header",
                              "application/octet-stream",
                              list,
                              ib_auditlog_gen_header_flist,
                              NULL);

    return rc;
}

static ib_status_t ib_auditlog_add_part_http_request_body(ib_auditlog_t *log)
{
    ib_tx_t *tx = log->tx;
    ib_status_t rc;

    rc = ib_auditlog_part_add(log,
                              "http-request-body",
                              "application/octet-stream",
                              tx->request_body,
                              ib_auditlog_gen_raw_stream,
                              NULL);

    return rc;
}

/**
 * Add response header to the audit log
 *
 * @param[in,out] log Audit log to log to
 *
 * @return Status code
 */
static ib_status_t ib_auditlog_add_part_http_response_head(ib_auditlog_t *log)
{
    ib_mpool_t *mpool = log->mp;
    ib_tx_t *tx = log->tx;
    ib_list_t *list;
    ib_field_t *f;
    ib_status_t rc;

    /* Generate a list of fields in this part. */
    rc = ib_list_create(&list, mpool);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add the raw response line
     *
     * The response_line may be NULL for HTTP/0.9 requests.
     */
    if (tx->response_line != NULL) {
        rc = ib_field_create(&f, mpool,
                             IB_FIELD_NAME("response_line"),
                             IB_FTYPE_BYTESTR,
                             tx->response_line->raw);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to create response line field: %s",
                            ib_status_to_string(rc));
            return rc;
        }
        rc = ib_list_push(list, f);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to add response line field: %s",
                            ib_status_to_string(rc));
            return rc;
        }
    }

    /* Add the response header fields */
    if (tx->response_header != NULL) {
        rc = ib_auditlog_add_part_http_head_fields(tx, mpool,
                                                   list, "response",
                                                   tx->response_header);
        if (rc != IB_OK) {
            return rc;
        }
    }

    /* Add the part to the auditlog. */
    rc = ib_auditlog_part_add(log,
                              "http-response-header",
                              "application/octet-stream",
                              list,
                              ib_auditlog_gen_header_flist,
                              NULL);

    return rc;
}

static ib_status_t ib_auditlog_add_part_http_response_body(ib_auditlog_t *log)
{
    ib_tx_t *tx = log->tx;
    ib_status_t rc;

    rc = ib_auditlog_part_add(log,
                              "http-response-body",
                              "application/octet-stream",
                              tx->response_body,
                              ib_auditlog_gen_raw_stream,
                              NULL);

    return rc;
}

/**
 * Handle writing the logevents.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param event Event type.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t logevent_hook_logging(ib_engine_t *ib,
                                         ib_tx_t *tx,
                                         ib_state_event_type_t event,
                                         void *cbdata)
{
    assert(event == handle_logging_event);

    ib_auditlog_t *log;
    ib_core_cfg_t *corecfg;
    core_audit_cfg_t *cfg;
    ib_provider_inst_t *audit;
    ib_list_t *events;
    uint32_t boundary_rand = rand(); /// @todo better random num
    char boundary[46];
    ib_status_t rc;

    /* If there's not events, do nothing */
    if (tx->logevents == NULL) {
        return IB_OK;
    }

    rc = ib_context_module_config(tx->ctx, ib_core_module(),
                                  (void *)&corecfg);
    if (rc != IB_OK) {
        return rc;
    }

    switch (corecfg->audit_engine) {
        /* Always On */
        case 1:
            break;
        /* Only if events are present */
        case 2:
            rc = ib_logevent_get_all(tx, &events);
            if (rc != IB_OK) {
                return rc;
            }
            if (ib_list_elements(events) == 0) {
                return IB_OK;
            }
            break;
        /* Anything else is Off */
        default:
            return IB_OK;
    }

    /* Mark time. */
    tx->t.logtime = ib_clock_get_time();

    /* Auditing */
    /// @todo Only create if needed
    log = (ib_auditlog_t *)ib_mpool_calloc(tx->mp, 1, sizeof(*log));
    if (log == NULL) {
        return IB_EALLOC;
    }

    log->ib = ib;
    log->mp = tx->mp;
    log->ctx = tx->ctx;
    log->tx = tx;

    rc = ib_list_create(&log->parts, log->mp);
    if (rc != IB_OK) {
        return rc;
    }

    /* Create a unique MIME boundary. */
    snprintf(boundary, sizeof(boundary), "%08x-%s",
             boundary_rand, log->tx->id ? log->tx->id : "FixMe-No-Tx-on-Audit");

    /* Create the core config. */
    cfg = (core_audit_cfg_t *)ib_mpool_calloc(log->mp, 1, sizeof(*cfg));
    if (cfg == NULL) {
        return IB_EALLOC;
    }

    cfg->tx = tx;
    cfg->boundary = boundary;
    log->cfg_data = cfg;


    /* Add all the parts to the log. */
    if (corecfg->auditlog_parts & IB_ALPART_HEADER) {
        ib_auditlog_add_part_header(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_EVENTS) {
        ib_auditlog_add_part_events(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_REQUEST_METADATA) {
        ib_auditlog_add_part_http_request_meta(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_RESPONSE_METADATA) {
        ib_auditlog_add_part_http_response_meta(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_REQUEST_HEADER) {
        ib_auditlog_add_part_http_request_head(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_REQUEST_BODY) {
        ib_auditlog_add_part_http_request_body(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_RESPONSE_HEADER) {
        ib_auditlog_add_part_http_response_head(log);
    }
    if (corecfg->auditlog_parts & IB_ALPART_HTTP_RESPONSE_BODY) {
        ib_auditlog_add_part_http_response_body(log);
    }

    /* Audit Log Provider Instance */
    rc = ib_provider_instance_create_ex(ib, corecfg->pr.audit, &audit,
                                        tx->mp, log);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx,
                        "Failed to create audit log provider instance: %s",
                        ib_status_to_string(rc));
        return rc;
    }

    ib_auditlog_write(audit);

    /* Events */
    ib_logevent_write_all(tx);

    return IB_OK;
}

/**
 * Handle the connection starting.
 *
 * Create the data provider instance and initialize the parser.
 *
 * @param ib Engine.
 * @param event Event type.
 * @param conn Connection.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t core_hook_conn_started(ib_engine_t *ib,
                                          ib_state_event_type_t event,
                                          ib_conn_t *conn,
                                          void *cbdata)
{
    assert(event == conn_started_event);

    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    rc = ib_context_module_config(conn->ctx, ib_core_module(),
                                  (void *)&corecfg);

    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize core module: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}


/* -- Parser Implementation -- */

/**
 * Parser provider registration function.
 *
 * This just does a version and sanity check on a registered provider.
 *
 * @param ib Engine
 * @param pr Logger provider
 *
 * @returns Status code
 */
static ib_status_t parser_register(ib_engine_t *ib,
                                   ib_provider_t *pr)
{
    assert(pr != NULL);
    IB_PROVIDER_IFACE_TYPE(parser) *iface =
        (IB_PROVIDER_IFACE_TYPE(parser) *)pr->iface;
    assert(iface != NULL);

    /* Check that versions match. */
    if (iface->version != IB_PROVIDER_VERSION_PARSER) {
        return IB_EINCOMPAT;
    }

    return IB_OK;
}

/* -- Matcher Implementation -- */

/**
 * Compile a pattern.
 *
 * @param mpr Matcher provider
 * @param pool Memory pool
 * @param pcpatt Address which compiled pattern is written
 * @param patt Pattern
 * @param errptr Address which any error is written (if non-NULL)
 * @param erroffset Offset in pattern where the error occurred (if non-NULL)
 *
 * @returns Status code
 */
static ib_status_t matcher_api_compile_pattern(ib_provider_t *mpr,
                                               ib_mpool_t *pool,
                                               void *pcpatt,
                                               const char *patt,
                                               const char **errptr,
                                               int *erroffset)

{
    IB_PROVIDER_IFACE_TYPE(matcher) *iface =
        mpr ? (IB_PROVIDER_IFACE_TYPE(matcher) *)mpr->iface : NULL;
    ib_status_t rc;

    if (iface == NULL) {
        /// @todo Probably should not need this check
        ib_util_log_error("Failed to fetch matcher interface");
        return IB_EUNKNOWN;
    }

    if (iface->compile == NULL) {
        return IB_ENOTIMPL;
    }

    rc = iface->compile(mpr, pool, pcpatt, patt, errptr, erroffset);
    return rc;
}

/**
 * Match a compiled pattern against a buffer.
 *
 * @param mpr Matcher provider
 * @param cpatt Compiled pattern
 * @param flags Flags
 * @param data Data buffer to perform match on
 * @param dlen Data buffer length
 * @param ctx Context
 *
 * @returns Status code
 */
static ib_status_t matcher_api_match_compiled(ib_provider_t *mpr,
                                              void *cpatt,
                                              ib_flags_t flags,
                                              const uint8_t *data,
                                              size_t dlen,
                                              void *ctx)
{
    IB_PROVIDER_IFACE_TYPE(matcher) *iface =
        mpr ? (IB_PROVIDER_IFACE_TYPE(matcher) *)mpr->iface : NULL;
    ib_status_t rc;

    if (iface == NULL) {
        /// @todo Probably should not need this check
        ib_util_log_error("Failed to fetch matcher interface");
        return IB_EUNKNOWN;
    }

    if (iface->match_compiled == NULL) {
        return IB_ENOTIMPL;
    }

    rc = iface->match_compiled(mpr, cpatt, flags, data, dlen, ctx);
    return rc;
}

/**
 * Add a pattern to a matcher provider instance.
 *
 * Multiple patterns can be added to a provider instance and all used
 * to perform a match later on.
 *
 * @todo Document parameters
 *
 * @returns Status code
 */
static ib_status_t matcher_api_add_pattern_ex(ib_provider_inst_t *mpi,
                                              void *patterns,
                                              const char *patt,
                                              ib_void_fn_t callback,
                                              void *arg,
                                              const char **errptr,
                                              int *erroffset)
{
    assert(mpi != NULL);
    assert(mpi->pr != NULL);

    IB_PROVIDER_IFACE_TYPE(matcher) *iface = NULL;

    ib_status_t rc;
    iface = (IB_PROVIDER_IFACE_TYPE(matcher) *)mpi->pr->iface;

    rc = iface->add_ex(mpi, patterns, patt, callback, arg,
                       errptr, erroffset);
    if (rc != IB_OK) {
        ib_log_error(mpi->pr->ib,
                     "Failed to add pattern %s patt: (%s) %s at"
                     " offset %d", patt, ib_status_to_string(rc), *errptr,
                     *erroffset);
        return rc;
    }

    return IB_OK;
}


/**
 * Add a pattern to a matcher provider instance.
 *
 * Multiple patterns can be added to a provider instance and all used
 * to perform a match later on.
 *
 * @param mpi Matcher provider instance
 * @param patt Pattern
 *
 * @returns Status code
 */
static ib_status_t matcher_api_add_pattern(ib_provider_inst_t *mpi,
                                           const char *patt)
{
    return IB_ENOTIMPL;
}

/**
 * Match all the provider instance patterns on a data field.
 *
 * @warning Not yet implemented
 *
 * @param mpi Matcher provider instance
 * @param flags Flags
 * @param data Data buffer
 * @param dlen Data buffer length
 * @param ctx Context
 *
 * @returns Status code
 */
static ib_status_t matcher_api_match(ib_provider_inst_t *mpi,
                                     ib_flags_t flags,
                                     const uint8_t *data,
                                     size_t dlen,
                                     void *ctx)
{
    return IB_ENOTIMPL;
}

/**
 * Matcher provider API mapping for core module.
 */
static IB_PROVIDER_API_TYPE(matcher) matcher_api = {
    matcher_api_compile_pattern,
    matcher_api_match_compiled,
    matcher_api_add_pattern,
    matcher_api_add_pattern_ex,
    matcher_api_match,
};

/**
 * Matcher provider registration function.
 *
 * This just does a version and sanity check on a registered provider.
 *
 * @param ib Engine
 * @param mpr Matcher provider
 *
 * @returns Status code
 */
static ib_status_t matcher_register(ib_engine_t *ib,
                                    ib_provider_t *mpr)
{
    IB_PROVIDER_IFACE_TYPE(matcher) *iface =
        (IB_PROVIDER_IFACE_TYPE(matcher) *)mpr->iface;

    /* Check that versions match. */
    if (iface->version != IB_PROVIDER_VERSION_MATCHER) {
        return IB_EINCOMPAT;
    }

    /* Verify that required interface functions are implemented. */
    /// @todo

    return IB_OK;
}


/* -- Filters -- */

/**
 * Core buffer filter.
 *
 * This is a simplistic buffer filter that holds request data while
 * it can be inspected.
 *
 * @todo This needs lots of work on configuration, etc.
 *
 * @param f Filter
 * @param fdata Filter data
 * @param ctx Config context
 * @param pool Memory pool
 * @param pflags Address which flags are written
 *
 * @returns Status code
 */
static ib_status_t filter_buffer(ib_filter_t *f,
                                 ib_fdata_t *fdata,
                                 ib_context_t *ctx,
                                 ib_mpool_t *pool,
                                 ib_flags_t *pflags)
{
    ib_stream_t *buf = (ib_stream_t *)fdata->state;
    ib_sdata_t *sdata;
    ib_status_t rc;

    if (buf == NULL) {
        fdata->state = ib_mpool_calloc(pool, 1, sizeof(*buf));
        if (buf == NULL) {
            return IB_EALLOC;
        }
        buf = (ib_stream_t *)fdata->state;
    }

    /* Move data to buffer until we get an EOS, then move
     * the data back into the stream. */
    /// @todo Need API to move data between streams.
    rc = ib_stream_pull(fdata->stream, &sdata);
    while (rc == IB_OK) {
        rc = ib_stream_push_sdata(buf, sdata);
        if (rc == IB_OK) {
            if (sdata->type == IB_STREAM_EOS) {
                rc = ib_stream_pull(buf, &sdata);
                while (rc == IB_OK) {
                    rc = ib_stream_push_sdata(fdata->stream, sdata);
                    if (rc == IB_OK) {
                        rc = ib_stream_pull(buf, &sdata);
                    }
                }
                if (rc != IB_ENOENT) {
                    return rc;
                }
                break;
            }
            rc = ib_stream_pull(fdata->stream, &sdata);
        }
    }
    if (rc != IB_ENOENT) {
        return rc;
    }

    return IB_OK;
}

/**
 * Configure the filter controller.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param event Event type.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t filter_ctl_config(ib_engine_t *ib,
                                     ib_tx_t *tx,
                                     ib_state_event_type_t event,
                                     void *cbdata)
{
    assert(event == handle_context_tx_event);

    ib_status_t rc = IB_OK;

    /// @todo Need an API for this.
    tx->fctl->filters = tx->ctx->filters;
    tx->fctl->fbuffer = (ib_filter_t *)cbdata;
    ib_fctl_meta_add(tx->fctl, IB_STREAM_FLUSH);

    return rc;
}


/* -- Core Data Processors -- */

/**
 * Initialize the DPI in the given transaction.
 *
 * @param[in] ib IronBee object.
 * @param[in,out] tx The transaction whose tx->data will be populated wit
 *                default values.
 *
 * @returns IB_OK on success or the failure of ib_data_add_list(...).
 */
static ib_status_t data_default_init(ib_engine_t *ib, ib_tx_t *tx)
{
    ib_status_t rc;

    assert(ib!=NULL);
    assert(tx!=NULL);
    assert(tx->data!=NULL);

    rc = ib_data_add_list_ex(tx->data, IB_TX_CAPTURE, 2, NULL);

    if (rc!=IB_OK) {
        ib_log_debug2_tx(tx, "Unable to add list \""IB_TX_CAPTURE"\".");
        return rc;
    }

    return rc;
}

/* -- Core Hook Handlers -- */

/**
 * Execute the InitVar directive to initialize field in a transaction's DPI
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] initvar_list List of the initvar fields
 *
 * @returns Status code.
 */
static ib_status_t core_initvar(ib_engine_t *ib,
                                ib_tx_t *tx,
                                const ib_list_t *initvar_list)
{
    const ib_list_node_t *node;
    ib_status_t rc = IB_OK;

    if (initvar_list == NULL) {
        ib_log_debug_tx(tx, "No InitVars defined for context \"%s\"",
                        ib_context_full_get(tx->ctx));
        return IB_OK;
    }

    IB_LIST_LOOP_CONST(initvar_list, node) {
        ib_status_t trc; /* Temp RC */
        const ib_field_t *field =
            (const ib_field_t *)ib_list_node_data_const(node);
        ib_field_t *newf;

        trc = ib_field_copy(&newf, tx->mp, field->name, field->nlen, field);
        if (trc != IB_OK) {
            ib_log_debug_tx(tx, "Failed to copy field: %s",
                            ib_status_to_string(trc));
            if (rc == IB_OK) {
                rc = trc;
            }
            continue;
        }

        trc = ib_data_add(tx->data, newf);
        if (trc != IB_OK) {
            ib_log_error_tx(tx, "Failed to add field \"%.*s\" to TX DPI: %s",
                            (int)field->nlen, field->name,
                            ib_status_to_string(trc));
            if (rc == IB_OK) {
                rc = trc;
            }
        }
        else {
            ib_log_trace_tx(tx, "InitVar: Created field \"%.*s\" (type %s)",
                            (int)field->nlen, field->name,
                            ib_field_type_name(field->type));
        }
    }
    ib_log_debug_tx(tx, "Created %zd InitVar fields for context \"%s\"",
                    ib_list_elements(initvar_list),
                    ib_context_full_get(tx->ctx));

    return rc;
}

/**
 * Populate @a tx's collections using the associated context's list of managed
 * collections.
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] mancoll_list List of the managed collections
 *
 * @returns Status code.
 */
static ib_status_t core_managed_collection_populate_tx(
    ib_engine_t *ib,
    ib_tx_t *tx,
    const ib_list_t *mancoll_list)
{
    const ib_list_node_t *node;
    ib_status_t rc = IB_OK;
    size_t count;

    /* If there are no managed collections, done */
    count = (mancoll_list == NULL) ? 0 : ib_list_elements(mancoll_list);
    if (count == 0) {
        ib_log_debug_tx(tx,
                        "No managed collections defined for context \"%s\"",
                        ib_context_full_get(tx->ctx));
        return IB_OK;
    }

    /* Walk through the list of collections & populate them. */
    IB_LIST_LOOP_CONST(mancoll_list, node) {
        const ib_managed_collection_t *collection =
            (const ib_managed_collection_t *)node->data;

        rc = ib_managed_collection_populate(ib, tx, collection);
        if (rc != IB_OK) {
            ib_log_warning_tx(tx,
                              "Error creating managed collection \"%s\": %s",
                              collection->collection_name,
                              ib_status_to_string(rc));
            return rc;
        }
        ib_log_trace_tx(tx, "Created managed collection \"%s\"",
                        collection->collection_name);
    }
    ib_log_debug_tx(tx,
                    "Created %zd managed collections for context \"%s\"",
                    count, ib_context_full_get(tx->ctx));
    return IB_OK;
}

/**
 * Handle the transaction context selected
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param event Event type.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t core_hook_context_tx(ib_engine_t *ib,
                                        ib_tx_t *tx,
                                        ib_state_event_type_t event,
                                        void *cbdata)
{
    assert(event == handle_context_tx_event);

    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    rc = ib_context_module_config(tx->ctx, ib_core_module(),
                                  (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx,
                        "Failure accessing core module: %s",
                        ib_status_to_string(rc));
        return rc;
    }

    /* Handle InitVar list */
    rc = core_initvar(ib, tx, corecfg->initvar_list);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx, "Failure executing InitVar(s): %s",
                        ib_status_to_string(rc));
        return rc;
    }

    /* Handle InitCollection list */
    rc = core_managed_collection_populate_tx(ib, tx, corecfg->mancoll_list);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx, "Failure executing InitCollection(s): %s",
                        ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Handle the transaction starting.
 *
 * Create the transaction provider instances.  And setup placeholders
 * for all of the core fields. This allows other modules to refer to
 * the field prior to it it being initialized.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param event Event type.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t core_hook_tx_started(ib_engine_t *ib,
                                        ib_tx_t *tx,
                                        ib_state_event_type_t event,
                                        void *cbdata)
{
    assert(event == tx_started_event);

    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    rc = ib_context_module_config(tx->ctx, ib_core_module(),
                                  (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx,
                        "Failure accessing core module: %s",
                        ib_status_to_string(rc));
        return rc;
    }

    /* Set default inspection options. */
    tx->flags |= corecfg->inspection_engine_options;

    /* Data Default Initialization */
    rc = data_default_init(ib, tx);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx, "Failed to initialize data provider instance.");
        return rc;
    }

    /* Create the rule engine execution environment object */
    rc = ib_rule_exec_create(tx, NULL);
    if (rc != IB_OK) {
        ib_rule_log_tx_error(tx,
                             "Failed to create rule execution object: %s",
                             ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Handle the tx logging event.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param event Event type.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t core_hook_logging(ib_engine_t *ib,
                                     ib_tx_t *tx,
                                     ib_state_event_type_t event,
                                     void *cbdata)
{
    assert(event == handle_logging_event);
    ib_status_t rc;

    rc = ib_managed_collection_persist_tx(ib, tx);
    return rc;
}

static ib_status_t core_hook_request_body_data(ib_engine_t *ib,
                                               ib_tx_t *tx,
                                               ib_state_event_type_t event,
                                               ib_txdata_t *txdata,
                                               void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);

    ib_core_cfg_t *corecfg;
    void *data_copy;
    ib_status_t rc;

    if (txdata == NULL) {
        return IB_OK;
    }

    /* Get the current context config. */
    rc = ib_context_module_config(tx->ctx, ib_core_module(), (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib,
                     "Failed to fetch core module context config: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    if (! (corecfg->auditlog_parts & IB_ALPART_HTTP_REQUEST_BODY)) {
        return IB_OK;
    }

    data_copy = ib_mpool_memdup(tx->mp, txdata->data, txdata->dlen);

    // TODO: Add a limit to this: size and type
    rc = ib_stream_push(tx->request_body,
                        IB_STREAM_DATA,
                        data_copy,
                        txdata->dlen);

    return rc;
}

static ib_status_t core_hook_response_body_data(ib_engine_t *ib,
                                                ib_tx_t *tx,
                                                ib_state_event_type_t event,
                                                ib_txdata_t *txdata,
                                                void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);

    ib_core_cfg_t *corecfg;
    void *data_copy;
    ib_status_t rc;

    if (txdata == NULL) {
        return IB_OK;
    }

    /* Get the current context config. */
    rc = ib_context_module_config(tx->ctx, ib_core_module(), (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib,
                     "Failed to fetch core module context config: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    if (! (corecfg->auditlog_parts & IB_ALPART_HTTP_RESPONSE_BODY)) {
        return IB_OK;
    }

    data_copy = ib_mpool_memdup(tx->mp, txdata->data, txdata->dlen);

    // TODO: Add a limit to this: size and type
    rc = ib_stream_push(tx->response_body,
                        IB_STREAM_DATA,
                        data_copy,
                        txdata->dlen);

    return rc;
}

ib_status_t ib_core_module_data(ib_module_t **core_module,
                                ib_core_module_data_t **core_data)
{
    ib_module_t *module;

    /* Get the core module data */
    module = ib_core_module();
    if (core_module != NULL) {
        *core_module = module;
    }

    if (core_data == NULL) {
        return IB_OK;
    }

    if (core_data != NULL) {
        *core_data = (ib_core_module_data_t *)module->data;
        if (*core_data == NULL) {
            return IB_EUNKNOWN;
        }
    }
    return IB_OK;
}


/* -- Directive Handlers -- */

/**
 * Make an absolute filename out of a base directory and relative filename.
 *
 * @todo Needs to not assume the trailing slash will be there.
 *
 * @param ib Engine
 * @param basedir Base directory
 * @param file Relative filename
 * @param pabsfile Address which absolute path is written
 *
 * @returns Status code
 */
static ib_status_t core_abs_module_path(ib_engine_t *ib,
                                        const char *basedir,
                                        const char *file,
                                        char **pabsfile)
{
    ib_mpool_t *pool = ib_engine_pool_config_get(ib);

    *pabsfile = (char *)
        ib_mpool_alloc(pool, strlen(basedir) + 1 + strlen(file) + 1);
    if (*pabsfile == NULL) {
        return IB_EALLOC;
    }

    strcpy(*pabsfile, basedir);
    strcat(*pabsfile, "/");
    strcat(*pabsfile, file);

    return IB_OK;
}

/**
 * Core: Create site
 *
 * @param[in] cp Configuration parser
 * @param[in,out] core_data Core module data
 * @param[in] site_name Site name string
 * @param[out] pctx Pointer to new context / NULL
 * @param[out] psite Pointer to new site object / NULL
 *
 * @returns Status code:
 * - IB_OK
 * - Errors from ib_context_create()
 * - Errors from ib_context_data_set()
 * - Errors from ib_cfgparser_context_push()
 */
static ib_status_t core_site_create(
    ib_cfgparser_t *cp,
    ib_core_module_data_t *core_data,
    const char *site_name,
    ib_context_t **pctx,
    ib_site_t **psite)
{
    assert(cp != NULL);
    assert(core_data != NULL);
    assert(site_name != NULL);

    ib_engine_t *ib = cp->ib;
    ib_context_t *ctx;
    ib_status_t rc;


    /* Create the site list if this is the first site */
    if (core_data->site_list == NULL) {
        rc = ib_list_create(&(core_data->site_list), cp->cur_ctx->mp);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to create core site list: %s",
                         ib_status_to_string(rc));
            return rc;
        }
    }

    /* Create the context */
    ib_cfg_log_debug2(cp, "Creating site context for \"%s\"", site_name);
    rc = ib_context_create(ib, cp->cur_ctx, IB_CTYPE_SITE,
                           "site", site_name, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to create context for \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return IB_EINVAL;
    }
    core_data->cur_ctx = ctx;

    ib_cfg_log_debug2(cp, "Opening site context %p for \"%s\"", ctx, site_name);
    rc = ib_context_open(ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error opening context for \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return IB_EINVAL;
    }

    /* Create the site */
    rc = ib_ctxsel_site_create(ctx, site_name, psite);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error creating site \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return rc;
    }

    /* Store the site in the context */
    rc = ib_context_site_set(ctx, *psite);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to set site for site context \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return rc;
    }

    if (pctx != NULL) {
        *pctx = ctx;
    }
    return IB_OK;
}

/**
 * Core: Site open
 *
 * @param[in] cp Configuration parser
 * @param[in] ctx Configuration context
 * @param[in,out] core_data Core module data
 * @param[in,out] site Site to open
 *
 * @returns Status code:
 * - IB_OK
 * - Errors from ib_context_config_set_parser()
 * - Errors from ib_cfgparser_context_push()
 * - Errors from ib_ctxsel_open()
 */
static ib_status_t core_site_open(ib_cfgparser_t *cp,
                                  ib_context_t *ctx,
                                  ib_core_module_data_t *core_data,
                                  ib_site_t *site)
{
    assert(cp != NULL);
    assert(site != NULL);
    ib_status_t rc;

    if (core_data->cur_site != NULL) {
        return IB_EUNKNOWN;
    }

    rc = ib_ctxsel_site_open(cp->ib, site);
    if (rc != IB_OK) {
        return rc;
    }

    core_data->cur_site = site;
    return rc;
}

/**
 * Core: Site close
 *
 * @param[in] cp Configuration parser
 * @param[in,out] core_data Core module data
 * @param[in,out] site Site to close
 *
 * @returns Status code:
 * - IB_OK
 * - Errors from ib_cfgparser_context_pop()
 * - Errors from ib_ctxsel_close()
 */
static ib_status_t core_site_close(
    ib_cfgparser_t *cp,
    ib_core_module_data_t *core_data,
    ib_site_t *site)
{
    assert(cp != NULL);
    assert(site != NULL);
    ib_status_t rc;
    ib_context_t *ctx;

    if (core_data->cur_site == NULL) {
        return IB_EUNKNOWN;
    }

    /* Verify that the current context matches the site context */
    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        goto done;
    }
    if (core_data->cur_ctx != ctx) {
        rc = IB_EUNKNOWN;
        goto done;
    }

    /* Close the site */
    rc = ib_ctxsel_site_close(cp->ib, site);
    if (rc != IB_OK) {
        goto done;
    }

    /* Close the context */
    ib_cfg_log_debug2(cp, "Closing context %p for site \"%s\"",
                      ctx, site->name);
    rc = ib_context_close(ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error closing context for site \"%s\" end: %s",
                         site->name, ib_status_to_string(rc));
        goto done;
    }

    /* NULL the site pointer *after* closing the site */
done :
    core_data->cur_site = NULL;
    return rc;
}

/**
 * Core: Create a location for a site
 *
 * @param[in] cp Configuration parser
 * @param[in,out] core_data Core module data
 * @param[in] path Location path
 * @param[in,out] plocation Pointer to new location object
 *
 * @returns Status code:
 * - IB_OK
 * - Errors from ib_context_create()
 * - Errors from ib_context_data_set()
 * - Errors from ib_context_config_set_parser()
 * - Errors from ib_cfgparser_context_push()
 */
static ib_status_t core_location_create(
    ib_cfgparser_t *cp,
    ib_core_module_data_t *core_data,
    const char *path,
    ib_site_location_t **plocation)
{
    assert(cp != NULL);
    assert(core_data != NULL);
    assert(core_data->cur_site != NULL);
    assert(path != NULL);

    ib_status_t rc;
    ib_context_t *ctx;
    ib_site_t *site = core_data->cur_site;

    rc = ib_context_create(cp->ib, site->context, IB_CTYPE_LOCATION,
                           "location", path, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_debug2(cp,
                          "Failed to create location context for \"%s:%s\": %s",
                          site->name, path, ib_status_to_string(rc));
        return rc;
    }

    /* Store the site in the context */
    rc = ib_context_site_set(ctx, site);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to set site for context \"%s\": %s",
                         ib_context_full_get(ctx), ib_status_to_string(rc));
        return rc;
    }

    /* Open the new context */
    rc = ib_context_open(ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error opening context for \"%s:%s\": %s",
                         site->name, path, ib_status_to_string(rc));
        return rc;
    }
    core_data->cur_ctx = ctx;

    /* Create the location object */
    rc = ib_ctxsel_location_create(site, ctx, path, plocation);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to create location \"%s:%s\": %s",
                         site->name, path, ib_status_to_string(rc));
        return rc;
    }

    /* Store the site in the context */
    rc = ib_context_location_set(ctx, *plocation);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to set location for context \"%s\": %s",
                         ib_context_full_get(ctx), ib_status_to_string(rc));
        return rc;
    }

    ib_cfg_log_debug2(cp, "Created location context for \"%s:%s\"",
                      site->name, path);

    return IB_OK;
}

/**
 * Core: Location open
 *
 * @param[in] cp Configuration parser
 * @param[in,out] core_data Core module data
 * @param[in,out] location Location to open
 *
 * @returns Status code:
 * - IB_OK
 * - IB_EUNKNOWN if current location is not NULL
 * - Errors from ib_ctxsel_location_open()
 */
static ib_status_t core_location_open(ib_cfgparser_t *cp,
                                      ib_core_module_data_t *core_data,
                                      ib_site_location_t *location)
{
    assert(cp != NULL);
    assert(location != NULL);

    ib_status_t rc;
    ib_core_cfg_t *site_cfg;
    ib_core_cfg_t *location_cfg;

    if (core_data->cur_location != NULL) {
        return IB_EUNKNOWN;
    }

    rc = ib_ctxsel_location_open(cp->ib, location);
    core_data->cur_location = location;
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_context_module_config(location->site->context, ib_core_module(),
                                  (void *)&site_cfg);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_context_module_config(location->context, ib_core_module(),
                                  (void *)&location_cfg);
    if (rc != IB_OK) {
        return rc;
    }

    /* Copy the InitVar list from the site context */
    if (site_cfg->initvar_list != NULL) {
        const ib_list_node_t *node;

        rc = ib_list_create(&(location_cfg->initvar_list),
                            location->context->mp);
        if (rc != IB_OK) {
            return rc;
        }
        IB_LIST_LOOP_CONST(site_cfg->initvar_list, node) {
            assert(node->data != NULL);
            rc = ib_list_push(location_cfg->initvar_list, node->data);
            if (rc != IB_OK) {
                return rc;
            }
        }
    }

    return rc;
}

/**
 * Core: Location close
 *
 * @param[in] cp Configuration parser
 * @param[in,out] core_data Core module data
 * @param[in,out] location Location to close
 *
 * @returns Status code:
 * - IB_OK
 * - IB_EUNKNOWN if current location is not NULL
 * - Errors from ib_ctxsel_location_open()
 */
static ib_status_t core_location_close(ib_cfgparser_t *cp,
                                       ib_core_module_data_t *core_data,
                                       ib_site_location_t *location)
{
    assert(cp != NULL);
    assert(core_data != NULL);
    assert(location != NULL);

    ib_status_t rc;
    ib_context_t *ctx;

    if (core_data->cur_location == NULL) {
        return IB_EUNKNOWN;
    }

    /* Verify that the context matches */
    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        goto done;
    }
    if (core_data->cur_ctx != ctx) {
        rc = IB_EUNKNOWN;
        goto done;
    }

    /* Close the context */
    ib_cfg_log_debug2(cp, "Closing location context \"%s\"",
                      ib_context_full_get(ctx));
    rc = ib_context_close(ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error closing context \"%s\": %s",
                         ib_context_full_get(ctx), ib_status_to_string(rc));
        goto done;
    }

    /* After closing the context, store the current one */
    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        goto done;
    }
    core_data->cur_ctx = ctx;

    /* Close the location */
    rc = ib_ctxsel_location_close(cp->ib, location);

done:
    core_data->cur_location = NULL;
    return rc;
}

/**
 * Handle the start of a Site block.
 *
 * This function sets up the new site and pushes it onto the parser stack.
 *
 * @param cp Config parser
 * @param dir_name Directive name
 * @param p1 First parameter
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_site_start(ib_cfgparser_t *cp,
                                       const char *dir_name,
                                       const char *p1,
                                       void *cbdata)
{
    assert(cp != NULL);

    ib_engine_t *ib = cp->ib;
    ib_context_t *ctx;
    ib_status_t rc;
    char *site_name;
    ib_site_t *site;
    ib_core_module_data_t *core_data;

    assert(ib != NULL);
    assert(ib->mp != NULL);
    assert(p1 != NULL);

    /* Get core module data */
    rc = ib_core_module_data(NULL, &core_data);
    if (rc != IB_OK) {
        return rc;
    }

    /* Checks */
    if (core_data->cur_site != NULL) {
        ib_cfg_log_error(cp, "Site start within site \"%s\"",
                         core_data->cur_site->name);
        return IB_EINVAL;
    }

    /* Unescape the parameter */
    rc = core_unescape(ib, &site_name, p1);
    if (rc != IB_OK) {
        ib_cfg_log_debug2(cp, "Could not unescape configuration %s=%s",
                          dir_name, p1);
        return rc;
    }

    /* Create and open the site object */
    rc = core_site_create(cp, core_data, site_name, &ctx, &site);
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_site_open(cp, ctx, core_data, site);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error opening site \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return rc;
    }

    ib_cfg_log_debug2(cp, "Created site \"%s\"", site_name);
    return rc;
}

/**
 * Handle the end of a Site block.
 *
 * This function closes out the site and pops it from the parser stack.
 *
 * @param cp Config parser
 * @param dir_name Directive name
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_site_end(ib_cfgparser_t *cp,
                                     const char *dir_name,
                                     void *cbdata)
{
    assert( cp != NULL );
    assert( cp->ib != NULL );
    assert( dir_name != NULL );

    ib_status_t rc;
    ib_core_module_data_t *core_data;
    const char *site_name;

    /* Get core module data */
    rc = ib_core_module_data(NULL, &core_data);
    if (rc != IB_OK) {
        return rc;
    }

    ib_cfg_log_debug2(cp, "Processing end of site block \"%s\"", dir_name);

    if (core_data->cur_site == NULL) {
        ib_cfg_log_error(cp, "Site end with no open site");
        return IB_EINVAL;
    }
    site_name = core_data->cur_site->name;

    rc = core_site_close(cp, core_data, core_data->cur_site);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error closing site \"%s\": %s",
                         site_name, ib_status_to_string(rc));
        return IB_EINVAL;
    }

    return IB_OK;
}

/**
 * Handle the start of a Location block.
 *
 * This function sets up the new location and pushes it onto the parser stack.
 *
 * @param cp Config parser
 * @param dir_name Directive name
 * @param p1 First parameter
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_loc_start(ib_cfgparser_t *cp,
                                      const char *dir_name,
                                      const char *p1,
                                      void *cbdata)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);

    ib_engine_t *ib = cp->ib;
    ib_site_t *site;
    ib_site_location_t *location;
    ib_status_t rc;
    char *path;
    ib_core_module_data_t *core_data;

    assert(dir_name != NULL);
    assert(p1 != NULL);

    /* Get core module data */
    rc = ib_core_module_data(NULL, &core_data);
    if (rc != IB_OK) {
        return rc;
    }

    /* Check that we're in a site, and not in a location */
    site = core_data->cur_site;
    if (site == NULL) {
        ib_cfg_log_debug2(cp, "%s directive with no site", dir_name);
        return IB_EINVAL;
    }
    if (core_data->cur_location != NULL) {
        ib_cfg_log_debug2(cp, "%s directive with location \"%s:%s\" open",
                          dir_name,
                          site->name,
                          core_data->cur_location->path);
        return IB_EINVAL;
    }

    rc = core_unescape(ib, &path, p1);
    if (rc != IB_OK) {
        ib_cfg_log_debug2(cp, "Failed to unescape parameter %s=%s.",
                          dir_name, p1);
        return rc;
    }

    /* Create and open the location object */
    rc = core_location_create(cp, core_data, path, &location);
    if (rc != IB_OK) {
        return rc;
    }
    rc = core_location_open(cp, core_data, location);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Handle the end of a Location block.
 *
 * This function closes out the location and pops it from the parser stack.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_loc_end(ib_cfgparser_t *cp,
                                    const char *name,
                                    void *cbdata)
{
    ib_status_t rc;
    ib_core_module_data_t *core_data;

    /* Get core module data */
    rc = ib_core_module_data(NULL, &core_data);
    if (rc != IB_OK) {
        return rc;
    }

    if (core_data->cur_location == NULL) {
        ib_cfg_log_error(cp, "End of location block with no open!");
        return IB_EINVAL;
    }

    ib_cfg_log_debug2(cp, "Processing location block \"%s\"", name);
    rc = core_location_close(cp, core_data, core_data->cur_location);
    if (rc != IB_OK) {
        return IB_EINVAL;
    }
    core_data->cur_location = NULL;

    return IB_OK;
}

/**
 * Handle the site-specific directives
 *
 * @param cp Config parser
 * @param directive Directive name
 * @param vars The list of variables passed to @a directive.
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_site_list(ib_cfgparser_t *cp,
                                      const char *directive,
                                      const ib_list_t *vars,
                                      void *cbdata)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);
    assert(directive != NULL);
    assert(vars != NULL);

    ib_engine_t *ib = cp->ib;
    ib_status_t rc;
    const ib_list_node_t *node;
    const char *param1;
    const char *param1u;
    ib_core_module_data_t *core_data;
    ib_site_t *site;

    /* Get core module data */
    rc = ib_core_module_data(NULL, &core_data);
    if (rc != IB_OK) {
        return rc;
    }

    /* Get the first parameter */
    node = ib_list_first_const(vars);
    if (node == NULL) {
        ib_cfg_log_error(cp, "No %s specified for \"%s\" directive",
                         directive, directive);
        return IB_EINVAL;
    }
    param1 = (const char *)node->data;

    /* Verify that we are in a site */
    if (core_data->cur_site == NULL) {
        ib_cfg_log_error(cp, "No site for %s directive", directive);
        return IB_EINVAL;
    }
    site = core_data->cur_site;

    /* We remove constness to populate this buffer. */
    rc = core_unescape(ib, (char**)&param1u, param1);
    if (rc != IB_OK) {
        ib_cfg_log_debug2(cp, "Failed to unescape %s parameter \"%s\"",
                          directive, param1);
        return rc;
    }

    /* Now, look at the parameter name */
    if (strcasecmp("SiteId", directive) == 0) {

        /* Store the ASCII version for logging */
        site->id_str =
            ib_mpool_strdup(ib_engine_pool_config_get(ib), param1u);

        /* Calculate the binary version. */
        rc = ib_uuid_ascii_to_bin(&site->id, (const char *)param1u);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Invalid UUID at %s: %s should have UUID format "
                             "(xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx "
                             "where x are hex values)",
                             directive, param1u);

            /* Use the default id. */
            site->id_str = (const char *)ib_uuid_default_str;
            rc = ib_uuid_ascii_to_bin(&site->id, ib_uuid_default_str);

            return rc;
        }

        ib_cfg_log_debug2(cp, "%s: %s", directive, site->id_str);
        return IB_OK;
    }
    else if (strcasecmp("Hostname", directive) == 0) {
        const char *ip = "*";
        const char *port = NULL;
        bool service_specified = false;

        rc = ib_ctxsel_host_create(site, param1u, NULL);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "%s: Invalid hostname \"%s\" for site \"%s\"",
                             directive, param1u, site->id_str);
            return rc;
        }
        ib_cfg_log_debug2(cp, "%s: added hostname %s to site %s",
                          directive, param1u, site->id_str);

        /* Handle ip= and port= for backward compatibility */
        while( (node = ib_list_node_next_const(node)) != NULL) {
            const char *param = (const char *)node->data;
            const char *unescaped;

            rc = core_unescape(ib, (char**)&unescaped, param);
            if ( rc != IB_OK ) {
                ib_cfg_log_debug2(cp, "Failed to unescape %s parameter \"%s\"",
                                  directive, param);
                return rc;
            }

            if (strncasecmp(unescaped, "ip=", 3) == 0) {
                ip = unescaped+3;
                if (*ip == '\0') {
                    ip = "*";
                }
            }
            else if (strncasecmp(unescaped, "port=", 5) == 0) {
                port = unescaped+5;
                if (*port == '\0') {
                    port = NULL;
                }
            }
            else {
                ib_cfg_log_error(cp, "Unhandled %s parameter: \"%s\"",
                                 directive, unescaped);
                return IB_EINVAL;
            }
            service_specified = true;
        }

        if (! service_specified) {
            return IB_OK;
        }

        if (port == NULL) {
            rc = ib_ctxsel_service_create(site, ip, NULL);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp, "%s: Invalid port=\"%s\" for site \"%s\"",
                                 directive, param1u, site->id_str);
                return rc;
            }
        }
        else {
            size_t len = strlen(ip) + 1 + strlen(port) + 1;
            char *service = (char *)ib_mpool_alloc(cp->mp, len);
            if (service == NULL) {
                ib_cfg_log_error(cp, "%s: Failed to allocate service buffer",
                                 directive);
                return IB_EALLOC;
            }

            strcpy(service, ip);
            strcat(service, ":");
            strcat(service, port);
            rc = ib_ctxsel_service_create(site, service, NULL);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp,
                                 "%s: Invalid service \"%s\" for site \"%s\"",
                                 directive, service, site->id_str);
                return rc;
            }
            ib_cfg_log_debug2(cp, "%s: added service %s to site %s",
                              directive, service, site->id_str);
        }

        return IB_OK;
    }
    else if (strcasecmp("Service", directive) == 0) {
        rc = ib_ctxsel_service_create(site, param1u, NULL);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "%s: Invalid service \"%s\" for site \"%s\"",
                             directive, param1u, site->id_str);
            return rc;
        }
        ib_cfg_log_debug2(cp, "%s: added service %s to site %s",
                          directive, param1u, site->id_str);
        return IB_OK;
    }

    ib_cfg_log_error(cp, "Unhandled directive: %s \"%s\"", directive, param1u);
    return IB_EINVAL;
}

/**
 * Handle single parameter directives.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param p1 First parameter
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_param1(ib_cfgparser_t *cp,
                                   const char *name,
                                   const char *p1,
                                   void *cbdata)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);

    ib_engine_t *ib = cp->ib;
    ib_status_t rc;
    ib_core_cfg_t *corecfg;
    const char *p1_unescaped;
    ib_context_t *ctx;

    assert(name != NULL);
    assert(p1 != NULL);

    ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);

    /* We remove constness to populate this buffer. */
    rc = core_unescape(ib, (char**)&p1_unescaped, p1);
    if ( rc != IB_OK ) {
        ib_log_debug2(ib, "Failed to unescape %s=%s", name, p1);
        return rc;
    }

    if (strcasecmp("InspectionEngine", name) == 0) {
        ib_log_debug(ib,
                     "TODO: Handle Directive: %s \"%s\"", name, p1_unescaped);
    }
    else if (strcasecmp("AuditEngine", name) == 0) {
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        if (strcasecmp("RelevantOnly", p1_unescaped) == 0) {
            rc = ib_context_set_num(ctx, "audit_engine", 2);
            return rc;
        }
        else if (strcasecmp("On", p1_unescaped) == 0) {
            rc = ib_context_set_num(ctx, "audit_engine", 1);
            return rc;
        }
        else if (strcasecmp("Off", p1_unescaped) == 0) {
            rc = ib_context_set_num(ctx, "audit_engine", 0);
            return rc;
        }

        ib_log_error(ib,
                     "Failed to parse directive: %s \"%s\"",
                     name,
                     p1_unescaped);
        return IB_EINVAL;
    }
    else if (strcasecmp("AuditLogIndex", name) == 0) {
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);

        /* "None" means do not use the index file at all. */
        if (strcasecmp("None", p1_unescaped) == 0) {
            rc = ib_context_set_auditlog_index(ctx, false, NULL);
            return rc;
        }

        rc = ib_context_set_auditlog_index(ctx, true, p1_unescaped);
        return rc;
    }
    else if (strcasecmp("AuditLogIndexFormat", name) == 0) {
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        rc = ib_context_set_string(ctx, "auditlog_index_fmt", p1_unescaped);
        return rc;
    }
    else if (strcasecmp("AuditLogDirMode", name) == 0) {
        long lmode = strtol(p1_unescaped, NULL, 0);

        if ((lmode > 0777) || (lmode <= 0)) {
            ib_log_error(ib, "Invalid mode: %s \"%s\"", name, p1_unescaped);
            return IB_EINVAL;
        }
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        rc = ib_context_set_num(ctx, "auditlog_dmode", lmode);
        return rc;
    }
    else if (strcasecmp("AuditLogFileMode", name) == 0) {
        ib_num_t mode;
        rc = ib_string_to_num(p1_unescaped, 0, &mode);
        if ( (rc != IB_OK) || (mode > 0777) || (mode <= 0) ) {
            ib_log_error(ib, "Invalid mode: %s \"%s\"", name, p1_unescaped);
            return IB_EINVAL;
        }
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        rc = ib_context_set_num(ctx, "auditlog_fmode", mode);
        return rc;
    }
    else if (strcasecmp("AuditLogBaseDir", name) == 0) {
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        rc = ib_context_set_string(ctx, "auditlog_dir", p1_unescaped);
        return rc;
    }
    else if (strcasecmp("AuditLogSubDirFormat", name) == 0) {
        ib_log_debug2(ib, "%s: \"%s\" ctx=%p", name, p1_unescaped, ctx);
        rc = ib_context_set_string(ctx, "auditlog_sdir_fmt", p1_unescaped);
        return rc;
    }
    /* Set the default block status for responding to blocked transactions. */
    else if (strcasecmp("DefaultBlockStatus", name) == 0) {
        int status;

        rc = ib_context_module_config(ctx, ib_core_module(), (void *)&corecfg);

        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Could not set DefaultBlockStatus %s",
                         p1_unescaped);
            return rc;
        }

        status  = atoi(p1);

        if (!(status <= 200 && status < 600))
        {
            ib_log_debug2(ib,
                          "DefaultBlockStatus must be 200 <= status < 600.");
            ib_log_debug2(ib, "DefaultBlockStatus may not be %d", status);
            return IB_EINVAL;
        }

        corecfg->block_status = status;
        ib_log_debug2(ib, "DefaultBlockStatus: %d", status);
        return IB_OK;
    }
    else if (strcasecmp("Log", name) == 0)
    {
        ib_mpool_t   *mp  = ib_engine_pool_main_get(ib);
        const char   *uri = NULL;

        ib_log_debug2(ib, "%s: \"%s\"", name, p1_unescaped);

        /* Create a file URI from the file path, using memory
         * from the context's mem pool. */
        if ( strstr(p1_unescaped, "://") == NULL )  {
            char *buf = (char *)ib_mpool_alloc( mp, 8+strlen(p1_unescaped) );
            strcpy( buf, "file://" );
            strcat( buf, p1_unescaped );
            uri = buf;
        }
        else if ( strncmp(p1_unescaped, "file://", 7) != 0 ) {
            ib_log_error(ib,
                         "Unsupported URI in %s: \"%s\"",
                         name, p1_unescaped);
            return IB_EINVAL;
        }
        else {
            uri = p1_unescaped;
        }
        ib_log_debug2(ib, "%s: URI=\"%s\"", name, uri);
        rc = ib_context_set_string(ctx, "logger.log_uri", uri);
        return rc;
    }
    else if (strcasecmp("LoadModule", name) == 0) {
        char *absfile;
        ib_module_t *m;

        if (*p1_unescaped == '/') {
            absfile = (char *)p1_unescaped;
        }
        else {
            rc = ib_context_module_config(ctx,
                                          ib_core_module(),
                                          (void *)&corecfg);

            if (rc != IB_OK) {
                return rc;
            }

            rc = core_abs_module_path(ib,
                                      corecfg->module_base_path,
                                      p1_unescaped, &absfile);

            if (rc != IB_OK) {
                return rc;
            }
        }

        rc = ib_module_load(&m, ib, absfile);
        /* ib_module_load will report errors. */
        return rc;
    }
    else if (strcasecmp("RequestBuffering", name) == 0) {
        ib_log_debug2(ib, "%s: %s", name, p1_unescaped);
        if (strcasecmp("On", p1_unescaped) == 0) {
            rc = ib_context_set_num(ctx, "buffer_req", 1);
            return rc;
        }

        rc = ib_context_set_num(ctx, "buffer_req", 0);
        return rc;
    }
    else if (strcasecmp("ResponseBuffering", name) == 0) {
        ib_log_debug2(ib, "%s: %s", name, p1_unescaped);
        if (strcasecmp("On", p1_unescaped) == 0) {
            rc = ib_context_set_num(ctx, "buffer_res", 1);
            return rc;
        }

        rc = ib_context_set_num(ctx, "buffer_res", 0);
        return rc;
    }
    else if (strcasecmp("SensorId", name) == 0) {
        union {
            uint64_t uint64;
            uint32_t uint32[2];
        } reduce;

        /* Store the ASCII version for logging */
        ib->sensor_id_str = ib_mpool_strdup(ib_engine_pool_config_get(ib),
                                            p1_unescaped);

        /* Calculate the binary version. */
        rc = ib_uuid_ascii_to_bin(&ib->sensor_id, (const char *)p1_unescaped);
        if (rc != IB_OK) {
            ib_log_error(ib, "Invalid UUID at %s: %s should have "
                         "UUID format "
                         "(xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx where x are"
                         " hex values)",
                         name, p1_unescaped);

            /* Use the default id. */
            ib->sensor_id_str = (const char *)ib_uuid_default_str;
            rc = ib_uuid_ascii_to_bin(&ib->sensor_id, ib_uuid_default_str);

            return rc;
        }

        ib_log_debug2(ib, "%s: %s", name, ib->sensor_id_str);

        /* Generate a 4byte hash id to use it for transaction id generations */
        reduce.uint64 = ib->sensor_id.uint64[0] ^
                        ib->sensor_id.uint64[1];

        ib->sensor_id_hash = reduce.uint32[0] ^
                             reduce.uint32[1];

        return IB_OK;
    }
    else if (strcasecmp("SensorName", name) == 0) {
        ib->sensor_name = ib_mpool_strdup(ib_engine_pool_config_get(ib),
                                          p1_unescaped);
        ib_log_debug2(ib, "%s: %s", name, ib->sensor_name);
        return IB_OK;
    }
    else if (strcasecmp("SensorHostname", name) == 0) {
        ib->sensor_hostname =
            ib_mpool_strdup(ib_engine_pool_config_get(ib), p1_unescaped);
        ib_log_debug2(ib, "%s: %s", name, ib->sensor_hostname);
        return IB_OK;
    }
    else if (strcasecmp("ModuleBasePath", name) == 0) {
        rc = ib_context_module_config(ctx, ib_core_module(), (void *)&corecfg);

        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Could not set ModuleBasePath %s", p1_unescaped);
            return rc;
        }

        corecfg->module_base_path = p1_unescaped;
        ib_log_debug2(ib, "ModuleBasePath: %s", p1_unescaped);
        return IB_OK;
    }
    else if (strcasecmp("RuleBasePath", name) == 0) {
        rc = ib_context_module_config(ctx, ib_core_module(), (void *)&corecfg);

        if (rc != IB_OK) {
            ib_log_error(ib, "Could not set RuleBasePath %s", p1_unescaped);
            return rc;
        }

        corecfg->rule_base_path = p1_unescaped;
        ib_log_debug2(ib, "RuleBasePath: %s", p1_unescaped);
        return IB_OK;

    }

    ib_log_error(ib, "Unhandled directive: %s %s", name, p1_unescaped);
    return IB_EINVAL;
}

/**
 * Handle loglevel directives.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param p1 First parameter
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_loglevel(ib_cfgparser_t *cp,
                                     const char *name,
                                     const char *p1,
                                     void *cbdata)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);
    assert(cbdata != NULL);

    ib_engine_t *ib = cp->ib;
    ib_status_t rc;
    const ib_strval_t *map = (const ib_strval_t *)cbdata;
    ib_context_t *ctx;
    ib_num_t level;
    long tmp = 0;

    assert(name != NULL);
    assert(p1 != NULL);

    ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);

    if (sscanf(p1, "%ld", &tmp) != 0) {
        level = tmp;
    }
    else {
        rc = ib_config_strval_pair_lookup(p1, map, &level);
        if (rc != IB_OK) {
            return IB_EUNKNOWN;
        }
    }

    if (strcasecmp("LogLevel", name) == 0)
    {
        ib_log_debug2(ib, "%s: %u", name, (unsigned int)level);
        rc = ib_context_set_num(ctx, "logger.log_level", level);
        return rc;
    }
    else if (strcasecmp("RuleEngineLogLevel", name) == 0) {
        ib_log_debug2(ib, "%s: %u", name, (unsigned int)level);
        rc = ib_context_set_num(ctx, "rule_log_level", level);
        return rc;
    }

    ib_log_error(ib, "Unhandled directive: %s %s", name, p1);
    return IB_EINVAL;
}

/**
 * Handle single parameter directives.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param flags Flags
 * @param fmask Flags mask (which bits were actually set)
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_auditlogparts(ib_cfgparser_t *cp,
                                          const char *name,
                                          ib_flags_t flags,
                                          ib_flags_t fmask,
                                          void *cbdata)
{
    ib_engine_t *ib = cp->ib;
    ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
    ib_num_t parts;
    ib_status_t rc;

    rc = ib_context_get(ctx, "auditlog_parts", ib_ftype_num_out(&parts), NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Merge the set flags with the previous value. */
    parts = (flags & fmask) | (parts & ~fmask);

    ib_log_debug2(ib, "AUDITLOG PARTS: 0x%08lu", (unsigned long)parts);

    rc = ib_context_set_num(ctx, "auditlog_parts", parts);
    return rc;
}

/**
 * Handle single parameter directives.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param flags Flags
 * @param fmask Flags mask (which bits were actually set)
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_rulelog_data(ib_cfgparser_t *cp,
                                         const char *name,
                                         ib_flags_t flags,
                                         ib_flags_t fmask,
                                         void *cbdata)
{
    ib_engine_t *ib = cp->ib;
    ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
    ib_num_t tmp;
    ib_flags_t log_flags;
    ib_status_t rc;

    rc = ib_context_get(ctx, "rule_log_flags", ib_ftype_num_out(&tmp), NULL);
    if (rc != IB_OK) {
        return rc;
    }
    log_flags = tmp;

    /* Merge the set flags with the previous value. */
    log_flags = (flags & fmask) | (log_flags & ~fmask);

    ib_log_debug2(ib, "RULE ENGINE LOG FLAGS: 0x%08lx",
                  (unsigned long)log_flags);

    rc = ib_context_set_num(ctx, "rule_log_flags", log_flags);
    return rc;
}

/**
 * Handle InspectionEngineOptions directive.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param flags Flags
 * @param fmask Flags mask (which bits were actually set)
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_inspection_engine_options(ib_cfgparser_t *cp,
                                                      const char *name,
                                                      ib_flags_t flags,
                                                      ib_flags_t fmask,
                                                      void *cbdata)
{
    ib_engine_t *ib = cp->ib;
    ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
    ib_num_t options = 0;
    ib_status_t rc;

    rc = ib_context_get(ctx, "inspection_engine_options",
                        ib_ftype_num_out(&options), NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Merge the set flags with the previous value. */
    options = (flags & fmask) | (options & ~fmask);

    ib_log_debug2(ib, "INSPECTION_ENGINE_OPTIONS: 0x%08lx", (unsigned long)options);

    rc = ib_context_set_num(ctx, "inspection_engine_options", options);
    return rc;
}

/**
 * Parse a InitCollection directive.
 *
 * Register a InitCollection directive to the engine.
 *
 * @param[in] cp Configuration parser
 * @param[in] directive The directive name.
 * @param[in] vars The list of variables passed to @c name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t core_dir_initcollection(ib_cfgparser_t *cp,
                                           const char *directive,
                                           const ib_list_t *vars,
                                           void *cbdata)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(vars != NULL);

    ib_status_t              rc;
    ib_mpool_t              *mp;
    const ib_list_node_t    *node;
    ib_list_node_t          *mcnode;
    ib_list_t               *params;
    const char              *collection_name;
    const char              *collection_uri;
    ib_core_cfg_t           *cfg;
    ib_managed_collection_t *collection = NULL;
    bool                     new_collection = false;
    ib_list_t               *managers_debug = NULL;

    mp = ib_engine_pool_config_get(cp->ib);

    /* Get the configuration */
    rc = ib_context_module_config(cp->cur_ctx, ib_core_module(), (void *)&cfg);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get core module configuration: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the context's managed collection list on the first run */
    if (cfg->mancoll_list == NULL) {
        rc = ib_list_create(&(cfg->mancoll_list), mp);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "%s: Failed to create list: %s",
                             directive, ib_status_to_string(rc));
            return rc;
        }
    }

    /* Get the collection name string */
    node = ib_list_first_const(vars);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp, " %s: No collection name specified", directive);
        return IB_EINVAL;
    }
    collection_name = (const char *)(node->data);

    /* The next node is the URI */
    node = ib_list_node_next_const(node);
    if ( (node == NULL) || (node->data == NULL) ) {
        ib_cfg_log_error(cp, " %s: No collection URI specified", directive);
        return IB_EINVAL;
    }
    collection_uri = (const char *)(node->data);

    /* Parameters */
    rc = ib_list_create(&params, mp);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, " %s: Error allocation parameter list", directive);
        return IB_EINVAL;
    }

    /* Loop through the remaining parameters */
    while( (node = ib_list_node_next_const(node)) != NULL) {
        const char *nodestr = (const char *)node->data;
        rc = ib_list_push(params, (char *)nodestr);
        if (rc != IB_OK) {
            break;
        }
    }

    /* Check if this collection name is already registered */
    IB_LIST_LOOP(cfg->mancoll_list, mcnode) {
        ib_managed_collection_t *mc = (ib_managed_collection_t *)mcnode->data;
        if (strcmp(mc->collection_name, collection_name) == 0) {
            collection = mc;
            break;
        }
    }

    /* Create a new collection if required */
    if (collection == NULL) {
        rc = ib_managed_collection_create(cp->ib, mp,
                                          collection_name,
                                          &collection);
        if (rc != IB_OK) {
            return rc;
        }
        new_collection = true;
    }

    /* For logging, create a list of manager objects */
    if (ib_log_get_level(cp->ib) >= IB_LOG_DEBUG) {
        ib_list_create(&managers_debug, mp);
    }

    /* Select a collection manager */
    rc = ib_managed_collection_select(cp->ib, mp,
                                      collection_name,
                                      collection_uri,
                                      params,
                                      collection,
                                      managers_debug);
    if (rc == IB_ENOENT) {
        ib_cfg_log_error(cp,
                         "%s: No matching collection manager found for \"%s\"",
                         directive, collection_name);
        return IB_EINVAL;
    }
    else if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "%s: Failed to create managed collection \"%s\": %s",
                         directive, collection_name, ib_status_to_string(rc));
        return rc;
    }

    /* Add the collection (if it's new) to the managed collection list */
    if (new_collection) {
        rc = ib_list_push(cfg->mancoll_list, collection);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "%s: Error adding managed collection to list: %s",
                             directive, ib_status_to_string(rc));
            return rc;
        }
    }

    if (managers_debug != NULL) {
        IB_LIST_LOOP_CONST(managers_debug, node) {
            const ib_collection_manager_t *manager =
                (const ib_collection_manager_t *)node->data;
            ib_cfg_log_debug(cp,
                             "%s: %s collection \"%s\" managed by \"%s\" "
                             "for context \"%s\"",
                             directive,
                             new_collection ? "New" : "Existing",
                             collection_name,
                             ib_collection_manager_name(manager),
                             ib_context_full_get(cp->cur_ctx));
        }
    }

    /* Done */
    return IB_OK;
}

/**
 * Perform any extra duties when certain config parameters are "Set".
 *
 * @param cp Config parser
 * @param ctx Context
 * @param type Config parameter type
 * @param name Config parameter name
 * @param val Config parameter value
 *
 * @returns Status code
 */
static ib_status_t core_set_value(ib_cfgparser_t *cp,
                                  ib_context_t *ctx,
                                  ib_ftype_t type,
                                  const char *name,
                                  const char *val)
{
    ib_engine_t *ib = ctx->ib;
    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    /* Get the core module config. */
    rc = ib_context_module_config(ib->ctx, ib_core_module(),
                                  (void *)&corecfg);
    if (rc != IB_OK) {
        corecfg = &core_global_cfg;
    }

    if (strcasecmp("parser", name) == 0) {
        ib_provider_inst_t *pi;

        if (strcmp(MODULE_NAME_STR, corecfg->parser) == 0) {
            return IB_OK;
        }
        /* Lookup/set parser provider instance. */
        rc = ib_provider_instance_create(ib, IB_PROVIDER_TYPE_PARSER,
                                         val, &pi,
                                         ib->mp, NULL);
        if (rc != IB_OK) {
            ib_cfg_log_alert(cp, "Failed to create %s provider instance: %s",
                             IB_PROVIDER_TYPE_PARSER, ib_status_to_string(rc));
            return rc;
        }

        rc = ib_parser_provider_set_instance(ctx, pi);
        if (rc != IB_OK) {
            ib_cfg_log_alert(cp, "Failed to set %s provider instance: %s",
                             IB_PROVIDER_TYPE_PARSER, ib_status_to_string(rc));
            return rc;
        }
    }
    else if (strcasecmp("audit", name) == 0) {
        /* Lookup the audit log provider. */
        rc = ib_provider_lookup(ib,
                                IB_PROVIDER_TYPE_AUDIT,
                                val,
                                &corecfg->pr.audit);
        if (rc != IB_OK) {
            ib_cfg_log_alert(cp, "Failed to lookup %s audit log provider: %s",
                             val, ib_status_to_string(rc));
            return rc;
        }
    }
    else if (strcasecmp("RuleEngineDebugLogLevel", name) == 0) {
        rc = ib_rule_engine_set(cp, name, val);
        if (rc != IB_OK) {
            return rc;
        }
    }

    else {
        return IB_EINVAL;
    }

    return IB_OK;
}


/**
 * Handle two parameter directives.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param p1 First parameter
 * @param p2 Second parameter
 * @param cbdata Callback data (from directive registration)
 *
 * @returns Status code
 */
static ib_status_t core_dir_param2(ib_cfgparser_t *cp,
                                   const char *name,
                                   const char *p1,
                                   const char *p2,
                                   void *cbdata)
{
    ib_engine_t *ib = cp->ib;
    ib_status_t rc;

    if (strcasecmp("Set", name) == 0) {
        ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
        void *val;
        ib_ftype_t type;

        ib_context_get(ctx, p1, &val, &type);
        switch(type) {
            case IB_FTYPE_NULSTR:
                ib_context_set_string(ctx, p1, p2);
                break;
            case IB_FTYPE_NUM:
                ib_context_set_num(ctx, p1, atol(p2));
                break;
            default:
                ib_log_error(ib,
                             "Can only set string(%d) or numeric(%d) "
                             "types, but %s was type=%d",
                             IB_FTYPE_NULSTR, IB_FTYPE_NUM,
                             p1, type);
                return IB_EINVAL;
        }

        rc = core_set_value(cp, ctx, type, p1, p2);
        return rc;
    }

    ib_log_error(ib, "Unhandled directive: %s %s %s", name, p1, p2);
    return IB_EINVAL;
}

/**
 * Parse a InitVar directive.
 *
 * Register a InitVar directive to the engine.
 *
 * @param[in] cp Configuration parser
 * @param[in] directive The directive name.
 * @param[in] name 1st parameter to InitVar
 * @param[in] value 2nd parameter to InitVar
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t core_dir_initvar(ib_cfgparser_t *cp,
                                    const char *directive,
                                    const char *name,
                                    const char *value,
                                    void *cbdata)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(name != NULL);
    assert(value != NULL);

    ib_status_t rc;
    ib_mpool_t *mp = cp->cur_ctx->mp;
    ib_core_cfg_t *corecfg;
    ib_field_t *field;
    ib_field_val_union_t fval;

    /* Get the core module config. */
    rc = ib_context_module_config(cp->cur_ctx, ib_core_module(),
                                  (void *)&corecfg);
    if (rc != IB_OK) {
        corecfg = &core_global_cfg;
    }

    /* Initialize the fields list */
    if (corecfg->initvar_list == NULL) {
        rc = ib_list_create(&(corecfg->initvar_list), mp);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Failed to create InitVar directive list: %s",
                             ib_status_to_string(rc));
            return rc;
        }
    }

    /* Create the field based on whether the value looks like a number or not */
    rc = ib_field_from_string(mp, IB_FIELD_NAME(name), value, &field, &fval);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error creating field for InitVar: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Add the field to the list */
    rc = ib_list_push(corecfg->initvar_list, field);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "InitVar: Error pushing value on list: %s",
                         ib_status_to_string(rc));
        return rc;
    }
    if (field->type == IB_FTYPE_NUM) {
        ib_cfg_log_debug(cp,
                         "InitVar: Created numeric field \"%s\" %"PRId64" "
                         "for context \"%s\"",
                         name, fval.num,
                         ib_context_full_get(cp->cur_ctx));
    }
    else if (field->type == IB_FTYPE_FLOAT) {
        ib_cfg_log_debug(cp,
                         "InitVar: Created float field \"%s\" %Lf "
                         "for context \"%s\"",
                         name, fval.fnum,
                         ib_context_full_get(cp->cur_ctx));
    }
    else {
        ib_cfg_log_debug(cp,
                         "InitVar:Created string field \"%s\" \"%s\" "
                         "for context \"%s\"",
                         name, fval.nulstr, ib_context_full_get(cp->cur_ctx));
    }

    /* Done */
    return IB_OK;
}


/**
 * Mapping of valid debug log levels to numerical value
 */
static IB_STRVAL_MAP(core_loglevels_map) = {
    IB_STRVAL_PAIR("emergency", IB_LOG_EMERGENCY),
    IB_STRVAL_PAIR("alert", IB_LOG_ALERT),
    IB_STRVAL_PAIR("critical", IB_LOG_CRITICAL),
    IB_STRVAL_PAIR("error", IB_LOG_ERROR),
    IB_STRVAL_PAIR("warning", IB_LOG_WARNING),
    IB_STRVAL_PAIR("notice", IB_LOG_NOTICE),
    IB_STRVAL_PAIR("info", IB_LOG_INFO),
    IB_STRVAL_PAIR("debug", IB_LOG_DEBUG),
    IB_STRVAL_PAIR("debug2", IB_LOG_DEBUG2),
    IB_STRVAL_PAIR("debug3", IB_LOG_DEBUG3),
    IB_STRVAL_PAIR("trace", IB_LOG_TRACE),
    IB_STRVAL_PAIR_LAST
};

/**
 * Mapping of valid audit log part names to flag values.
 */
static IB_STRVAL_MAP(core_auditlog_parts_map) = {
    /* Auditlog Part Groups */
    IB_STRVAL_PAIR("none", 0),
    IB_STRVAL_PAIR("minimal", IB_ALPART_HEADER|IB_ALPART_EVENTS),
    IB_STRVAL_PAIR("all", IB_ALPARTS_ALL),
    IB_STRVAL_PAIR("debug", IB_ALPART_DEBUG_FIELDS),
    IB_STRVAL_PAIR("default", IB_ALPARTS_DEFAULT),
    IB_STRVAL_PAIR("request", IB_ALPARTS_REQUEST),
    IB_STRVAL_PAIR("response", IB_ALPARTS_RESPONSE),

    /* AuditLog Individual Parts */
    IB_STRVAL_PAIR("header", IB_ALPART_HEADER),
    IB_STRVAL_PAIR("events", IB_ALPART_EVENTS),
    IB_STRVAL_PAIR("requestmetadata", IB_ALPART_HTTP_REQUEST_METADATA),
    IB_STRVAL_PAIR("requestheader", IB_ALPART_HTTP_REQUEST_HEADER),
    IB_STRVAL_PAIR("requestbody", IB_ALPART_HTTP_REQUEST_BODY),
    IB_STRVAL_PAIR("requesttrailer", IB_ALPART_HTTP_REQUEST_TRAILER),
    IB_STRVAL_PAIR("responsemetadata", IB_ALPART_HTTP_RESPONSE_METADATA),
    IB_STRVAL_PAIR("responseheader", IB_ALPART_HTTP_RESPONSE_HEADER),
    IB_STRVAL_PAIR("responsebody", IB_ALPART_HTTP_RESPONSE_BODY),
    IB_STRVAL_PAIR("responsetrailer", IB_ALPART_HTTP_RESPONSE_TRAILER),
    IB_STRVAL_PAIR("debugfields", IB_ALPART_DEBUG_FIELDS),

    /* End */
    IB_STRVAL_PAIR_LAST
};

/**
 * Mapping of valid rule logging names to flag values.
 */
static IB_STRVAL_MAP(core_rulelog_flags_map) = {
    /* Rule log Flag Groups */
    IB_STRVAL_PAIR("all", IB_RULE_LOG_FLAGS_ALL),
    IB_STRVAL_PAIR("request", IB_RULE_LOG_FLAGS_REQUEST),
    IB_STRVAL_PAIR("response", IB_RULE_LOG_FLAGS_RESPONSE),
    IB_STRVAL_PAIR("ruleExec", IB_RULE_LOG_FLAGS_EXEC),

    /* Rule log Individual flags */
    IB_STRVAL_PAIR("tx", IB_RULE_LOG_FLAG_TX),
    IB_STRVAL_PAIR("requestLine", IB_RULE_LOG_FLAG_REQ_LINE),
    IB_STRVAL_PAIR("requestHeader", IB_RULE_LOG_FLAG_REQ_HEADER),
    IB_STRVAL_PAIR("requestBody", IB_RULE_LOG_FLAG_REQ_BODY),
    IB_STRVAL_PAIR("responseLine", IB_RULE_LOG_FLAG_RSP_LINE),
    IB_STRVAL_PAIR("responseHeader", IB_RULE_LOG_FLAG_RSP_HEADER),
    IB_STRVAL_PAIR("responseBody", IB_RULE_LOG_FLAG_RSP_BODY),
    IB_STRVAL_PAIR("phase", IB_RULE_LOG_FLAG_PHASE),
    IB_STRVAL_PAIR("rule", IB_RULE_LOG_FLAG_RULE),
    IB_STRVAL_PAIR("target", IB_RULE_LOG_FLAG_TARGET),
    IB_STRVAL_PAIR("transformation", IB_RULE_LOG_FLAG_TFN),
    IB_STRVAL_PAIR("operator", IB_RULE_LOG_FLAG_OPERATOR),
    IB_STRVAL_PAIR("action", IB_RULE_LOG_FLAG_ACTION),
    IB_STRVAL_PAIR("event", IB_RULE_LOG_FLAG_EVENT),
    IB_STRVAL_PAIR("audit", IB_RULE_LOG_FLAG_AUDIT),
    IB_STRVAL_PAIR("timing", IB_RULE_LOG_FLAG_TIMING),

    IB_STRVAL_PAIR("allRules", IB_RULE_LOG_FILT_ALL),
    IB_STRVAL_PAIR("actionableRulesOnly", IB_RULE_LOG_FILT_ACTIONABLE),
    IB_STRVAL_PAIR("operatorExecOnly", IB_RULE_LOG_FILT_OPEXEC),
    IB_STRVAL_PAIR("operatorErrorOnly", IB_RULE_LOG_FILT_ERROR),
    IB_STRVAL_PAIR("returnedTrueOnly", IB_RULE_LOG_FILT_TRUE),
    IB_STRVAL_PAIR("returnedFalseOnly", IB_RULE_LOG_FILT_FALSE),

    /* End */
    IB_STRVAL_PAIR_LAST
};

/**
 * Mapping of valid inspection engine options
 */
static IB_STRVAL_MAP(core_inspection_engine_options_map) = {
    /* Inspection Engine Options Groups */
    IB_STRVAL_PAIR("none", 0),
    IB_STRVAL_PAIR("all", IB_IEOPT_ALL),
    IB_STRVAL_PAIR("default", IB_IEOPT_DEFAULT),
    IB_STRVAL_PAIR("request", IB_IEOPT_REQUEST),
    IB_STRVAL_PAIR("response", IB_IEOPT_RESPONSE),

    /* Individual Inspection Engine Options */
    IB_STRVAL_PAIR("requestheader", IB_IEOPT_REQUEST_HEADER),
    IB_STRVAL_PAIR("requestbody", IB_IEOPT_REQUEST_BODY),
    IB_STRVAL_PAIR("responseheader", IB_IEOPT_RESPONSE_HEADER),
    IB_STRVAL_PAIR("responsebody", IB_IEOPT_RESPONSE_BODY),

    /* End */
    IB_STRVAL_PAIR_LAST
};

/**
 * Directive initialization structure.
 */
static IB_DIRMAP_INIT_STRUCTURE(core_directive_map) = {
    /* Modules */
    IB_DIRMAP_INIT_PARAM1(
        "LoadModule",
        core_dir_param1,
        NULL
    ),

    /* Parameters */
    IB_DIRMAP_INIT_PARAM2(
        "Set",
        core_dir_param2,
        NULL
    ),

    /* Sensor */
    IB_DIRMAP_INIT_PARAM1(
        "SensorId",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "SensorName",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "SensorHostname",
        core_dir_param1,
        NULL
    ),

    /* Buffering */
    IB_DIRMAP_INIT_PARAM1(
        "RequestBuffering",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "ResponseBuffering",
        core_dir_param1,
        NULL
    ),

    /* Blocking */
    IB_DIRMAP_INIT_PARAM1(
        "DefaultBlockStatus",
        core_dir_param1,
        NULL
    ),

    /* Logging */
    IB_DIRMAP_INIT_PARAM1(
        "LogLevel",
        core_dir_loglevel,
        core_loglevels_map
    ),
    IB_DIRMAP_INIT_PARAM1(
        "Log",
        core_dir_param1,
        NULL
    ),

    /* Config */
    IB_DIRMAP_INIT_SBLK1(
        "Site",
        core_dir_site_start,
        core_dir_site_end,
        NULL,
        NULL
    ),
    IB_DIRMAP_INIT_LIST(
        "SiteId",
        core_dir_site_list,
        NULL
    ),
    IB_DIRMAP_INIT_SBLK1(
        "Location",
        core_dir_loc_start,
        core_dir_loc_end,
        NULL,
        NULL
    ),
    IB_DIRMAP_INIT_LIST(
        "Hostname",
        core_dir_site_list,
        NULL
    ),
    IB_DIRMAP_INIT_LIST(
        "Service",
        core_dir_site_list,
        NULL
    ),

    /* Inspection Engine */
    IB_DIRMAP_INIT_PARAM1(
        "InspectionEngine",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_OPFLAGS(
        "InspectionEngineOptions",
        core_dir_inspection_engine_options,
        NULL,
        core_inspection_engine_options_map
    ),

    /* Audit Engine */
    IB_DIRMAP_INIT_PARAM1(
        "AuditEngine",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogIndex",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogIndexFormat",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogBaseDir",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogSubDirFormat",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogDirMode",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "AuditLogFileMode",
        core_dir_param1,
        NULL
    ),
    IB_DIRMAP_INIT_OPFLAGS(
        "AuditLogParts",
        core_dir_auditlogparts,
        NULL,
        core_auditlog_parts_map
    ),

    /* Search Paths - Modules */
    IB_DIRMAP_INIT_PARAM1(
        "ModuleBasePath",
        core_dir_param1,
        NULL
    ),

    /* Search Paths - Rules */
    IB_DIRMAP_INIT_PARAM1(
        "RuleBasePath",
        core_dir_param1,
        NULL
    ),

    /* Rule logging data */
    IB_DIRMAP_INIT_OPFLAGS(
        "RuleEngineLogData",
        core_dir_rulelog_data,
        NULL,
        core_rulelog_flags_map
    ),
    IB_DIRMAP_INIT_PARAM1(
        "RuleEngineLogLevel",
        core_dir_loglevel,
        core_loglevels_map
    ),

    /* TX DPI Initializers */
    IB_DIRMAP_INIT_PARAM2(
        "InitVar",
        core_dir_initvar,
        NULL
    ),

    /* TX Initialized Collection */
    IB_DIRMAP_INIT_LIST(
        "InitCollection",
        core_dir_initcollection,
        NULL
    ),

    /* End */
    IB_DIRMAP_INIT_LAST
};


/* -- Module Routines -- */

/**
 * Logger for util logger.
 **/
static void core_util_logger(
    void *ib, int level,
    const char *file, int line,
    const char *fmt, va_list ap
)
{
    ib_log_vex_ex((ib_engine_t *)ib, level, file, line, fmt, ap);

    return;
}

/**
 * Initialize the core module on load.
 *
 * @param[in] ib Engine
 * @param[in] m Module
 * @param[in] cbdata Callback data (unused)
 *
 * @returns Status code
 */
static ib_status_t core_init(ib_engine_t *ib,
                             ib_module_t *m,
                             void        *cbdata)
{
    ib_core_cfg_t *corecfg;
    ib_provider_t *core_audit_provider;
    ib_core_module_data_t *core_data;
    ib_provider_inst_t *parser;
    ib_filter_t *fbuffer;
    ib_status_t rc;

    /* Get the core module config. */
    rc = ib_context_module_config(ib->ctx, m, (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to fetch core module config: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Set defaults */
    corecfg->log_level            = 4;
    corecfg->log_uri              = "";
    corecfg->parser               = MODULE_NAME_STR;
    corecfg->buffer_req           = 0;
    corecfg->buffer_res           = 0;
    corecfg->audit_engine         = 2; // TODO: enum these
    corecfg->auditlog_dmode       = 0700;
    corecfg->auditlog_fmode       = 0600;
    corecfg->auditlog_parts       = IB_ALPARTS_DEFAULT;
    corecfg->auditlog_dir         = "/var/log/ironbee";
    corecfg->auditlog_sdir_fmt    = "";
    corecfg->auditlog_index_fmt   = IB_LOGFORMAT_DEFAULT;
    corecfg->audit                = MODULE_NAME_STR;
    corecfg->data                 = MODULE_NAME_STR;
    corecfg->module_base_path     = X_MODULE_BASE_PATH;
    corecfg->rule_base_path       = X_RULE_BASE_PATH;
    corecfg->rule_log_flags       = IB_RULE_LOG_FLAGS_EXEC;
    corecfg->rule_log_level       = IB_LOG_INFO;
    corecfg->rule_debug_str       = "error";
    corecfg->rule_debug_level     = IB_RULE_DLOG_ERROR;
    corecfg->block_status         = 403;
    corecfg->inspection_engine_options = IB_IEOPT_DEFAULT;

    /* Register logger functions. */
    ib_log_set_logger_fn(ib, core_vlogmsg, NULL);
    ib_log_set_loglevel_fn(ib, core_loglevel, NULL);

    /* Force any IBUtil calls to use the default logger */
    rc = ib_util_log_logger(core_util_logger, ib);
    if (rc != IB_OK) {
        return rc;
    }

    /* Define the audit provider API. */
    rc = ib_provider_define(ib, IB_PROVIDER_TYPE_AUDIT,
                            audit_register, &audit_api);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the core audit provider. */
    rc = ib_provider_register(ib, IB_PROVIDER_TYPE_AUDIT,
                              MODULE_NAME_STR, &core_audit_provider,
                              &core_audit_iface,
                              NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Define the parser provider API. */
    rc = ib_provider_define(ib, IB_PROVIDER_TYPE_PARSER,
                            parser_register, NULL);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to define parser provider: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Filter/Buffer */
    rc = ib_filter_register(&fbuffer,
                            ib,
                            "core-buffer",
                            IB_FILTER_TX,
                            IB_FILTER_OBUF,
                            filter_buffer,
                            NULL);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to register buffer filter: %s", ib_status_to_string(rc));
        return rc;
    }
    ib_hook_tx_register(ib, handle_context_tx_event,
                        filter_ctl_config, fbuffer);

    /* Register hooks. */
    ib_hook_tx_register(ib, handle_context_tx_event,
                        core_hook_context_tx, NULL);
    ib_hook_conn_register(ib, conn_started_event, core_hook_conn_started, NULL);
    ib_hook_tx_register(ib, tx_started_event, core_hook_tx_started, NULL);
    ib_hook_tx_register(ib, handle_logging_event, core_hook_logging, NULL);
    /*
     * @todo Need the parser to parse the header before context, but others
     * after context so that the personality can change based on the header
     * (Host, uri path, etc)
     */
    /*
     * ib_hook_register(ib, handle_context_tx_event,
     *                  (void *)parser_hook_req_header, NULL);
     */

    /* Register auditlog body buffering hooks. */
    ib_hook_txdata_register(ib, request_body_data_event,
                            core_hook_request_body_data, NULL);

    ib_hook_txdata_register(ib, response_body_data_event,
                            core_hook_response_body_data, NULL);

    /* Register logevent hooks. */
    ib_hook_tx_register(ib, handle_logging_event,
                        logevent_hook_logging, NULL);

    /* Create core data structure */
    core_data = ib_mpool_calloc(ib->mp, sizeof(*core_data), 1);
    if (core_data == NULL) {
        ib_log_error(ib, "Failed to allocate memory for core module");
        return IB_EALLOC;
    }
    m->data = (void *)core_data;

    /* Register context selection hooks, etc. */
    rc = ib_core_ctxsel_init(ib, m);
    if (rc != IB_OK) {
        return rc;
    }

    /* Define the matcher provider API */
    rc = ib_provider_define(ib, IB_PROVIDER_TYPE_MATCHER,
                            matcher_register, &matcher_api);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to define matcher provider: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Lookup the core audit log provider. */
    rc = ib_provider_lookup(ib,
                            IB_PROVIDER_TYPE_AUDIT,
                            IB_DSTR_CORE,
                            &corecfg->pr.audit);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to lookup %s audit log provider: %s",
                     IB_DSTR_CORE, ib_status_to_string(rc));
        return rc;
    }

    /* Lookup/set default parser provider if not the "core" parser. */
    if (strcmp(MODULE_NAME_STR, corecfg->parser) != 0) {
        rc = ib_provider_instance_create(ib, IB_PROVIDER_TYPE_PARSER,
                                         corecfg->parser, &parser,
                                         ib->mp, NULL);
        if (rc != IB_OK) {
            ib_log_alert(ib, "Failed to create %s provider instance: %s",
                         IB_DSTR_CORE, ib_status_to_string(rc));
            return rc;
        }
        ib_parser_provider_set_instance(ib->ctx, parser);
    }

    /* Initialize the core fields */
    rc = ib_core_fields_init(ib, m);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to initialize core fields: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the rule engine */
    rc = ib_rule_engine_init(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize rule engine: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the core transformations */
    rc = ib_core_transformations_init(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize core operators: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the core operators */
    rc = ib_core_operators_init(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize core operators: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the core actions */
    rc = ib_core_actions_init(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize core actions: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the managed collection logic */
    rc = ib_managed_collection_init(ib);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize managed collections: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the core collection managers */
    rc = ib_core_collection_managers_register(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to register core collection managers: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Shutdown the core module on exit.
 *
 * @param[in] ib Engine
 * @param[in] m Module
 * @param[in] cbdata Callback data (unused)
 *
 * @returns Status code
 */
static
ib_status_t core_finish(
    ib_engine_t *ib,
    ib_module_t *m,
    void        *cbdata
)
{
    ib_status_t rc;

    /* Shut down the core collection managers */
    rc = ib_core_collection_managers_finish(ib, m);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to shut down core collection managers: %s",
                     ib_status_to_string(rc));
    }

    /* Shut down the managed collection logic */
    rc = ib_managed_collection_finish(ib);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to initialize managed collections: %s",
                     ib_status_to_string(rc));
    }

    return IB_OK;
}

/**
 * Core module configuration parameter initialization structure.
 */
static IB_CFGMAP_INIT_STRUCTURE(core_config_map) = {
    /* Logger */
    IB_CFGMAP_INIT_ENTRY(
        "logger.log_level",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        log_level
    ),
    IB_CFGMAP_INIT_ENTRY(
        "logger.log_uri",
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        log_uri
    ),

    /* Rule logging */
    IB_CFGMAP_INIT_ENTRY(
        "rule_log_flags",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        rule_log_flags
    ),
    IB_CFGMAP_INIT_ENTRY(
        "rule_log_level",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        rule_log_level
    ),
    IB_CFGMAP_INIT_ENTRY(
        "RuleEngineDebugLogLevel",
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        rule_debug_str
    ),
    IB_CFGMAP_INIT_ENTRY(
        "_RuleEngineDebugLevel",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        rule_debug_level
    ),

    /* Parser */
    IB_CFGMAP_INIT_ENTRY(
        IB_PROVIDER_TYPE_PARSER,
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        parser
    ),

    /* Buffering */
    IB_CFGMAP_INIT_ENTRY(
        "buffer_req",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        buffer_req
    ),
    IB_CFGMAP_INIT_ENTRY(
        "buffer_res",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        buffer_res
    ),

    /* Audit Log */
    IB_CFGMAP_INIT_ENTRY(
        "audit_engine",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        audit_engine
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_dmode",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        auditlog_dmode
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_fmode",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        auditlog_fmode
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_parts",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        auditlog_parts
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_dir",
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        auditlog_dir
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_sdir_fmt",
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        auditlog_sdir_fmt
    ),
    IB_CFGMAP_INIT_ENTRY(
        "auditlog_index_fmt",
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        auditlog_index_fmt
    ),
    IB_CFGMAP_INIT_ENTRY(
        IB_PROVIDER_TYPE_AUDIT,
        IB_FTYPE_NULSTR,
        ib_core_cfg_t,
        audit
    ),

    IB_CFGMAP_INIT_ENTRY(
        "inspection_engine_options",
        IB_FTYPE_NUM,
        ib_core_cfg_t,
        inspection_engine_options
    ),

    /* End */
    IB_CFGMAP_INIT_LAST
};

ib_module_t *ib_core_module(void)
{
    return IB_MODULE_STRUCT_PTR;
}

/**
 * Handle context open events for the core module
 *
 * @param ib Engine
 * @param mod Module
 * @param ctx Context
 * @param cbdata Callback data (unused)
 *
 * @returns Status code
 */
static ib_status_t core_ctx_open(ib_engine_t  *ib,
                                 ib_module_t  *mod,
                                 ib_context_t *ctx,
                                 void         *cbdata)
{
    ib_status_t rc;

    /* Initialize the core fields context. */
    rc = ib_core_fields_ctx_init(ib, mod, ctx, cbdata);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to initialize core fields: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Inform the rule engine of the context open */
    rc = ib_rule_engine_ctx_open(ib, mod, ctx);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Rule engine failed to open context: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Handle context close events for the core module
 *
 * @param ib Engine
 * @param mod Module
 * @param ctx Context
 * @param cbdata Callback data (unused)
 *
 * @returns Status code
 */
static ib_status_t core_ctx_close(ib_engine_t  *ib,
                                  ib_module_t  *mod,
                                  ib_context_t *ctx,
                                  void         *cbdata)
{
    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    /* Initialize the rule engine for the context */
    rc = ib_rule_engine_ctx_close(ib, mod, ctx);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to close rule engine context: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Get the current context config. */
    rc = ib_context_module_config(ctx, mod, (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib,
                     "Failed to fetch core module context config: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Build the site selection list at the close of the main context */
    if (ib_context_type(ctx) == IB_CTYPE_MAIN) {
        rc = ib_ctxsel_finalize( ib );
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

static ib_status_t core_ctx_managed_collection_destroy(
    ib_engine_t *ib,
    ib_module_t *mod,
    ib_context_t *ctx,
    ib_core_cfg_t *corecfg)
{
    assert(ib != NULL);
    assert(mod != NULL);
    assert(ctx != NULL);
    assert(corecfg != NULL);

    const ib_list_node_t *node;
    ib_status_t rc = IB_OK;

    /* If there are no collections, do nothing */
    if (corecfg->mancoll_list == NULL) {
        return IB_OK;
    }

    /* Walk through the list of collections & destroy. */
    IB_LIST_LOOP_CONST(corecfg->mancoll_list, node) {
        const ib_managed_collection_t *collection =
            (const ib_managed_collection_t *)node->data;
        ib_status_t tmprc;

        tmprc = ib_managed_collection_destroy(ib, collection);
        if (tmprc != IB_OK) {
            ib_log_warning(ib,
                           "Error creating managed collection \"%s\": %s",
                           collection->collection_name,
                           ib_status_to_string(tmprc));
            if (rc == IB_OK) {
                rc = tmprc;
            }
        }
        else {
            ib_log_trace(ib, "Unregistered managed collection \"%s\"",
                         collection->collection_name);
        }
    }
    ib_list_clear(corecfg->mancoll_list);

    return rc;
}

/**
 * Close the core module context
 *
 * @param ib Engine
 * @param mod Module
 * @param ctx Context
 * @param cbdata Callback data (unused)
 *
 * @returns Status code
 */
static ib_status_t core_ctx_destroy(ib_engine_t *ib,
                                    ib_module_t *mod,
                                    ib_context_t *ctx,
                                    void *cbdata)
{
    ib_core_cfg_t *config;
    ib_core_cfg_t *main_config;
    ib_status_t rc;

    /* Get the current context config. */
    rc = ib_context_module_config(ctx, mod, (void *)&config);
    if (rc != IB_OK) {
        ib_log_alert(ib,
                     "Failed to fetch core module context config: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Tell the collection manager about this context going away */
    rc = core_ctx_managed_collection_destroy(ib, mod, ctx, config);
    if (rc != IB_OK) {
        ib_log_alert(ib,
                     "Failed to shut down managed collections for \"%s\": %s",
                     ib_context_full_get(ctx), ib_status_to_string(rc));
    }

    /* Close the log file in the main configuration only */
    main_config = core_get_main_config(ib, false);
    if (config == main_config) {
        core_log_file_close(ib, config);
    }

    return IB_OK;
}

/**
 * Static core module structure.
 */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,           /**< Default metadata */
    MODULE_NAME_STR,                     /**< Module name */
    IB_MODULE_CONFIG(&core_global_cfg),  /**< Global config data */
    core_config_map,                     /**< Configuration field map */
    core_directive_map,                  /**< Config directive map */
    core_init,                           /**< Initialize function */
    NULL,                                /**< Callback data */
    core_finish,                         /**< Finish function */
    NULL,                                /**< Callback data */
    core_ctx_open,                       /**< Context open function */
    NULL,                                /**< Callback data */
    core_ctx_close,                      /**< Context close function */
    NULL,                                /**< Callback data */
    core_ctx_destroy,                    /**< Context destroy function */
    NULL                                 /**< Callback data */
);
