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
 * @brief IronBee --- SQLi/XSS Module based on libinjection.
 *
 * This module utilizes libinjection to implement SQLi and XSS detection. The
 * libinjection library is the work of Nick Galbreath.
 *
 * http://www.client9.com/projects/libinjection/
 *
 * Transformations:
 *   - normalizeSqli: Normalize SQL routine from libinjection.
 *
 * Operators:
 *   - is_sqli: Returns true if the data contains SQL injection.
 *   - is_xss:  Returns true if the data contains XSS.
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

/* See `man 7 feature_test_macros` on certain Linux flavors. */
#define _POSIX_C_SOURCE 200809L

#include <ironbee/context.h>
#include <ironbee/mm_mpool_lite.h>
#include <ironbee/module.h>
#include <ironbee/path.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>
#include <ironbee/transformation.h>
#include <ironbee/type_convert.h>
#include <ironbee/util.h>

#include <libinjection.h>
#include <libinjection_sqli.h>
#include <libinjection_xss.h>

#ifndef LIBINJECTION_SQLI_MAX_TOKENS
#define LIBINJECTION_SQLI_MAX_TOKENS 5
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME     sqli
#define MODULE_NAME_STR IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/* Fingerprint and Confidence */
typedef struct sqli_fingerprint_entry_t {
    ib_num_t  confidence;
    char     *fingerprint;
} sqli_fingerprint_entry_t;

/* Finger printer database. */
typedef struct sqli_fingerprint_set_t {
    sqli_fingerprint_entry_t *fingerprints;     /**< Sorted array of entries. */
    size_t                    num_fingerprints; /**< Size of @ref fingerprints. */
} sqli_fingerprint_set_t;

/* Callback data for lookup. */
typedef struct sqli_callback_data_t {
    const sqli_fingerprint_set_t *fingerprint_set;
    ib_num_t                      confidence;
} sqli_callback_data_t;

/* Module configuration. */
typedef struct sqli_module_config_t {
    /* For now, only support main context configuration. */
    /**
     * Hash of set name to sqli_fingerprint_set_t*.
     **/
    ib_hash_t *fingerprint_sets;
} sqli_module_config_t;
static sqli_module_config_t sqli_initial_config = { NULL };

/* Normalization function prototype. */
typedef int (*sqli_tokenize_fn_t)(sfilter * sf, stoken_t * sout);

static
int sqli_cmp(const void *a, const void *b)
{
    const sqli_fingerprint_entry_t *a_entry = (const sqli_fingerprint_entry_t *)a;
    const sqli_fingerprint_entry_t *b_entry = (const sqli_fingerprint_entry_t *)b;

    return strcmp(a_entry->fingerprint, b_entry->fingerprint);
}

static
int sqli_is_sqli_fingerprint(const char *fingerprint, size_t len, void *cbdata)
{
    assert(len <= LIBINJECTION_SQLI_MAX_TOKENS);

    sqli_callback_data_t *callback_data =
        (sqli_callback_data_t *)cbdata;
    char fp[LIBINJECTION_SQLI_MAX_TOKENS + 1];
    const sqli_fingerprint_entry_t *result = NULL;

    /* Create a NUL terminated string. */
    memcpy(fp, fingerprint, len);
    fp[len] = '\0';

    /* Calling bsearch on an array of *pointers*, thus a pointer to an
       element is a pointer to a pointer.

       Note that &fp is different than &fp_p. */
    if (
        callback_data != NULL &&
        callback_data->fingerprint_set != NULL &&
        callback_data->fingerprint_set->num_fingerprints > 0
    ) {
        const sqli_fingerprint_set_t *fps = callback_data->fingerprint_set;

        sqli_fingerprint_entry_t key = { 0, fp };
        result = bsearch(
            &key,
            fps->fingerprints, fps->num_fingerprints, sizeof(*fps->fingerprints),
            &sqli_cmp
        );
        if (result != NULL) {
            callback_data->confidence = result->confidence;
        }
    }

    return result != NULL;
}

