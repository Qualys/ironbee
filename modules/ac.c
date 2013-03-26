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
 * @brief IronBee --- AhoCorasick Matcher Module
 *
 * This module adds an AhoCorasick based matcher named "ac".
 *
 * @author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 */

#include <ironbee/ahocorasick.h>
#include <ironbee/bytestr.h>
#include <ironbee/capture.h>
#include <ironbee/cfgmap.h>
#include <ironbee/engine.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/path.h>
#include <ironbee/provider.h>
#include <ironbee/rule_capture.h>
#include <ironbee/rule_engine.h>
#include <ironbee/types.h>
#include <ironbee/util.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        ac
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Informational extra data.. version of this module (should be better to
 * register it with the module itself) */
#define AC_MAJOR           0
#define AC_MINOR           1
#define AC_DATE            20110812

typedef struct modac_cfg_t modac_cfg_t;
typedef struct modac_cpatt_t modac_cpatt_t;

/* Define the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Internal representation of AC compiled patterns.
 */
struct modac_provider_data_t {
    ib_ac_t *ac_tree;                 /**< The AC tree */
};

/* Instantiate a module global configuration. */
typedef struct modac_provider_data_t modac_provider_data_t;

/**
 * Workspace data stored per rule per transaction in tx.
 *
 * This is not directly stored in tx but in an ib_hash_t that is
 * stored in tx.
 */
struct modac_workspace_t {
    ib_ac_context_t *ctx; /**< Context. */
};
typedef struct modac_workspace_t modac_workspace_t;

/* -- Helper Internal Functions -- */

/**
 * Get or create an ib_hash_t inside tx for storing tx rule data.
 *
 * @param[in] tx The transaction containing the data which holds
 *            the @a rule_data object.
 * @param[out] rule_data The fetched or created rule data hash. This is set
 *             to NULL on failure.
 *
 * @return
 *   - IB_OK on success.
 *   - IB_EALLOC on allocation failure
 */
static ib_status_t get_or_create_rule_data_hash(ib_tx_t *tx,
                                                ib_hash_t **rule_data)
{
    assert(tx);
    assert(tx->mp);

    ib_status_t rc;

    /* Get or create the hash that contains the rule data. */
    rc = ib_tx_get_module_data(tx, IB_MODULE_STRUCT_PTR, (void **)rule_data);

    if (rc == IB_OK && *rule_data != NULL) {
        ib_log_debug2_tx(tx, "Found rule data hash in tx.");
        return IB_OK;
    }

    ib_log_debug2_tx(tx, "Rule data hash did not exist in tx.");
    ib_log_debug2_tx(tx, "Creating rule data hash.");

    rc = ib_hash_create(rule_data, tx->mp);
    if (rc != IB_OK) {
        ib_log_debug2_tx(tx,
                         "Failed to create hash: %d", rc);
        return rc;
    }

    rc = ib_tx_set_module_data(tx, IB_MODULE_STRUCT_PTR, *rule_data);
    if (rc != IB_OK) {
        ib_log_debug2_tx(tx,
                         "Failed to store hash: %d", rc);
        *rule_data = NULL;
    }

    ib_log_debug2_tx(tx,
                     "Returning rule hash at %p.",
                     *rule_data);

    return rc;

}

/**
 * Create the per-transaction data for use with the dfa operator.
 *
 * @param[in,out] tx Transaction to store the value in.
 * @param[in] ac The Ahocorasic engine object.
 * @param[in] id The operator identifier used to get it's workspace.
 * @param[out] workspace Created.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on an allocation error.
 */
