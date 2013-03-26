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
 * @brief IronBee --- HTP Module
 *
 * This module adds a PCRE based matcher named "pcre".
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include <ironbee/bytestr.h>
#include <ironbee/capture.h>
#include <ironbee/cfgmap.h>
#include <ironbee/engine.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/provider.h>
#include <ironbee/rule_capture.h>
#include <ironbee/rule_engine.h>
#include <ironbee/util.h>

#include <pcre.h>

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        pcre
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* How many matches will PCRE find and populate. */
#define MATCH_MAX 10

/* PCRE can use an independent stack or the machine stack.
 * If PCRE_JIT_STACK is true (conditional on PCRE_HAVE_JIT being true)
 * then pcrejit will use an independent stack. If PCRE_JIT_STACK is not
 * defined then the machine stack will be used.  */
#ifdef PCRE_HAVE_JIT
#define PCRE_JIT_STACK
const int PCRE_JIT_STACK_START_MULT = 32;
const int PCRE_JIT_STACK_MAX_MULT   = 512;
#endif

/* Port forward the pcre constant PCRE_PARTIAL as PCRE_PARTIAL_SOFT. */
#ifndef PCRE_PARTIAL_SOFT
#define PCRE_PARTIAL_SOFT PCRE_PARTIAL
#endif

/**
 * From pcreapi man page.
 */
#define WORKSPACE_SIZE_MIN     (20)

/**
 * Build a reasonable buffer size.
 */
#define WORKSPACE_SIZE_DEFAULT (WORKSPACE_SIZE_MIN * 10)

/* Define the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Module Configuration Structure.
 */
typedef struct modpcre_cfg_t {
    ib_num_t       study;                 /**< Bool: Study compiled regexs */
    ib_num_t       use_jit;               /**< Bool: Use JIT if available */
    ib_num_t       match_limit;           /**< Match limit */
    ib_num_t       match_limit_recursion; /**< Match recursion depth limit */
    ib_num_t       jit_stack_start;       /**< Starting JIT stack size */
    ib_num_t       jit_stack_max;         /**< Max JIT stack size */
    ib_num_t       dfa_workspace_size;    /**< Size of DFA workspace */
} modpcre_cfg_t;

/**
 * Internal representation of PCRE compiled patterns.
 */
typedef struct modpcre_cpat_data_t {
    pcre                *cpatt;           /**< Compiled pattern */
    size_t               cpatt_sz;        /**< Size of cpatt. */
    pcre_extra          *edata;           /**< PCRE Study data */
    size_t               study_data_sz;   /**< Size of edata->study_data. */
    const char          *patt;            /**< Regex pattern text */
    bool                 is_dfa;          /**< Is this a DFA? */
    bool                 is_jit;          /**< Is this JIT compiled? */
    int                  jit_stack_start; /**< Starting JIT stack size */
    int                  jit_stack_max;   /**< Max JIT stack size */
    int                  dfa_ws_size;     /**< Size of DFA workspace */
} modpcre_cpat_data_t;

/**
 * PCRE and DFA rule data types are an alias for the compiled pattern structure.
 */
typedef struct modpcre_rule_data_t {
    modpcre_cpat_data_t *cpdata;          /**< Compiled pattern data */
    const char          *id;              /**< ID for DFA rules */
} modpcre_rule_data_t;

/* Instantiate a module global configuration. */
static modpcre_cfg_t modpcre_global_cfg = {
    1,                      /* study */
    1,                      /* use_jit */
    5000,                   /* match_limit */
    5000,                   /* match_limit_recursion */
    0,                      /* jit_stack_start; 0 means auto */
    0,                      /* jit_stack_max; 0 means auto */
    WORKSPACE_SIZE_DEFAULT  /* dfa_workspace_size */
};

/**
 * Internal compilation of the modpcre pattern.
 *
 * @param[in] ib IronBee engine for logging.
 * @param[in] pool The memory pool to allocate memory out of.
 * @param[in] config Module configuration
 * @param[in] is_dfa Set to true for DFA
 * @param[out] pcpdata Pointer to new struct containing the compilation.
 * @param[in] patt The uncompiled pattern to match.
 * @param[out] errptr Pointer to an error message describing the failure.
 * @param[out] erroffset The location of the failure, if this fails.
 *
 * @returns IronBee status. IB_EINVAL if the pattern is invalid,
 *          IB_EALLOC if memory allocation fails or IB_OK.
 */