static
char sqli_lookup_word(sfilter *sf, int lookup_type,
                      const char* str, size_t len)
{
    /* Only care about fingerprint lookups. */
    if (lookup_type != LOOKUP_FINGERPRINT) {
        return libinjection_sqli_lookup_word(sf, lookup_type, str, len);
    }

    /* Must return 'X' or '\0' as true or false. */
    return sqli_is_sqli_fingerprint(str, len, sf->userdata) ? 'X' : '\0';
}

/*********************************
 * Transformations
 *********************************/

static
ib_status_t sqli_normalize_tfn(
    ib_mm_t            mm,
    const ib_field_t  *field_in,
    const ib_field_t **field_out,
    void              *instdata,
    void              *tfn_data
)
{
    assert(field_in  != NULL);
    assert(field_out != NULL);

    const sqli_fingerprint_set_t *ps = (const sqli_fingerprint_set_t *)tfn_data;
    sfilter                   sf;
    ib_bytestr_t             *bs_in;
    ib_bytestr_t             *bs_out;
    const char               *buf_in;
    char                     *buf_in_start;
    size_t                    buf_in_len;
    char                     *buf_out;
    char                     *buf_out_end;
    size_t                    buf_out_len;
    size_t                    lead_len = 0;
    char                      prev_token_type;
    ib_field_t               *field_new;
    ib_status_t               rc;
    size_t                    fingerprint_len;

    /* Currently only bytestring types are supported.
     * Other types will just get passed through. */
    if (field_in->type != IB_FTYPE_BYTESTR) {
        *field_out = field_in;
        return IB_OK;
    }

    /* Extract the underlying incoming value. */
    rc = ib_field_value(field_in, ib_ftype_bytestr_mutable_out(&bs_in));
    if (rc != IB_OK) {
        return rc;
    }
    if (ib_bytestr_length(bs_in) == 0) {
        *field_out = field_in;
        return IB_OK;
    }

    /* Create a buffer big enough (double) to allow for normalization. */
    buf_in = (const char *)ib_bytestr_const_ptr(bs_in);
    buf_out = buf_out_end = (char *)ib_mm_calloc(mm, 2, ib_bytestr_length(bs_in));
    if (buf_out == NULL) {
        return IB_EALLOC;
    }

/* TODO: With the latest libinjection, we will need to do something like the
 * following, but more robust, instead of just calling is_sqli. This seems
 * to be because folding is now called, which removes some tokens.
 */
#if 0
    /* As SQL can be injected into a string, the normalization
     * needs to start after the first quote character if one
     * exists.
     *
     * First try single quote, then double, then none.
     *
     * TODO: Handle returning multiple transformations:
     *       1) Straight normalization
     *       2) Normalization as if with single quotes (starting point
     *          should be based on straight normalization)
     *       3) Normalization as if with double quotes (starting point
     *          should be based on straight normalization)
     */
    buf_in_start = memchr(buf_in, CHAR_SINGLE, ib_bytestr_length(bs_in));
    if (buf_in_start == NULL) {
        buf_in_start = memchr(buf_in, CHAR_DOUBLE, ib_bytestr_length(bs_in));
    }
    if (buf_in_start == NULL) {
        buf_in_start = (char *)buf_in;
        buf_in_len = ib_bytestr_length(bs_in);
    }
    else {
        ++buf_in_start; /* After the quote. */
        buf_in_len = ib_bytestr_length(bs_in) - (buf_in_start - buf_in);
    }

    /* Copy the leading string if one exists. */
    if (buf_in_start != buf_in) {
        lead_len = buf_in_start - buf_in;
        memcpy(buf_out, buf_in, lead_len);
        buf_out_end += lead_len;
    }
#endif
    buf_in_start = (char *)buf_in;
    buf_in_len = ib_bytestr_length(bs_in);

    /* Copy the normalized tokens as a space separated list. Since
     * the tokenizer does not backtrack, and the normalized values
     * are always equal to or less than the original length, the
     * tokens are written back to the beginning of the original
     * buffer.
     */
    libinjection_sqli_init(&sf,buf_in_start, buf_in_len, FLAG_NONE);
    libinjection_sqli_callback(&sf, sqli_lookup_word, (void *)ps);

    /* NOTE: We do not care if it is sqli, but just want the tokens. */
    libinjection_is_sqli(&sf);

    if (strlen(sf.fingerprint) == 0) {
        *field_out = field_in;
        return IB_OK;
    }

    buf_out_len = 0;
    prev_token_type = 0;
    fingerprint_len = strlen(sf.fingerprint);
    for (size_t i = 0; i < fingerprint_len; ++i) {
        stoken_t current = sf.tokenvec[i];
        size_t token_len = strlen(current.val);

        /* Add in the space if required. */
        if ((buf_out_end != buf_out) &&
            (current.type != 'o') &&
            (prev_token_type != 'o') &&
            (current.type != ',') &&
            (*(buf_out_end - 1) != ','))
        {
            *buf_out_end = ' ';
            buf_out_end += 1;
            ++buf_out_len;
        }

        /* Copy the token value. */
        memcpy(buf_out_end, current.val, token_len);
        buf_out_end += token_len;
        buf_out_len += token_len;

        prev_token_type = current.type;
    }


    /* Create the output field wrapping bs_out. */
    buf_out_len += lead_len;
    rc = ib_bytestr_alias_mem(&bs_out, mm, (uint8_t *)buf_out, buf_out_len);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_field_create(&field_new, mm,
                         field_in->name, field_in->nlen,
                         IB_FTYPE_BYTESTR,
                         ib_ftype_bytestr_mutable_in(bs_out));
    if (rc == IB_OK) {
        *field_out = field_new;
    }
    return rc;
}