static ib_status_t alloc_ac_tx_data(ib_tx_t *tx,
                                    ib_ac_t *ac,
                                    const char *id,
                                    modac_workspace_t **workspace)
{
    assert(tx);
    assert(tx->mp);
    assert(id);
    assert(workspace);

    ib_hash_t *rule_data;
    ib_status_t rc;

    rc = get_or_create_rule_data_hash(tx, &rule_data);
    if (rc != IB_OK) {
        return rc;
    }

    *workspace = (modac_workspace_t *)ib_mpool_alloc(tx->mp,
                                                     sizeof(**workspace));

    if (*workspace == NULL) {
        return IB_EALLOC;
    }

    (*workspace)->ctx =
        (ib_ac_context_t *)ib_mpool_alloc(tx->mp, sizeof(*(*workspace)->ctx));
    if ((*workspace)->ctx == NULL) {
        return IB_EALLOC;
    }

    ib_ac_init_ctx((*workspace)->ctx, ac);

    rc = ib_hash_set(rule_data, id, *workspace);
    if (rc != IB_OK) {
        *workspace = NULL;
    }

    return rc;
}

/**
 * Return the per-transaction data for use with the dfa operator.
 *
 * @param[in,out] tx Transaction to store the value in.
 * @param[in] id The operator identifier used to get it's workspace.
 * @param[out] workspace Retrieved.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_ENOENT if the structure does not exist. Call alloc_ac_tx_data then.
 *   - IB_EALLOC on an allocation error.
 */
static ib_status_t get_dfa_tx_data(ib_tx_t *tx,
                                   const char *id,
                                   modac_workspace_t **workspace)
{
    assert(tx);
    assert(tx->mp);
    assert(id);
    assert(workspace);

    ib_hash_t *rule_data;
    ib_status_t rc;

    rc = get_or_create_rule_data_hash(tx, &rule_data);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hash_get(rule_data, workspace, id);
    if (rc != IB_OK) {
        *workspace = NULL;
    }

    return rc;
}

/* -- Matcher Interface -- */

/**
 * Add a pattern to the patterns of the matcher given a pattern and
 * callback + extra arg
 *
 * @param mpi matcher provider
 * @param patterns pointer to the pattern container (i.e.: an AC tree)
 * @param patt the pattern to be added
 * @param callback the callback to register with the given pattern
 * @param arg the extra argument to pass to the callback
 * @param errptr a pointer reference to point where an error occurred
 * @param erroffset a pointer holding the offset of the error
 *
 * @return status of the operation
 */
static ib_status_t modac_add_pattern_ex(ib_provider_inst_t *mpi,
                                        void *patterns,
                                        const char *patt,
                                        ib_void_fn_t callback,
                                        void *arg,
                                        const char **errptr,
                                        int *erroffset)
{
    ib_status_t rc;
    ib_ac_t *ac_tree = (ib_ac_t *)((modac_provider_data_t *)mpi->data)->ac_tree;

    /* If the ac_tree doesn't exist, create it before adding the pattern */
    if (ac_tree == NULL) {
        rc = ib_ac_create(&ac_tree, 0, mpi->mp);
        if (rc != IB_OK || ac_tree == NULL) {
            ib_log_error(mpi->pr->ib,
                         "Unable to create the AC tree at modac");
            return rc;
        }
        ((modac_provider_data_t *)mpi->data)->ac_tree = ac_tree;
    }

    rc = ib_ac_add_pattern(ac_tree, patt, (ib_ac_callback_t)callback, arg, 0);

    if (rc == IB_OK) {
        ib_log_debug(mpi->pr->ib, "pattern %s added to the AC tree %p", patt,
                     ac_tree);
    }
    else {
        ib_log_error(mpi->pr->ib,  "Failed to load pattern %s to the AC tree %p",
                     patt, ac_tree);
    }

    return rc;
}

/**
 * Initialize a provider instance with the given data
 *
 * @param mpi provider instance
 * @param data data
 *
 * @return status of the operation
 */
static ib_status_t modac_provider_instance_init(ib_provider_inst_t *mpi,
                                                void *data)
{
    ib_status_t rc;
    modac_provider_data_t *dt;

    dt = (modac_provider_data_t *)ib_mpool_calloc(
        mpi->mp, 1,
        sizeof(modac_provider_data_t)
    );
    if (dt == NULL) {
        return IB_EALLOC;
    }

    mpi->data = (void *)dt;
    rc = ib_ac_create(&dt->ac_tree, 0, mpi->mp);

    if (rc != IB_OK) {
        ib_log_error(mpi->pr->ib,  "Unable to create the AC tree at modac");
    }

    return rc;
}