static ib_status_t pcre_compile_internal(ib_engine_t *ib,
                                         ib_mpool_t *pool,
                                         const modpcre_cfg_t *config,
                                         bool is_dfa,
                                         modpcre_cpat_data_t **pcpdata,
                                         const char *patt,
                                         const char **errptr,
                                         int *erroffset)
{
    assert(ib != NULL);
    assert(pool != NULL);
    assert(config != NULL);
    assert(pcpdata != NULL);
    assert(patt != NULL);

    /* Pattern data structure we'll create */
    modpcre_cpat_data_t *cpdata;

    /* Compiled pattern. */
    pcre *cpatt = NULL;

    /* Compiled pattern size. Used to copy cpatt. */
    size_t cpatt_sz;

    /* Extra data structure. This contains the study_data pointer. */
    pcre_extra *edata = NULL;

    /* Size of edata->study_data. */
    size_t study_data_sz;

    /* How cpatt is produced. */
    const int compile_flags = PCRE_DOTALL | PCRE_DOLLAR_ENDONLY;

    /* Are we using JIT? */
    bool use_jit = !is_dfa;

#ifdef PCRE_HAVE_JIT
    if (config->use_jit == 0) {
        use_jit = false;
    }

    /* Do we want to be using JIT? */
    const bool want_jit = use_jit;
#else
    use_jit = false;
#endif /* PCRE_HAVE_JIT */

    cpatt = pcre_compile(patt, compile_flags, errptr, erroffset, NULL);

    if (*errptr != NULL) {
        ib_log_error(ib, "PCRE compile error for \"%s\": %s at offset %d",
                     patt, *errptr, *erroffset);
        return IB_EINVAL;
    }

    if (config->study) {
        if (use_jit) {
#ifdef PCRE_HAVE_JIT
            edata = pcre_study(cpatt, PCRE_STUDY_JIT_COMPILE, errptr);
            if (*errptr != NULL)  {
                pcre_free(cpatt);
                use_jit = false;
                ib_log_warning(ib, "PCRE-JIT study failed: %s", *errptr);
            }
#endif
        }
        else {
            edata = pcre_study(cpatt, 0, errptr);
            if (*errptr != NULL)  {
                pcre_free(cpatt);
                ib_log_error(ib, "PCRE study failed: %s", *errptr);
            }
        }
    }
    else if (use_jit) {
        ib_log_warning(ib, "PCRE: Disabling JIT because study disabled");
        use_jit = false;
    }

#ifdef PCRE_HAVE_JIT
    /* The check to see if JIT compilation was a success changed in 8.20RC1
       now uses pcre_fullinfo see doc/pcrejit.3 */
    if (use_jit) {
        int rc;
        int pcre_jit_ret;

        rc = pcre_fullinfo(cpatt, edata, PCRE_INFO_JIT, &pcre_jit_ret);
        if (rc != 0) {
            ib_log_error(ib, "PCRE-JIT failed to get pcre_fullinfo");
            use_jit = false;
        }
        else if (pcre_jit_ret != 1) {
            ib_log_info(ib, "PCRE-JIT compiler does not support: %s", patt);
            use_jit = false;
        }
        else { /* Assume pcre_jit_ret == 1. */
            /* Do nothing */
        }
    }
    if (want_jit && !use_jit) {
        ib_log_info(ib, "Falling back to normal PCRE");
    }
#endif /*PCRE_HAVE_JIT*/

    /* Compute the size of the populated values of cpatt. */
    pcre_fullinfo(cpatt, edata, PCRE_INFO_SIZE, &cpatt_sz);
    if (edata != NULL) {
        pcre_fullinfo(cpatt, edata, PCRE_INFO_STUDYSIZE, &study_data_sz);
    }
    else {
        study_data_sz = 0;
    }

    /**
     * Below is only allocation and copy operations to pass the PCRE results
     * back to the output variable cpdata.
     */

    cpdata = (modpcre_cpat_data_t *)ib_mpool_calloc(pool, sizeof(*cpdata), 1);
    if (cpdata == NULL) {
        pcre_free(cpatt);
        pcre_free(edata);
        ib_log_error(ib,
                     "Failed to allocate cpdata of size: %zd",
                     sizeof(*cpdata));
        return IB_EALLOC;
    }

    cpdata->is_dfa = is_dfa;
    cpdata->is_jit = use_jit;
    cpdata->cpatt_sz = cpatt_sz;
    cpdata->study_data_sz = study_data_sz;

    /* Copy pattern. */
    cpdata->patt = ib_mpool_strdup(pool, patt);
    if (cpdata->patt == NULL) {
        pcre_free(cpatt);
        pcre_free(edata);
        ib_log_error(ib, "Failed to duplicate pattern string: %s", patt);
        return IB_EALLOC;
    }

    /* Copy compiled pattern. */
    cpdata->cpatt = ib_mpool_memdup(pool, cpatt, cpatt_sz);
    pcre_free(cpatt);
    if (cpdata->cpatt == NULL) {
        pcre_free(edata);
        ib_log_error(ib, "Failed to duplicate pattern of size: %zd", cpatt_sz);
        return IB_EALLOC;
    }
    ib_log_debug(ib, "PCRE copied cpatt @ %p -> %p (%zd bytes)",
                 (void *)cpatt, (void *)cpdata->cpatt, cpatt_sz);

    /* Copy extra data (study data). */
    if (edata != NULL) {

        /* Copy edata. */
        cpdata->edata = ib_mpool_memdup(pool, edata, sizeof(*edata));
        if (cpdata->edata == NULL) {
            pcre_free(edata);
            ib_log_error(ib, "Failed to duplicate edata.");
            return IB_EALLOC;
        }

        /* Copy edata->study_data. */
        if (edata->study_data != NULL) {
            cpdata->edata->study_data =
                ib_mpool_memdup(pool, edata->study_data, study_data_sz);

            if (cpdata->edata->study_data == NULL) {
                ib_log_error(ib, "Failed to study data of size: %zd",
                             study_data_sz);
                pcre_free(edata);
                return IB_EALLOC;
            }
        }
        pcre_free(edata);
    }
    else {
        cpdata->edata = ib_mpool_calloc(pool, 1, sizeof(*edata));
        if (cpdata->edata == NULL) {
            pcre_free(edata);
            ib_log_error(ib, "Failed to allocate edata.");
            return IB_EALLOC;
        }
    }

    /* Set the PCRE limits for non-DFA patterns */
    if (! is_dfa) {
        cpdata->edata->flags |=
            (PCRE_EXTRA_MATCH_LIMIT | PCRE_EXTRA_MATCH_LIMIT_RECURSION);
        cpdata->edata->match_limit =
            (unsigned long)config->match_limit;
        cpdata->edata->match_limit_recursion =
            (unsigned long)config->match_limit_recursion;
        cpdata->dfa_ws_size = 0;
    }
    else {
        cpdata->edata->match_limit = 0U;
        cpdata->edata->match_limit_recursion = 0U;
        cpdata->dfa_ws_size = (int)config->dfa_workspace_size;
    }

    /* Set stack limits for JIT */
    if (cpdata->is_jit) {
#ifdef PCRE_HAVE_JIT
        if (config->jit_stack_start == 0U) {
            cpdata->jit_stack_start =
                PCRE_JIT_STACK_START_MULT * config->match_limit_recursion;
        }
        else {
            cpdata->jit_stack_start = (int)config->jit_stack_start;
        }
        if (config->jit_stack_max == 0U) {
            cpdata->jit_stack_max =
                PCRE_JIT_STACK_MAX_MULT * config->match_limit_recursion;
        }
        else {
            cpdata->jit_stack_max = (int)config->jit_stack_max;
        }
#endif
    }
    else {
        cpdata->jit_stack_start = 0;
        cpdata->jit_stack_max = 0;
    }

    ib_log_trace(ib,
                 "Compiled pcre pattern \"%s\": "
                 "cpatt=%p edata=%p limit=%ld rlimit=%ld study=%p "
                 "dfa=%s dfa-ws-sz=%d "
                 "jit=%s jit-stack: start=%d max=%d",
                 patt,
                 (void *)cpdata->cpatt,
                 (void *)cpdata->edata,
                 cpdata->edata->match_limit,
                 cpdata->edata->match_limit_recursion,
                 cpdata->edata->study_data,
                 cpdata->is_dfa ? "yes" : "no",
                 cpdata->dfa_ws_size,
                 cpdata->is_jit ? "yes" : "no",
                 cpdata->jit_stack_start,
                 cpdata->jit_stack_max);
    *pcpdata = cpdata;

    return IB_OK;
}