/*********************************
 * Operators
 *********************************/

static
ib_status_t sqli_op_create(
    ib_context_t *ctx,
    ib_mm_t       mm,
    const char   *parameters,
    void         *instance_data,
    void         *cbdata
)
{
    ib_engine_t *ib = ib_context_get_engine(ctx);
    ib_status_t rc;
    ib_module_t *m = (ib_module_t *)cbdata;
    const char *set_name;
    size_t set_name_len;

    const sqli_module_config_t *cfg = NULL;
    const sqli_fingerprint_set_t    *ps  = NULL;

    if (parameters == NULL) {
        ib_log_error(ib, "Missing parameter for operator sqli");
        return IB_EINVAL;
    }

    set_name = parameters;
    set_name_len = strlen(parameters);
    if (set_name[0] == '\'') {
        ++set_name;
        --set_name_len;
    }
    if (set_name[set_name_len-1] == '\'') {
        --set_name_len;
    }

    if (strncmp("default", set_name, set_name_len) == 0) {
        *(const sqli_fingerprint_set_t **)instance_data = NULL;
        return IB_OK;
    }

    rc = ib_context_module_config(ctx, m, &cfg);
    assert(rc == IB_OK);
    if (cfg->fingerprint_sets == NULL) {
        rc = IB_ENOENT;
    }
    else {
        rc = ib_hash_get_ex(cfg->fingerprint_sets, &ps, set_name, set_name_len);
    }
    if (rc == IB_ENOENT) {
        ib_log_error(ib, "No such fingerprint set: %s", parameters);
        return IB_EINVAL;
    }
    assert(rc == IB_OK);
    assert(ps != NULL);

    *(const sqli_fingerprint_set_t **)instance_data = ps;

    return IB_OK;
}