/**
 * Match against the AC tree
 *
 * @param mpi provider instance
 * @param flags extra flags
 * @param data the data to search in
 * @param dlen length of the the data to search in
 * @param ctx Current configuration context
 *
 * @return status of the operation
 */
static ib_status_t modac_match(ib_provider_inst_t *mpi,
                               ib_flags_t flags,
                               const uint8_t *data,
                               size_t dlen,
                               void *ctx)
{
    modac_provider_data_t *dt = mpi->data;

    if (dt == NULL) {
        return IB_EINVAL;
    }

    ib_log_debug(mpi->pr->ib, "Matching AGAINST AC tree %p",
                 dt->ac_tree);


    ib_ac_t *ac_tree = dt->ac_tree;

    ib_ac_context_t *ac_mctx = (ib_ac_context_t *)ctx;

    ib_ac_reset_ctx(ac_mctx, ac_tree);

    /* Let's perform the search. Content is consumed in just one call */
    ib_status_t rc = ib_ac_consume(ac_mctx,
                                   (const char *)data,
                                   dlen,
                                   IB_AC_FLAG_CONSUME_DOLIST |
                                   IB_AC_FLAG_CONSUME_MATCHALL |
                                   IB_AC_FLAG_CONSUME_DOCALLBACK,
                                   mpi->mp);

    return rc;
}

static ib_status_t modac_compile(ib_provider_t *mpr,
                                 ib_mpool_t *pool,
                                 void *pcpatt,
                                 const char *patt,
                                 const char **errptr,
                                 int *erroffset)
{
    return IB_ENOTIMPL;
}

static ib_status_t modac_match_compiled(ib_provider_t *mpr,
                                        void *cpatt,
                                        ib_flags_t flags,
                                        const uint8_t *data,
                                        size_t dlen, void *ctx)
{
    return IB_ENOTIMPL;
}

static ib_status_t modac_add_pattern(ib_provider_inst_t *pi,
                                     void *cpatt)
{
    return IB_ENOTIMPL;
}

static IB_PROVIDER_IFACE_TYPE(matcher) modac_matcher_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,

    /* Provider Interface */
    modac_compile,
    modac_match_compiled,

    /* Provider Instance Interface */
    modac_add_pattern,
    modac_add_pattern_ex,
    modac_match
};

static void nop_ac_match(ib_ac_t *orig,
                         ib_ac_char_t *pattern,
                         size_t pattern_len,
                         void* userdata,
                         size_t offset,
                         size_t relative_offset)
{
    /* Nop. */
}

/**
 * @brief Read the given file into memory and return the malloced buffer.
 *
 * @param[in] ib Ironbee engine.
 * @param[in] ctx Configuration context
 * @param[in] mp Memory pool to use for allocations
 * @param[in] filename Filename to read.
 * @param[in,out] buffer Character buffer pointer that will be malloced
 *                and must be free'ed by the caller.
 */
static ib_status_t readfile(const ib_engine_t *ib,
                            const ib_context_t *ctx,
                            ib_mpool_t *mp,
                            const char *filename,
                            char **buffer)
{
    int fd;
    int rc;
    struct stat fd_stat;
    ssize_t len; /**< Length of the file. */
    ssize_t bytes_read;
    ssize_t total_bytes_read;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        const char *filepath;
        const char *path = ib_context_config_cwd(ctx);
        if (path == NULL) {
            return IB_ENOENT;
        }
        filepath = ib_util_path_join(mp, path, filename);
        if (filepath == NULL) {
            ib_log_error(ib, "Failed to create file path for \"%s\"", filename);
            return IB_EALLOC;
        }
        fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            ib_log_error(ib,
                         "Failed to open pattern file \"%s\" or \"%s\": %s",
                         filename, filepath, strerror(errno));
            return IB_ENOENT;
        }
        filename = filepath;
    }

    rc = fstat(fd, &fd_stat);

    if (rc == -1) {
        ib_log_error(ib,
                     "Failed to stat file %s: %s", filename, strerror(errno));
        return IB_EALLOC;
    }

    /* Protect the user from building a tree from a 1GB file of patterns. */
    if (fd_stat.st_size > 1024000000) {
        ib_log_error(ib,
                     "Refusing to parse file %s because it is too large.",
                     filename);
        return IB_EALLOC;
    }

    /* If conversion from off_t to ssize_t is required, it happens here. */
    len = fd_stat.st_size;

    *buffer = malloc(len + 1);

    if (*buffer == NULL) {
        close(fd);
        return IB_EALLOC;
    }

    total_bytes_read = 0;

    do {
        bytes_read = read(fd,
                          (*buffer)+total_bytes_read,
                          len - total_bytes_read);

        if (bytes_read < 0) {
            free(*buffer);
            *buffer = NULL;
            close(fd);
            return IB_EALLOC;
        }

        total_bytes_read += bytes_read;
    } while ( total_bytes_read < len );

    close(fd);

    /* Null terminate the buffer */
    (*buffer)[total_bytes_read] = '\0';

    ib_log_debug(ib, "Read AC pattern file \"%s\"", filename);
    return IB_OK;
}