/* -- Matcher Interface -- */

/**
 * @param[in] mpr Provider object.
 * @param[in] pool The memory pool to allocate memory out of.
 * @param[out] pcpatt When the pattern is successfully compiled
 *             a modpcre_cpat_data_t* is stored in *pcpatt.
 * @param[in] patt The uncompiled pattern to match.
 * @param[out] errptr Pointer to an error message describing the failure.
 * @param[out] erroffset The location of the failure, if this fails.
 *
 * @returns IronBee status. IB_EINVAL if the pattern is invalid,
 *          IB_EALLOC if memory allocation fails or IB_OK.
 */
static ib_status_t modpcre_compile(ib_provider_t *mpr,
                                   ib_mpool_t *pool,
                                   void *pcpatt,
                                   const char *patt,
                                   const char **errptr,
                                   int *erroffset)
{
    assert(mpr != NULL);
    assert(pool != NULL);
    assert(pcpatt != NULL);
    assert(patt != NULL);

    ib_status_t rc;
    ib_context_t *ctx;
    ib_module_t *module;
    modpcre_cfg_t *config;
    modpcre_cpat_data_t *cpdata;

    /* Get my module object */
    rc = ib_engine_module_get(mpr->ib, MODULE_NAME_STR, &module);
    if (rc != IB_OK) {
        ib_log_error(mpr->ib, "Failed to get pcre module object: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* All we have available is the main configuration */
    ctx = ib_context_main(mpr->ib);
    assert(ctx != NULL);

    /* Get the context configuration */
    rc = ib_context_module_config(ctx, module, &config);
    if (rc != IB_OK) {
        ib_log_error(mpr->ib, "Failed to get pcre module configuration: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    rc = pcre_compile_internal(mpr->ib,
                               pool,
                               config,
                               false,
                               &cpdata,
                               patt,
                               errptr,
                               erroffset);
    if ( (rc == IB_OK) && (cpdata != NULL) ) {
        *(modpcre_cpat_data_t **)pcpatt = cpdata;
    }
    else {
        *(modpcre_cpat_data_t **)pcpatt = NULL;
    }
    return rc;

}

/**
 * Provider instance match using pcre_exec.
 *
 * @param[in] mpr Provider instance.
 * @param[in] cpatt Callback data of type modpcre_cpat_data_t *.
 * @param[in] flags Flags used in rule compilation.
 * @param[in] data The subject to be checked.
 * @param[in] dlen The length of @a data.
 * @param[out] ctx User data for creation.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory allocation failures.
 *   - IB_ENOENT if a match was not found.
 *   - IB_EINVAL if an unexpected error is returned by @c pcre_exec.
 */
static ib_status_t modpcre_match_compiled(ib_provider_t *mpr,
                                          void *cpatt,
                                          ib_flags_t flags,
                                          const uint8_t *data,
                                          size_t dlen,
                                          void *ctx)
{
    assert(mpr != NULL);
    assert(cpatt != NULL);
    assert(data != NULL);

    modpcre_cpat_data_t *cpdata = (modpcre_cpat_data_t *)cpatt;
    assert(cpdata->is_dfa == false);

    int ec;
    const int ovector_sz = 30;
    int *ovector;


    ovector = (int *)malloc(ovector_sz*sizeof(*ovector));
    if (ovector == NULL) {
        return IB_EALLOC;
    }

    ec = pcre_exec(cpdata->cpatt, cpdata->edata,
                   (const char *)data, dlen,
                   0, 0, ovector, ovector_sz);

    free(ovector);

    if (ec >= 0) {
        return IB_OK;
    }
    else if (ec == PCRE_ERROR_NOMATCH) {
        return IB_ENOENT;
    }

    return IB_EINVAL;
}

static ib_status_t modpcre_add_pattern(ib_provider_inst_t *pi,
                                       void *cpatt)
{
    assert(pi != NULL);
    assert(cpatt != NULL);
    return IB_ENOTIMPL;
}

static ib_status_t modpcre_add_pattern_ex(ib_provider_inst_t *mpi,
                                          void *patterns,
                                          const char *patt,
                                          ib_void_fn_t callback,
                                          void *arg,
                                          const char **errptr,
                                          int *erroffset)
{
    assert(mpi != NULL);
    return IB_ENOTIMPL;
}

static ib_status_t modpcre_match(ib_provider_inst_t *mpi,
                                 ib_flags_t flags,
                                 const uint8_t *data,
                                 size_t dlen, void *ctx)
{
    assert(mpi != NULL);
    return IB_ENOTIMPL;
}

static IB_PROVIDER_IFACE_TYPE(matcher) modpcre_matcher_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,

    /* Provider Interface */
    modpcre_compile,
    modpcre_match_compiled,

    /* Provider Instance Interface */
    modpcre_add_pattern,
    modpcre_add_pattern_ex,
    modpcre_match
};

/**
 * @brief Create the PCRE operator.
 * @param[in] ib The IronBee engine (unused)
 * @param[in] ctx The current IronBee context (unused)
 * @param[in] rule Parent rule to the operator
 * @param[in,out] pool The memory pool into which @c op_inst->data
 *                will be allocated.
 * @param[in] pattern The regular expression to be built.
 * @param[out] op_inst The operator instance that will be populated by
 *             parsing @a pattern.
 *
 * @returns IB_OK on success or IB_EALLOC on any other type of error.
 */
static ib_status_t pcre_operator_create(ib_engine_t *ib,
                                        ib_context_t *ctx,
                                        const ib_rule_t *rule,
                                        ib_mpool_t *pool,
                                        const char *pattern,
                                        ib_operator_inst_t *op_inst)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(rule != NULL);
    assert(pool != NULL);
    assert(op_inst != NULL);

    modpcre_cpat_data_t *cpdata = NULL;
    modpcre_rule_data_t *rule_data = NULL;
    ib_module_t *module;
    modpcre_cfg_t *config;
    ib_status_t rc;
    const char *errptr;
    int erroffset;

    if (pattern == NULL) {
        ib_log_error(ib, "No pattern for %s operator", op_inst->op->name);
        return IB_EINVAL;
    }

    /* Get my module object */
    rc = ib_engine_module_get(ib, MODULE_NAME_STR, &module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get pcre module object: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Get the context configuration */
    rc = ib_context_module_config(ctx, module, &config);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get pcre module configuration: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Compile the pattern.  Note that the rule data is an alias for
     * the compiled pattern type */
    rc = pcre_compile_internal(ib,
                               pool,
                               config,
                               false,
                               &cpdata,
                               pattern,
                               &errptr,
                               &erroffset);
    if (rc != IB_OK) {
        return rc;
    }

    /* Allocate a rule data object, populate it */
    rule_data = ib_mpool_alloc(pool, sizeof(*rule_data));
    if (rule_data == NULL) {
        return IB_EALLOC;
    }
    rule_data->cpdata = cpdata;
    rule_data->id = NULL;           /* Not needed for rx rules */

    /* Rule data is an alias for the compiled pattern data */
    op_inst->data = rule_data;

    return rc;
}

/**
 * @brief Deinitialize the rule.
 * @param[in,out] op_inst The instance of the operator to be deallocated.
 *                Operator data is allocated out of the memory pool for
 *                IronBee so we do not destroy the operator here.
 *                pool for IronBee and need not be freed by us.
 * @returns IB_OK always.
 */
static ib_status_t pcre_operator_destroy(ib_operator_inst_t *op_inst)
{
    assert(op_inst != NULL);
    /* Nop */
    return IB_OK;
}

/**
 * Set the matches into the given field name as .0, .1, .2 ... .9.
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] ovector The vector of integer pairs of matches from PCRE.
 * @param[in] matches The number of matches.
 * @param[in] subject The matched-against string data.
 *
 * @returns IB_OK or IB_EALLOC.
 */
static ib_status_t pcre_set_matches(const ib_rule_exec_t *rule_exec,
                                    int *ovector,
                                    int matches,
                                    const char *subject)
{
    assert(rule_exec != NULL);
    assert(rule_exec->ib != NULL);
    assert(rule_exec->tx != NULL);
    assert(ovector != NULL);

    ib_status_t rc;
    ib_tx_t *tx = rule_exec->tx;
    int i;

    rc = ib_rule_capture_clear(rule_exec);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Error clearing captures: %s",
                        ib_status_to_string(rc));
    }

    /* We have a match! Now populate TX:0-9 in tx->data. */
    ib_log_debug2_tx(tx, "REGEX populating %d matches", matches);
    for (i = 0; i < matches; ++i)
    {
        /* The length of the match. */
        size_t match_len;

        /* The first character in the match. */
        const char *match_start;

        /* Field name */
        const char *name;

        /* Holder for a copy of the field value when creating a new field. */
        ib_bytestr_t *bs;

        /* Field holder. */
        ib_field_t *field;

        /* Readability. Mark the start and length of the string. */
        match_start = subject+ovector[i*2];
        match_len = ovector[i*2+1] - ovector[i*2];

        /* Create a byte-string representation */
        rc = ib_bytestr_dup_mem(&bs,
                                tx->mp,
                                (const uint8_t*)match_start,
                                match_len);
        if (rc != IB_OK) {
            return rc;
        }

        /* Create a field to hold the byte-string */
        name = ib_rule_capture_name(rule_exec, i);
        rc = ib_field_create(&field, tx->mp, name, strlen(name),
                             IB_FTYPE_BYTESTR, ib_ftype_bytestr_in(bs));
        if (rc != IB_OK) {
            return rc;
        }

        /* Add it to the capture collection */
        rc = ib_rule_capture_set_item(rule_exec, i, field);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Set the matches from a multi-match dfa as a list in the CAPTURE
 * collection (all with "0" key).
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] ovector The vector of integer pairs of matches from PCRE.
 * @param[in] matches The number of matches.
 * @param[in] subject The matched-against string data.
 *
 * @returns IB_OK or IB_EALLOC.
 */
static ib_status_t pcre_dfa_set_match(const ib_rule_exec_t *rule_exec,
                                      int *ovector,
                                      int matches,
                                      const char *subject)
{
    assert(rule_exec != NULL);
    assert(rule_exec->ib != NULL);
    assert(rule_exec->tx != NULL);
    assert(ovector != NULL);

    ib_tx_t *tx = rule_exec->tx;
    int i;

    /* We have a match! Now populate TX:0-9 in tx->data. */
    ib_log_debug2_tx(tx, "DFA populating %d matches", matches);
    for (i = 0; i < matches; ++i)
    {
        size_t match_len;
        const char *match_start;
        const char *name;
        ib_bytestr_t *bs;
        ib_field_t *field;
        ib_status_t rc;

        /* Readability. Mark the start and length of the string. */
        match_start = subject+ovector[i * 2];
        match_len = ovector[i * 2 + 1] - ovector[i * 2];

        /* Create a byte-string representation */
        rc = ib_bytestr_dup_mem(&bs,
                                tx->mp,
                                (const uint8_t*)match_start,
                                match_len);
        if (rc != IB_OK) {
            return rc;
        }

        /* Create a field to hold the byte-string */
        name = ib_rule_capture_name(rule_exec, 0);
        rc = ib_field_create(&field, tx->mp, name, strlen(name),
                             IB_FTYPE_BYTESTR, ib_ftype_bytestr_in(bs));
        if (rc != IB_OK) {
            return rc;
        }

        /* Add it to the capture collection */
        rc = ib_rule_capture_add_item(rule_exec, field);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/**
 * @brief Execute the PCRE operator
 *
 * @param[in] rule_exec The rule execution object
 * @param[in,out] data User data. A @c modpcre_rule_data_t.
 * @param[in] flags Operator instance flags
 * @param[in] field The field content.
 * @param[out] result The result.
 *
 * @returns IB_OK most times. IB_EALLOC when a memory allocation error handles.
 */
static ib_status_t pcre_operator_execute(const ib_rule_exec_t *rule_exec,
                                         void *data,
                                         ib_flags_t flags,
                                         ib_field_t *field,
                                         ib_num_t *result)
{
    assert(rule_exec != NULL);
    assert(rule_exec->ib != NULL);
    assert(rule_exec->tx != NULL);
    assert(rule_exec->tx->data != NULL);
    assert(data != NULL);

    int matches;
    ib_status_t ib_rc;
    const int ovecsize = 3 * MATCH_MAX;
    int *ovector = (int *)malloc(ovecsize*sizeof(*ovector));
    const char *subject = NULL;
    size_t subject_len = 0;
    const ib_bytestr_t *bytestr;
    modpcre_rule_data_t *rule_data = (modpcre_rule_data_t *)data;
    pcre_extra *edata = NULL;
#ifdef PCRE_JIT_STACK
    pcre_jit_stack *jit_stack = NULL;
#endif

    assert(rule_data->cpdata->is_dfa == false);

    if (ovector==NULL) {
        return IB_EALLOC;
    }

    if (field->type == IB_FTYPE_NULSTR) {
        ib_rc = ib_field_value(field, ib_ftype_nulstr_out(&subject));
        if (ib_rc != IB_OK) {
            free(ovector);
            return ib_rc;
        }

        if (subject != NULL) {
            subject_len = strlen(subject);
        }
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        ib_rc = ib_field_value(field, ib_ftype_bytestr_out(&bytestr));
        if (ib_rc != IB_OK) {
            free(ovector);
            return ib_rc;
        }

        if (bytestr != NULL) {
            subject_len = ib_bytestr_length(bytestr);
            subject = (const char *) ib_bytestr_const_ptr(bytestr);
        }
    }
    else {
        free(ovector);
        return IB_EINVAL;
    }

    if (subject == NULL) {
        subject     = "";
    }

    /* Debug block. Escapes a string and prints it to the log.
     * Memory is freed. */
    if (ib_log_get_level(rule_exec->ib) >= 9) {

        /* Worst case, we can have a string that is 4x larger.
         * Consider if a string of 0xF7 is passed.  That single character
         * will expand to a string of 4 printed characters +1 for the \0
         * character. */
        char *debug_str = ib_util_hex_escape(rule_exec->tx->mp,
                                             (const uint8_t *)subject,
                                             subject_len);

        if (debug_str != NULL) {
            ib_rule_log_trace(rule_exec, "Matching against: \"%s\"", debug_str);
        }
    }

    if (rule_data->cpdata->is_jit) {
#ifdef PCRE_JIT_STACK
        jit_stack = pcre_jit_stack_alloc(rule_data->cpdata->jit_stack_start,
                                         rule_data->cpdata->jit_stack_max);
        if (jit_stack == NULL) {
            ib_rule_log_debug(rule_exec,
                              "Failed to allocate a jit stack for a "
                              "jit-compiled rule.  "
                              "Not using jit for this call.");
        }
        /* If the study data is NULL or size zero, don't use it. */
        else if (rule_data->cpdata->study_data_sz > 0) {
            edata = rule_data->cpdata->edata;
        }
        if (edata != NULL) {
            pcre_assign_jit_stack(edata, NULL, jit_stack);
        }
#else
        edata = NULL;
#endif
    }
    else if (rule_data->cpdata->study_data_sz > 0) {
        edata = rule_data->cpdata->edata;
    }
    else {
        edata = NULL;
    }

    matches = pcre_exec(rule_data->cpdata->cpatt,
                        edata,
                        subject,
                        subject_len,
                        0, /* Starting offset. */
                        0, /* Options. */
                        ovector,
                        ovecsize);

#ifdef PCRE_JIT_STACK
    if (jit_stack != NULL) {
        pcre_jit_stack_free(jit_stack);
    }
#endif

    if (matches > 0) {
        if (ib_rule_should_capture(rule_exec, 1) ) {
            pcre_set_matches(rule_exec, ovector, matches, subject);
        }
        ib_rc = IB_OK;
        *result = 1;
    }
    else if (matches == PCRE_ERROR_NOMATCH) {

        if (ib_log_get_level(rule_exec->ib) >= 7) {
            char* tmp_c = malloc(subject_len+1);
            memcpy(tmp_c, subject, subject_len);
            tmp_c[subject_len] = '\0';
            /* No match. Return false to the caller (*result = 0). */
            ib_rule_log_trace(rule_exec,
                              "No match for \"%s\" using pattern \"%s\".",
                              tmp_c, rule_data->cpdata->patt);
            free(tmp_c);
        }


        ib_rc = IB_OK;
        *result = 0;
    }
    else {
        /* Some other error occurred. Set the status to false and
         * report the error. */
        ib_rule_log_error(rule_exec, "RX match failed (cpat=%p): %d",
                          (void *)rule_data->cpdata->cpatt, matches);
        ib_rc = IB_EUNKNOWN;
        *result = 0;
    }

    free(ovector);
    return ib_rc;
}

/**
 * Set the ID of a DFA rule.
 *
 * @param[in] rule Rule to use ID of (if available).
 * @param[in] op_inst Operator instance.
 * @param[in] mp Memory pool to use for allocations.
 * @param[in,out] rule_data DFA rule object to store ID into.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory failure.
 */
static ib_status_t dfa_id_set(const ib_rule_t *rule,
                              const ib_operator_inst_t *op_inst,
                              ib_mpool_t *mp,
                              modpcre_rule_data_t *rule_data)
{
    assert(rule != NULL);
    assert(op_inst != NULL);
    assert(mp != NULL);
    assert(rule_data != NULL);
    const char *rule_id;

    rule_id = ib_rule_id(rule);
    if (rule_id != NULL) {
        rule_data->id = rule_id;
        return IB_OK;
    }

    /* We compute the length of the string buffer as such:
     * +2 for the 0x prefix.
     * +1 for the \0 string terminations.
     * +16 for encoding 8 bytes (64 bits) as hex-pairs (2 chars / byte).
     */
    size_t id_sz = 16 + 2 + 1;
    char *id;
    id = ib_mpool_alloc(mp, id_sz+1);
    if (id == NULL) {
        return IB_EALLOC;
    }

    snprintf(id, id_sz, "%p", op_inst);
    rule_data->id = id;

    return IB_OK;
}

/**
 * @brief Create the PCRE operator.
 * @param[in] ib The IronBee engine (unused)
 * @param[in] ctx The current IronBee context (unused)
 * @param[in] rule Parent rule to the operator
 * @param[in,out] pool The memory pool into which @c op_inst->data
 *                will be allocated.
 * @param[in] pattern The regular expression to be built.
 * @param[out] op_inst The operator instance that will be populated by
 *             parsing @a pattern.
 *
 * @returns IB_OK on success or IB_EALLOC on any other type of error.
 */
static ib_status_t dfa_operator_create(ib_engine_t *ib,
                                       ib_context_t *ctx,
                                       const ib_rule_t *rule,
                                       ib_mpool_t *pool,
                                       const char *pattern,
                                       ib_operator_inst_t *op_inst)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(rule != NULL);
    assert(pool != NULL);
    assert(op_inst != NULL);

    modpcre_cpat_data_t *cpdata;
    modpcre_rule_data_t *rule_data;
    ib_module_t *module;
    modpcre_cfg_t *config;
    ib_status_t rc;
    const char *errptr;
    int erroffset;

    /* Get my module object */
    rc = ib_engine_module_get(ib, MODULE_NAME_STR, &module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get pcre module object: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Get the context configuration */
    rc = ib_context_module_config(ctx, module, &config);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get pcre module configuration: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    rc = pcre_compile_internal(ib,
                               pool,
                               config,
                               true,
                               &cpdata,
                               pattern,
                               &errptr,
                               &erroffset);

    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to parse DFA operator pattern \"%s\":%s",
                     pattern, ib_status_to_string(rc));
        return rc;
    }

    /* Allocate a rule data object, populate it */
    rule_data = ib_mpool_alloc(pool, sizeof(*rule_data));
    if (rule_data == NULL) {
        return IB_EALLOC;
    }
    rule_data->cpdata = cpdata;
    rc = dfa_id_set(rule, op_inst, pool, rule_data);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating ID for DFA: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    ib_log_debug(ib, "Compiled DFA id=\"%s\" operator pattern \"%s\" @ %p",
                 rule_data->id, pattern, (void *)cpdata->cpatt);

    op_inst->data = (void *)rule_data;
    return IB_OK;
}

/**
 * Get or create an ib_hash_t inside of @c tx for storing dfa rule data.
 *
 * The hash is stored at the key @c HASH_NAME_STR.
 *
 * @param[in] tx The transaction containing @c tx->data which holds
 *            the @a rule_data object.
 * @param[out] hash The fetched or created rule data hash. This is set
 *             to NULL on failure.
 *
 * @return
 *   - IB_OK on success.
 *   - IB_EALLOC on allocation failure
 */
static ib_status_t get_or_create_rule_data_hash(ib_tx_t *tx,
                                                ib_hash_t **hash)
{
    assert(tx);
    assert(tx->mp);

    ib_status_t rc;

    /* Get or create the hash that contains the rule data. */
    rc = ib_tx_get_module_data(tx, IB_MODULE_STRUCT_PTR, (void **)hash);
    if ( (rc == IB_OK) && (*hash != NULL) ) {
        ib_log_debug2_tx(tx, "Found rule data hash in tx.");
        return IB_OK;
    }

    ib_log_debug2_tx(tx, "Rule data hash did not exist in tx.");

    rc = ib_hash_create(hash, tx->mp);
    if (rc != IB_OK) {
        ib_log_debug2_tx(tx, "Failed to create hash: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    rc = ib_tx_set_module_data(tx, IB_MODULE_STRUCT_PTR, *hash);
    if (rc != IB_OK) {
        ib_log_debug2_tx(tx, "Failed to store hash: %s",
                         ib_status_to_string(rc));
        *hash = NULL;
    }

    ib_log_debug2_tx(tx, "Returning rule hash at %p.", *hash);

    return rc;

}

struct dfa_workspace_t {
    int *workspace;
    int wscount;
};
typedef struct dfa_workspace_t dfa_workspace_t;

/**
 * Create the per-transaction data for use with the dfa operator.
 *
 * @param[in,out] tx Transaction to store the value in.
 * @param[in] cpatt_data Compiled pattern data
 * @param[in] id The operator identifier used to get it's workspace.
 * @param[out] workspace Created.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on an allocation error.
 */
static ib_status_t alloc_dfa_tx_data(ib_tx_t *tx,
                                     const modpcre_cpat_data_t *cpatt_data,
                                     const char *id,
                                     dfa_workspace_t **workspace)
{
    assert(tx);
    assert(tx->mp);
    assert(id);
    assert(workspace);

    ib_hash_t *hash;
    ib_status_t rc;
    dfa_workspace_t *ws;
    size_t size;

    *workspace = NULL;
    rc = get_or_create_rule_data_hash(tx, &hash);
    if (rc != IB_OK) {
        return rc;
    }

    ws = (dfa_workspace_t *)ib_mpool_alloc(tx->mp, sizeof(*ws));
    if (ws == NULL) {
        return IB_EALLOC;
    }

    ws->wscount = cpatt_data->dfa_ws_size;
    size = sizeof(*(ws->workspace)) * (ws->wscount);
    ws->workspace = (int *)ib_mpool_alloc(tx->mp, size);
    if (ws->workspace == NULL) {
        return IB_EALLOC;
    }

    rc = ib_hash_set(hash, id, ws);
    if (rc == IB_OK) {
        *workspace = ws;
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
 *   - IB_ENOENT if the structure does not exist. Call alloc_dfa_tx_data then.
 *   - IB_EALLOC on an allocation error.
 */
static ib_status_t get_dfa_tx_data(ib_tx_t *tx,
                                   const char *id,
                                   dfa_workspace_t **workspace)
{
    assert(tx);
    assert(tx->mp);
    assert(id);
    assert(workspace);

    ib_hash_t *hash;
    ib_status_t rc;

    rc = get_or_create_rule_data_hash(tx, &hash);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hash_get(hash, workspace, id);
    if (rc != IB_OK) {
        *workspace = NULL;
    }

    return rc;
}

/**
 * @brief Execute the dfa operator
 *
 * @param[in] rule_exec The rule execution object
 * @param[in,out] data User data. A @c modpcre_rule_data_t.
 * @param[in] flags Operator instance flags
 * @param[in] field The field content.
 * @param[out] result The result.
 *
 * @returns IB_OK most times. IB_EALLOC when a memory allocation error handles.
 */
static ib_status_t dfa_operator_execute(const ib_rule_exec_t *rule_exec,
                                        void *data,
                                        ib_flags_t flags,
                                        ib_field_t *field,
                                        ib_num_t *result)
{
    assert(rule_exec);
    assert(rule_exec->tx != NULL);
    assert(rule_exec->ib != NULL);
    assert(data);


    ib_tx_t *tx = rule_exec->tx;
    int matches;
    ib_status_t ib_rc;
    const int ovecsize = 3 * MATCH_MAX;
    modpcre_rule_data_t *rule_data = (modpcre_rule_data_t *)data;
    int *ovector;
    const char *subject;
    size_t subject_len;
    const ib_bytestr_t *bytestr;
    dfa_workspace_t *dfa_workspace;
    const char *id = ib_rule_id(rule_exec->rule);
    int options; /* dfa exec options. */
    bool capture;
    int start_offset;
    int match_count;

    assert(rule_data->cpdata->is_dfa == true);

    ovector = (int *)malloc(ovecsize*sizeof(*ovector));
    if (ovector==NULL) {
        return IB_EALLOC;
    }

    if (field->type == IB_FTYPE_NULSTR) {
        ib_rc = ib_field_value(field, ib_ftype_nulstr_out(&subject));
        if (ib_rc != IB_OK) {
            free(ovector);
            return ib_rc;
        }

        subject_len = strlen(subject);
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        ib_rc = ib_field_value(field, ib_ftype_bytestr_out(&bytestr));
        if (ib_rc != IB_OK) {
            free(ovector);
            return ib_rc;
        }

        subject_len = ib_bytestr_length(bytestr);
        subject = (const char *) ib_bytestr_const_ptr(bytestr);
    }
    else {
        free(ovector);
        return IB_EINVAL;
    }

    /* Debug block. Escapes a string and prints it to the log.
     * Memory is freed. */
    if (ib_log_get_level(rule_exec->ib) >= 9) {

        /* Worst case, we can have a string that is 4x larger.
         * Consider if a string of 0xF7 is passed.  That single character
         * will expand to a string of 4 printed characters +1 for the \0
         * character. */
        char *debug_str = ib_util_hex_escape(rule_exec->tx->mp,
                                             (const uint8_t *)subject,
                                             subject_len);

        if (debug_str != NULL) {
            ib_log_debug3_tx(tx, "Matching against: \"%s\"", debug_str);
        }
    }

    /* Get the per-tx workspace data for this rule data id. */
    ib_rc = get_dfa_tx_data(tx, id, &dfa_workspace);
    if (ib_rc == IB_ENOENT) {
        /* First time we are called, clear the captures. */
        ib_rc = ib_rule_capture_clear(rule_exec);
        if (ib_rc != IB_OK) {
            ib_log_error_tx(tx, "Error clearing captures: %s",
                            ib_status_to_string(ib_rc));
        }

        options = PCRE_PARTIAL_SOFT;

        ib_rc = alloc_dfa_tx_data(tx, rule_data->cpdata, id, &dfa_workspace);
        if (ib_rc != IB_OK) {
            free(ovector);
            ib_rule_log_error(rule_exec,
                              "Error creating tx storage for dfa operator: %s",
                              ib_status_to_string(ib_rc));
            return ib_rc;
        }

        ib_rule_log_debug(rule_exec, "Created DFA workspace at %p.",
                          dfa_workspace);
    }
    else if (ib_rc == IB_OK) {
        options = PCRE_PARTIAL_SOFT | PCRE_DFA_RESTART;
        ib_rule_log_debug(rule_exec, "Reusing existing DFA workspace %p.",
                          dfa_workspace);
    }
    else {
        free(ovector);
        ib_rule_log_error(rule_exec,
                          "Error fetching dfa data for dfa operator: %s",
                          ib_status_to_string(ib_rc));
        return ib_rc;
    }

    /* Perform the match.
     * If capturing is specified, then find all matches.
     */
    capture = ib_rule_should_capture(rule_exec, 1);
    start_offset = 0;
    match_count = 0;
    do {
        matches = pcre_dfa_exec(rule_data->cpdata->cpatt,
                                rule_data->cpdata->edata,
                                subject,
                                subject_len,
                                start_offset, /* Starting offset. */
                                options,
                                ovector,
                                ovecsize,
                                dfa_workspace->workspace,
                                dfa_workspace->wscount);

        if (matches > 0) {
            ib_log_debug3_tx(tx, "DFA matched: %d", matches);

            ++match_count;

            /* Use the longest match - the first in ovector -
             * to set the offset in the subject for the next
             * match.
             */
            start_offset = ovector[1] + 1;
            if (capture) {
                pcre_dfa_set_match(rule_exec, ovector, 1, subject);
            }
        }
    } while (capture && (matches > 0));

    if (match_count > 0) {
        ib_rc = IB_OK;
        *result = 1;
    }
    else if ((matches == 0) || (matches == PCRE_ERROR_NOMATCH)) {
        if (ib_log_get_level(rule_exec->ib) >= 7) {
            char* tmp_c = malloc(subject_len+1);
            memcpy(tmp_c, subject, subject_len);
            tmp_c[subject_len] = '\0';
            /* No match. Return false to the caller (*result = 0). */
            ib_rule_log_debug(rule_exec,
                              "No match for \"%s\" using pattern \"%s\".",
                              tmp_c, rule_data->cpdata->patt);
            free(tmp_c);
        }

        ib_rc = IB_OK;
        *result = 0;
    }
    else if (matches == PCRE_ERROR_PARTIAL) {
        ib_log_debug3_tx(tx, "DFA matched partial: %d", matches);
        ib_rule_log_debug(rule_exec,
                          "Partial match found, but not a full match.");
        ib_rc = IB_OK;
        *result = 0;
    }
    else {
        /* Some other error occurred. Set the status to false and
        report the error. */
        ib_rule_log_error(rule_exec, "DFA match failed (cpat=%p): %d",
                          (void *)rule_data->cpdata->cpatt, matches);
        ib_rc = IB_EUNKNOWN;
        *result = 0;
    }

    free(ovector);
    return ib_rc;
}

/**
 * @brief Destroy the dfa operator
 *
 * @param[in,out] op_inst The operator instance
 *
 * @returns IB_OK
 */
static ib_status_t dfa_operator_destroy(ib_operator_inst_t *op_inst)
{
    assert(op_inst != NULL);

    /* Nop - Memory released by mpool. */

    return IB_OK;
}

/* -- Module Routines -- */

static IB_CFGMAP_INIT_STRUCTURE(config_map) = {
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".study",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        study
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".use_jit",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        use_jit
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".match_limit",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        match_limit
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".match_limit_recursion",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        match_limit_recursion
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".jit_stack_start",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        jit_stack_start
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".jit_stack_max",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        jit_stack_max
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".dfa_workspace_size",
        IB_FTYPE_NUM,
        modpcre_cfg_t,
        dfa_workspace_size
    ),
    IB_CFGMAP_INIT_LAST
};

/**
 * Handle on/off directives.
 *
 * @param[in] cp Config parser
 * @param[in] name Directive name
 * @param[in] onoff on/off flag
 * @param[in] cbdata Callback data (ignored)
 *
 * @returns Status code
 */
static ib_status_t handle_directive_onoff(ib_cfgparser_t *cp,
                                          const char *name,
                                          int onoff,
                                          void *cbdata)
{
    assert(cp != NULL);
    assert(name != NULL);
    assert(cp->ib != NULL);

    ib_engine_t *ib = cp->ib;
    ib_status_t rc;
    ib_module_t *module = NULL;
    modpcre_cfg_t *config = NULL;
    ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
    const char *pname;

    /* Get my module object */
    rc = ib_engine_module_get(cp->ib, MODULE_NAME_STR, &module);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module object: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    /* Get my module configuration */
    rc = ib_context_module_config(ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    if (strcasecmp("PcreStudy", name) == 0) {
        pname = MODULE_NAME_STR ".study";
    }
    else if (strcasecmp("PcreUseJit", name) == 0) {
        pname = MODULE_NAME_STR ".use_jit";
    }
    else {
        ib_cfg_log_error(cp, "Unhandled directive \"%s\"", name);
        return IB_EINVAL;
    }
    rc = ib_context_set_num(ctx, pname, onoff);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to set \"%s\" to %s for \"%s\": %s",
                         pname, onoff ? "true" : "false", name,
                         ib_status_to_string(rc));
    }
    return IB_OK;
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
static ib_status_t handle_directive_param(ib_cfgparser_t *cp,
                                          const char *name,
                                          const char *p1,
                                          void *cbdata)
{
    assert(cp != NULL);
    assert(name != NULL);
    assert(p1 != NULL);
    assert(cp->ib != NULL);

    ib_engine_t *ib = cp->ib;
    ib_status_t rc;
    ib_module_t *module = NULL;
    modpcre_cfg_t *config = NULL;
    ib_context_t *ctx = cp->cur_ctx ? cp->cur_ctx : ib_context_main(ib);
    const char *pname;
    ib_num_t value;

    /* Get my module object */
    rc = ib_engine_module_get(cp->ib, MODULE_NAME_STR, &module);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module object: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    /* Get my module configuration */
    rc = ib_context_module_config(ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    /* p1 should be a number */
    rc = ib_string_to_num(p1, 0, &value);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Failed to convert \"%s\" to a number for \"%s\": %s",
                         p1, name, ib_status_to_string(rc));
        return rc;
    }

    if (strcasecmp("PcreMatchLimit", name) == 0) {
        pname = "pcre.match_limit";
    }
    else if (strcasecmp("PcreMatchLimitRecursion", name) == 0) {
        pname = "pcre.match_limit_recursion";
    }
    else if (strcasecmp("PcreJitStackStart", name) == 0) {
        pname = "pcre.jit_stack_start";
    }
    else if (strcasecmp("PcreJitStackMax", name) == 0) {
        pname = "pcre.jit_stack_max";
    }
    else if (strcasecmp("PcreDfaWorkspaceSize", name) == 0) {
        pname = "pcre.dfa_workspace_size";
    }
    else {
        ib_cfg_log_error(cp, "Unhandled directive \"%s\"", name);
        return IB_EINVAL;
    }
    rc = ib_context_set_num(ctx, pname, value);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to set \"%s\" to %ld for \"%s\": %s",
                         pname, (long int)value, name, ib_status_to_string(rc));
    }
    return IB_OK;
}

static IB_DIRMAP_INIT_STRUCTURE(directive_map) = {
    IB_DIRMAP_INIT_ONOFF(
        "PcreStudy",
        handle_directive_onoff,
        NULL
    ),
    IB_DIRMAP_INIT_ONOFF(
        "PcreUseJit",
        handle_directive_onoff,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "PcreMatchLimit",
        handle_directive_param,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "PcreMatchLimitRecursion",
        handle_directive_param,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "PcreJitStackStart",
        handle_directive_param,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "PcreJitStackMax",
        handle_directive_param,
        NULL
    ),
    IB_DIRMAP_INIT_PARAM1(
        "PcreDfaWorkspaceSize",
        handle_directive_param,
        NULL
    ),
    IB_DIRMAP_INIT_LAST
};

static ib_status_t modpcre_init(ib_engine_t *ib,
                                ib_module_t *m,
                                void        *cbdata)
{
    assert(ib != NULL);
    assert(m != NULL);
    ib_status_t rc;

    /* Register as a matcher provider. */
    rc = ib_provider_register(ib,
                              IB_PROVIDER_TYPE_MATCHER,
                              MODULE_NAME_STR,
                              NULL,
                              &modpcre_matcher_iface,
                              NULL);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     MODULE_NAME_STR
                     ": Error registering pcre matcher provider: "
                     "%s", ib_status_to_string(rc));
        return IB_OK;
    }

    ib_log_debug(ib, "PCRE Status: compiled=\"%d.%d %s\" loaded=\"%s\"",
        PCRE_MAJOR, PCRE_MINOR, IB_XSTRINGIFY(PCRE_DATE), pcre_version());

    /* Register operators. */
    ib_operator_register(ib,
                         "pcre",
                         (IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE),
                         pcre_operator_create,
                         NULL,
                         pcre_operator_destroy,
                         NULL,
                         pcre_operator_execute,
                         NULL);

    /* An alias of pcre. The same callbacks are registered. */
    ib_operator_register(ib,
                         "rx",
                         (IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE),
                         pcre_operator_create,
                         NULL,
                         pcre_operator_destroy,
                         NULL,
                         pcre_operator_execute,
                         NULL);

    /* Register a pcre operator that uses pcre_dfa_exec to match streams. */
    ib_operator_register(ib,
                         "dfa",
                         (IB_OP_FLAG_PHASE | IB_OP_FLAG_STREAM | IB_OP_FLAG_CAPTURE),
                         dfa_operator_create,
                         NULL,
                         dfa_operator_destroy,
                         NULL,
                         dfa_operator_execute,
                         NULL);

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
    IB_MODULE_CONFIG(&modpcre_global_cfg),/**< Global config data */
    config_map,                           /**< Configuration field map */
    directive_map,                        /**< Config directive map */
    modpcre_init,                         /**< Initialize function */
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