static
ib_status_t sqli_op_execute(
    ib_tx_t *tx,
    const ib_field_t *field,
    ib_field_t *capture,
    ib_num_t *result,
    void *instance_data,
    void *cbdata
)
{
    assert(tx     != NULL);
    assert(field  != NULL);
    assert(result != NULL);

    const sqli_fingerprint_set_t *ps = (const sqli_fingerprint_set_t *)instance_data;
    sfilter                   sf;
    ib_bytestr_t             *bs;
    ib_status_t               rc;
    sqli_callback_data_t      callback_data;

    *result = 0;

    /* Currently only bytestring types are supported.
     * Other types will just get passed through. */
    if (field->type != IB_FTYPE_BYTESTR) {
        return IB_OK;
    }

    rc = ib_field_value(field, ib_ftype_bytestr_mutable_out(&bs));
    if (rc != IB_OK) {
        return rc;
    }

    /* Run through libinjection. */
    libinjection_sqli_init(
        &sf,
        (const char *)ib_bytestr_const_ptr(bs),
        ib_bytestr_length(bs),
        FLAG_NONE
    );
    callback_data.confidence = 0;
    callback_data.fingerprint_set = NULL;
    if (ps != NULL) {
        callback_data.fingerprint_set = ps;
        libinjection_sqli_callback(&sf, sqli_lookup_word, (void *)&callback_data);
    }
    if (libinjection_is_sqli(&sf)) {
        ib_log_debug_tx(tx, "Matched SQLi fingerprint: %s", sf.fingerprint);
        *result = 1;
    }
    if (*result == 1 && capture != NULL) {
        {
            ib_field_t *fingerprint_field;
            size_t fingerprint_length = strlen(sf.fingerprint);
            const uint8_t *fingerprint;

            fingerprint = ib_mm_memdup(
                tx->mm,
                sf.fingerprint, fingerprint_length
            );
            if (fingerprint == NULL) {
                return IB_EALLOC;
            }

            rc = ib_field_create_bytestr_alias(
                &fingerprint_field,
                tx->mm,
                IB_S2SL("fingerprint"),
                fingerprint, fingerprint_length
            );
            if (rc != IB_OK) {
                return rc;
            }

            rc = ib_field_list_add(capture, fingerprint_field);
            if (rc != IB_OK) {
                return rc;
            }
        }

        {
            ib_field_t *confidence_field;

            rc = ib_field_create(
                &confidence_field,
                tx->mm,
                IB_S2SL("confidence"),
                IB_FTYPE_NUM,
                ib_ftype_num_in(&callback_data.confidence)
            );
            if (rc != IB_OK) {
                return rc;
            }

            rc = ib_field_list_add(capture, confidence_field);
            if (rc != IB_OK) {
                return rc;
            }
        }
    }

    return IB_OK;
}

static
ib_status_t xss_op_execute(
    ib_tx_t *tx,
    const ib_field_t *field,
    ib_field_t *capture,
    ib_num_t *result,
    void *instance_data,
    void *cbdata
)
{
    assert(tx     != NULL);
    assert(field  != NULL);
    assert(result != NULL);

    ib_bytestr_t *bs;
    ib_status_t rc;

    *result = 0;

    /* Currently only bytestring types are supported.
     * Other types will just get passed through. */
    if (field->type != IB_FTYPE_BYTESTR) {
        return IB_OK;
    }

    rc = ib_field_value(field, ib_ftype_bytestr_mutable_out(&bs));
    if (rc != IB_OK) {
        return rc;
    }

    /* Run through libinjection. */
    // TODO: flags parameter is currently undocumented - using 0
    if (libinjection_is_xss((const char *)ib_bytestr_const_ptr(bs), ib_bytestr_length(bs), 0)) {
        ib_log_debug_tx(tx, "Matched XSS.");
        *result = 1;
    }

   return IB_OK;
}


/*********************************
 * Helper Functions
 *********************************/