static ib_status_t pmf_operator_create(ib_engine_t *ib,
                                       ib_context_t *ctx,
                                       const ib_rule_t *rule,
                                       ib_mpool_t *pool,
                                       const char *pattern_file,
                                       ib_operator_inst_t *op_inst)
{
    ib_status_t rc;

    ib_ac_t *ac;

    char* file = NULL;
    char* line = NULL;
    size_t pattern_file_len = strlen(pattern_file);

    /* Escaped directive and length. */
    char *pattern_file_unescaped;
    size_t pattern_file_unescaped_len;

    pattern_file_unescaped = malloc(pattern_file_len+1);

    if ( pattern_file_unescaped == NULL ) {
        return IB_EALLOC;
    }

    rc = ib_util_unescape_string(pattern_file_unescaped,
                                 &pattern_file_unescaped_len,
                                 pattern_file,
                                 pattern_file_len,
                                 IB_UTIL_UNESCAPE_NULTERMINATE |
                                 IB_UTIL_UNESCAPE_NONULL);
    if ( rc != IB_OK ) {
        const char *msg = (rc==IB_EBADVAL)?
            "Cannot unescape file \"%s\" because it contains NULLs." :
            "Cannot unescape file \"%s\".";
        free(pattern_file_unescaped);
        ib_log_debug(ib, msg, pattern_file);
        return rc;
    }

    /* Populate file. This data must be free'ed. */
    rc = readfile(ib, ctx, pool, pattern_file_unescaped, &file);

    free(pattern_file_unescaped);

    if (rc != IB_OK) {
        if (file != NULL) {
            free(file);
        }

        return rc;
    }

    rc = ib_ac_create(&ac, 0, pool);

    if (rc != IB_OK) {
        free(file);
        return rc;
    }

    /* Iterate through the file contents, one line at a time.
     * Each line is unescaped (allowing null characters) into line_unescaped
     * and added to the ahocorasic object as a pattern. */
    for (line=strtok(file, "\n"); line != NULL; line=strtok(NULL, "\n")) {
        size_t line_len = strlen(line);

        if ( line_len > 0 ) {
            size_t line_unescaped_len;
            char *line_unescaped = malloc(line_len+1);

            if ( line_unescaped == NULL ) {
                free(file);
                free(line_unescaped);
                return IB_EALLOC;
            }

            /* Escape a pattern, allowing nulls in the line. */
            ib_util_unescape_string(line_unescaped,
                                    &line_unescaped_len,
                                    line,
                                    line_len,
                                    IB_UTIL_UNESCAPE_NULTERMINATE);

            rc = ib_ac_add_pattern (ac, line_unescaped, &nop_ac_match, NULL, 0);

            free(line_unescaped);

            if (rc != IB_OK) {
                free(file);
                return rc;
            }
        }
    }

    rc = ib_ac_build_links(ac);

    if (rc != IB_OK) {
        free(file);
        return rc;
    }

    op_inst->data = ac;

    free(file);
    return IB_OK;
}