static
ib_status_t sqli_create_fingerprint_set_from_file(
    sqli_fingerprint_set_t **out_ps,
    const char         *path,
    ib_mm_t             mm
)
{
    assert(out_ps != NULL);
    assert(path   != NULL);

    ib_status_t         rc;
    FILE               *fp          = NULL;
    char               *buffer      = NULL;
    size_t              buffer_size = 0;
    ib_list_t          *items       = NULL;
    ib_list_node_t     *n           = NULL;
    ib_mpool_lite_t    *tmp         = NULL;
    ib_mm_t             tmp_mm;
    sqli_fingerprint_set_t *ps          = NULL;
    size_t              i           = 0;

    /* Temporary memory pool for this function only. */
    rc = ib_mpool_lite_create(&tmp);
    assert(rc == IB_OK);
    assert(tmp != NULL);
    tmp_mm = ib_mm_mpool_lite(tmp);

    fp = fopen(path, "r");
    if (fp == NULL) {
        goto fail;
    }

    rc = ib_list_create(&items, tmp_mm);
    assert(rc    == IB_OK);
    assert(items != NULL);

    for (;;) {
        char *buffer_copy;
        int   read = getline(&buffer, &buffer_size, fp);
        char *space = NULL;
        ib_num_t confidence = 0;
        sqli_fingerprint_entry_t *entry = ib_mm_alloc(tmp_mm, sizeof(*entry));

        if (read == -1) {
            if (! feof(fp)) {
                fclose(fp);
                goto fail;
            }
            else {
                break;
            }
        }
        while (buffer[read-1] == '\n' || buffer[read-1] == '\r') {
            buffer[read-1] = '\0';
            --read;
        }

        space = strstr(buffer, " ");
        if (space != NULL) {
            rc = ib_type_atoi(space + 1, 10, &confidence);
            if (rc != IB_OK || confidence > 100) {
                return IB_EINVAL;
            }
            *space = '\0';
        }

        buffer_copy = ib_mm_strdup(mm, buffer);
        assert(buffer_copy != NULL);

        entry->confidence = confidence;
        entry->fingerprint = buffer_copy;

        rc = ib_list_push(items, (void *)entry);
        assert(rc == IB_OK);
    }

    fclose(fp);

    ps = ib_mm_alloc(mm, sizeof(*ps));
    assert(ps != NULL);

    ps->num_fingerprints = ib_list_elements(items);
    ps->fingerprints =
        ib_mm_alloc(mm, ps->num_fingerprints * sizeof(*ps->fingerprints));
    assert(ps->fingerprints != NULL);

    i = 0;
    IB_LIST_LOOP(items, n) {
        const sqli_fingerprint_entry_t *entry =
            (const sqli_fingerprint_entry_t *)ib_list_node_data(n);
        ps->fingerprints[i] = *entry;
        ++i;
    }
    assert(i == ps->num_fingerprints);

    ib_mpool_lite_destroy(tmp);

    qsort(
        ps->fingerprints, ps->num_fingerprints,
        sizeof(*ps->fingerprints),
        &sqli_cmp
    );

    *out_ps = ps;

    return IB_OK;

fail:
    ib_mpool_lite_destroy(tmp);
    return IB_EINVAL;
}

/*********************************
 * Directive Functions
 *********************************/