static ib_status_t pm_operator_create(ib_engine_t *ib,
                                      ib_context_t *ctx,
                                      const ib_rule_t *rule,
                                      ib_mpool_t *pool,
                                      const char *pattern,
                                      ib_operator_inst_t *op_inst)
{
    ib_status_t rc;

    ib_ac_t *ac;

    const size_t pattern_len = strlen(pattern);
    size_t tok_buffer_sz = pattern_len+1;
    char* tok_buffer = malloc(tok_buffer_sz);
    char* tok;

    if (tok_buffer == NULL ) {
        return IB_EALLOC;
    }

    ib_util_unescape_string(tok_buffer,
                            &tok_buffer_sz,
                            pattern,
                            pattern_len,
                            IB_UTIL_UNESCAPE_NULTERMINATE);


    memcpy(tok_buffer, pattern, tok_buffer_sz);

    rc = ib_ac_create(&ac, 0, pool);

    if (rc != IB_OK) {
        free(tok_buffer);
        return rc;
    }

    for (tok = strtok(tok_buffer, " "); tok != NULL; tok = strtok(NULL, " "))
    {
        if (strlen(tok) > 0) {
            rc = ib_ac_add_pattern(ac, tok, &nop_ac_match, NULL, 0);

            if (rc != IB_OK) {
                free(tok_buffer);
                return rc;
            }
        }
    }

    rc = ib_ac_build_links(ac);

    if (rc != IB_OK) {
        free(tok_buffer);
        return rc;
    }

    op_inst->data = ac;

    free(tok_buffer);
    return IB_OK;
}

static ib_status_t initialize_ac_ctx(ib_tx_t *tx,
                                     ib_ac_t *ac,
                                     const ib_rule_t *rule,
                                     ib_ac_context_t **ac_ctx)
{
    assert(tx);
    assert(ac);
    assert(ac_ctx);
    assert(rule);

    ib_status_t rc;

    /* Stream rules maintain state across calls.
     * If a rule is a stream rule, try to fetch the rule context data. */
    if (ib_rule_is_stream(rule)) {
        modac_workspace_t *workspace = NULL;

        ib_log_debug_tx(tx, "Fetching stream rule data.");

        rc = get_dfa_tx_data(tx, ib_rule_id(rule), &workspace);

        if ( (rc == IB_ENOENT) || (workspace == NULL) ) {
            rc = alloc_ac_tx_data(tx, ac, ib_rule_id(rule), &workspace);
            if (rc != IB_OK) {
                ib_log_error_tx(tx,
                                "Unexpected error creating tx data: %d",
                                rc);
                return rc;
            }
        }
        else if (rc != IB_OK) {
            ib_log_error_tx(tx, "Unexpected error retrieving tx data: %d", rc);
            return rc;
        }

        *ac_ctx = workspace->ctx;
    }

    /* Create a new context for every operator call. */
    else {
        *ac_ctx = (ib_ac_context_t *)ib_mpool_alloc(tx->mp, sizeof(**ac_ctx));
        if (*ac_ctx == NULL) {
            return IB_EALLOC;
        }

        ib_ac_init_ctx(*ac_ctx, ac);
    }

    return IB_OK;
}