static
ib_status_t sqli_dir_fingerprint_set(
    ib_cfgparser_t *cp,
    const char     *directive_name,
    const char     *set_name,
    const char     *set_path,
    void           *cbdata
)
{
    assert(cp             != NULL);
    assert(directive_name != NULL);
    assert(set_name       != NULL);
    assert(set_path       != NULL);

    ib_status_t             rc;
    ib_context_t           *ctx = NULL;
    ib_module_t            *m   = NULL;
    sqli_module_config_t   *cfg = NULL;
    sqli_fingerprint_set_t *ps  = NULL;
    ib_mm_t                 mm;
    char                   *abs_set_path = NULL;

    rc = ib_cfgparser_context_current(cp, &ctx);
    assert(rc  == IB_OK);
    assert(ctx != NULL);

    if (ctx != ib_context_main(cp->ib)) {
        ib_cfg_log_error(cp,
            "%s: Only valid at main context.", directive_name
        );
        return IB_EINVAL;
    }

    if (strcmp("default", set_name) == 0) {
        ib_cfg_log_error(cp,
            "%s: default is a reserved set name.", directive_name
        );
        return IB_EINVAL;
    }

    mm = ib_engine_mm_main_get(cp->ib);

    rc = ib_engine_module_get(
        ib_context_get_engine(ctx),
        MODULE_NAME_STR,
        &m
    );
    assert(rc == IB_OK);

    rc = ib_context_module_config(ctx, m, &cfg);
    assert(rc == IB_OK);

    if (cfg->fingerprint_sets == NULL) {
        rc = ib_hash_create(&cfg->fingerprint_sets, mm);
        assert(rc == IB_OK);
    }
    assert(cfg->fingerprint_sets != NULL);

    rc = ib_hash_get(cfg->fingerprint_sets, NULL, set_name);
    if (rc == IB_OK) {
        ib_cfg_log_error(cp,
            "%s: Duplicate fingerprint set definition: %s",
            directive_name, set_name
        );
        return IB_EINVAL;
    }
    assert(rc == IB_ENOENT);

    abs_set_path = ib_util_relative_file(
        ib_engine_mm_config_get(cp->ib),
        ib_cfgparser_curr_file(cp),
        set_path
    );
    if (abs_set_path == NULL) {
        return IB_EALLOC;
    }

    rc = sqli_create_fingerprint_set_from_file(&ps, abs_set_path, mm);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
            "%s: Failure to load fingerprint set from file: %s",
            directive_name, abs_set_path
        );
        return IB_EINVAL;
    }
    assert(ps != NULL);

    rc = ib_hash_set(cfg->fingerprint_sets, ib_mm_strdup(mm, set_name), ps);
    assert(rc == IB_OK);

    return IB_OK;
}

/*********************************
 * Module Functions
 *********************************/

/* Called to initialize a module (on load). */
static ib_status_t sqli_init(ib_engine_t *ib, ib_module_t *m, void *cbdata)
{
    assert(ib != NULL);
    assert(m  != NULL);

    ib_status_t rc;

    /* Register normalizeSqli transformation. */
    rc = ib_transformation_create_and_register(
        NULL,
        ib,
        "normalizeSqli",
        false,
        NULL, NULL,
        NULL, NULL,
        sqli_normalize_tfn, NULL
    );
    if (rc != IB_OK) {
        return rc;
    }

    /* Register is_sqli operator. */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "is_sqli",
        IB_OP_CAPABILITY_CAPTURE,
        sqli_op_create, m,
        NULL, NULL,
        sqli_op_execute, NULL
    );
    if (rc != IB_OK) {
        return rc;
    }

    /* Register is_xss operator. */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "is_xss",
        IB_OP_CAPABILITY_NONE,
        NULL, NULL,
        NULL, NULL,
        xss_op_execute, NULL
    );
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

static IB_DIRMAP_INIT_STRUCTURE(sqli_directive_map) = {
    IB_DIRMAP_INIT_PARAM2(
        "LibInjectionFingerprintSet",
        sqli_dir_fingerprint_set, NULL
    ),
    IB_DIRMAP_INIT_LAST
};

/* Initialize the module structure. */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,           /* Default metadata */
    MODULE_NAME_STR,                     /* Module name */
    IB_MODULE_CONFIG(&sqli_initial_config), /* Global config data */
    NULL,                                /* Configuration field map */
    sqli_directive_map,                  /* Config directive map */
    sqli_init,                           /* Initialize function */
    NULL,                                /* Callback data */
    NULL,                                /* Finish function */
    NULL,                                /* Callback data */
);