static ib_status_t pm_operator_execute(const ib_rule_exec_t *rule_exec,
                                       void *data,
                                       ib_flags_t flags,
                                       ib_field_t *field,
                                       ib_num_t *result)
{
    assert(rule_exec);
    assert(data);

    ib_ac_t *ac = (ib_ac_t *)data;
    ib_ac_context_t *ac_ctx = NULL;
    ib_tx_t *tx = rule_exec->tx;
    ib_status_t rc;

    const char* subject;
    size_t subject_len;
    const ib_bytestr_t* bytestr;

    if (field->type == IB_FTYPE_NULSTR) {
        rc = ib_field_value(field, ib_ftype_nulstr_out(&subject));
        if (rc != IB_OK) {
            return rc;
        }

        subject_len = strlen(subject);
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        rc = ib_field_value(field, ib_ftype_bytestr_out(&bytestr));
        if (rc != IB_OK) {
            return rc;
        }

        subject_len = ib_bytestr_length(bytestr);
        subject = (const char *)ib_bytestr_const_ptr(bytestr);
    }
    else {
        return IB_EALLOC;
    }

    rc = initialize_ac_ctx(tx, ac, rule_exec->rule, &ac_ctx);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Cannot initialize AhoCorasic context: %d", rc);
        return rc;
    }

    rc = ib_ac_consume(ac_ctx, subject, subject_len, 0, tx->mp);

    if (rc == IB_ENOENT) {
        *result = 0;
        return IB_OK;
    }
    else if (rc == IB_OK) {
        *result = (ac_ctx->match_cnt > 0) ? 1 : 0;

        if (ib_rule_should_capture(rule_exec, *result)) {
            ib_field_t *f;
            const char *name;
            char *scopy;

            ib_rule_capture_clear(rule_exec);
            scopy = (char *)ib_mpool_alloc(tx->mp, subject_len);
            if (scopy != NULL) {
                memcpy(scopy, subject, subject_len);
                name = ib_capture_name(0);
                rc = ib_field_create_bytestr_alias(&f, tx->mp,
                                                   name, strlen(name),
                                                   (uint8_t *)scopy,
                                                   subject_len);
                if (rc == IB_OK) {
                    ib_rule_capture_set_item(rule_exec, 0, f);
                }
            }
        }

        return IB_OK;
    }

    return rc;
}

static ib_status_t pm_operator_destroy(ib_operator_inst_t *op_inst)
{
    /* Nop. */

    /* No callback required. Allocations are out of the IB memory pool. */

    return IB_OK;
}

/* -- Module Routines -- */

static ib_status_t modac_init(ib_engine_t *ib,
                              ib_module_t *m,
                              void        *cbdata)
{
    ib_status_t rc;

    ib_log_warning(ib,
        MODULE_NAME_STR ": Module deprecated. "
        "Consider: @streq, @istreq, @match, @imatch, @ee_match_any"
    );

    /* Register as a matcher provider. */
    rc = ib_provider_register(ib,
                              IB_PROVIDER_TYPE_MATCHER,
                              MODULE_NAME_STR,
                              NULL,
                              &modac_matcher_iface,
                              modac_provider_instance_init);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     MODULE_NAME_STR ": Error registering ac matcher provider: "
                     "%s", ib_status_to_string(rc));
        return IB_OK;
    }

    ib_operator_register(ib,
                         "pm",
                         ( IB_OP_FLAG_PHASE |
                           IB_OP_FLAG_STREAM |
                           IB_OP_FLAG_CAPTURE ),
                         &pm_operator_create,
                         NULL,
                         &pm_operator_destroy,
                         NULL,
                         &pm_operator_execute,
                         NULL);
    ib_operator_register(ib,
                         "pmf",
                         ( IB_OP_FLAG_PHASE |
                           IB_OP_FLAG_STREAM |
                           IB_OP_FLAG_CAPTURE ),
                         &pmf_operator_create,
                         NULL,
                         &pm_operator_destroy,
                         NULL,
                         &pm_operator_execute,
                         NULL);

    ib_log_debug(ib,
                 "AC Status: compiled=\"%d.%d %s\" AC Matcher registered",
                 AC_MAJOR, AC_MINOR, IB_XSTRINGIFY(AC_DATE));

    return IB_OK;
}

/**
 * Module structure.
 *
 * This structure defines some metadata, config data and various functions.
 */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,            /**< Default metadata */
    MODULE_NAME_STR,                      /**< Module name */
    IB_MODULE_CONFIG_NULL,                /* Global config data */
    NULL,                                 /**< Configuration field map */
    NULL,                                 /**< Config directive map */
    modac_init,                           /**< Initialize function */
    NULL,                                 /**< Callback data */
    NULL,                                 /**< Finish function */
    NULL,                                 /**< Callback data */
    NULL,                                 /**< Context open function */
    NULL,                                 /**< Callback data */
    NULL,                                 /**< Context close function */
    NULL,                                 /**< Callback data */
    NULL,                                 /**< Context destroy function */
    NULL                                  /**< Callback data */
);
