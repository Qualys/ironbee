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

#ifndef _IB_CONFIG_H_
#define _IB_CONFIG_H_

/**
 * @file
 * @brief IronBee --- Configuration Functions
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include <ironbee/build.h>
#include <ironbee/cfgmap.h>
#include <ironbee/engine.h>
#include <ironbee/log.h>
#include <ironbee/strval.h>
#include <ironbee/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeConfig Configuration
 * @ingroup IronBee
 *
 * Code related to parsing and interpreting the configuration file.
 *
 * @{
 */

typedef struct ib_dirmap_init_t ib_dirmap_init_t;

/// @todo Should probably be private structure
struct ib_cfgparser_t {
    ib_engine_t   *ib;                /**< Engine */
    ib_mpool_t    *mp;                /**< Memory pool */
    ib_list_t     *stack;             /**< Stack tracking contexts */
    ib_list_t     *block;             /**< Stack tracking blocks */
    ib_hash_t     *includes;          /**< Include tracking hash */
    const char    *linebuf;           /**< Line buffer, used for continuation */

    /* Parsing states */
    ib_context_t  *cur_ctx;           /**< Current context */
    const char    *cur_blkname;       /**< Current block name */
    const char    *cur_file;          /**< Current file name */
    const char    *cur_cwd;           /**< Directory of the current file */
    unsigned int   cur_lineno;        /**< Current line number */
};

/**
 * Directive types.
 */
typedef enum {
    IB_DIRTYPE_ONOFF,                    /**< Boolean param directive */
    IB_DIRTYPE_PARAM1,                   /**< One param directive */
    IB_DIRTYPE_PARAM2,                   /**< Two param directive */
    IB_DIRTYPE_LIST,                     /**< List param directive */
    IB_DIRTYPE_OPFLAGS,                  /**< Option flags directive */
    IB_DIRTYPE_SBLK1,                    /**< One param subblock directive */
} ib_dirtype_t;

/** Callback for ending (processing) a block */
typedef ib_status_t (*ib_config_cb_blkend_fn_t)(ib_cfgparser_t *cp,
                                                const char *name,
                                                void *cbdata);

/** Callback for ONOFF directives */
typedef ib_status_t (*ib_config_cb_onoff_fn_t)(ib_cfgparser_t *cp,
                                               const char *name,
                                               int onoff,
                                               void *cbdata);

/** Callback for PARAM1 directives */
typedef ib_status_t (*ib_config_cb_param1_fn_t)(ib_cfgparser_t *cp,
                                                const char *name,
                                                const char *p1,
                                                void *cbdata);

/** Callback for PARAM2 directives */
typedef ib_status_t (*ib_config_cb_param2_fn_t)(ib_cfgparser_t *cp,
                                                const char *name,
                                                const char *p1,
                                                const char *p2,
                                                void * cbdata);

/** Callback for LIST directives */
typedef ib_status_t (*ib_config_cb_list_fn_t)(ib_cfgparser_t *cp,
                                              const char *name,
                                              const ib_list_t *list,
                                              void *cbdata);

/** Callback for OPFLAGS directives */
typedef ib_status_t (*ib_config_cb_opflags_fn_t)(ib_cfgparser_t *cp,
                                                 const char *name,
                                                 ib_flags_t val,
                                                 ib_flags_t mask,
                                                 void *cbdata);

/** Callback for SBLK1 directives */
typedef ib_status_t (*ib_config_cb_sblk1_fn_t)(ib_cfgparser_t *cp,
                                               const char *name,
                                               const char *p1,
                                               void *cbdata);

/**
 * Directive initialization mapping structure.
 */
struct ib_dirmap_init_t {
    const char                   *name;      /**< Directive name */
    ib_dirtype_t                  type;      /**< Directive type */
    union {
        /** @cond internal */
        /**
         * C90/C++ always initializes the first member vs C99 which has
         * designated initializers, so in order to support initialization
         * there must be a generic function as the first member.
         */
        ib_void_fn_t              _init;
        /** @endcond */
        ib_config_cb_onoff_fn_t   fn_onoff;  /**< On|Off directive */
        ib_config_cb_param1_fn_t  fn_param1; /**< 1 param directive */
        ib_config_cb_param2_fn_t  fn_param2; /**< 2 param directive */
        ib_config_cb_list_fn_t    fn_list;   /**< List directive */
        ib_config_cb_opflags_fn_t fn_opflags;/**< Option flags directive */
        ib_config_cb_sblk1_fn_t   fn_sblk1;  /**< 1 param subblock directive */
    } cb;
    ib_config_cb_blkend_fn_t      fn_blkend; /**< Called when block ends */
    void                         *cbdata_cb; /**< Callback data for cb. */
    /*! Callback data for blkend. */
    void                         *cbdata_blkend;
    ib_strval_t                  *valmap;    /**< Value map */
    /// @todo Do we need help text or error messages???
};


/** @cond Internal */
/**
 * Helper macro to use designated initializers to typecheck @a cb argument to
 * @c IB_DIRMAP_INIT_X macros.  If we are in C, uses designated initializers,
 * but if we are in C++, uses generic @c _init member.
 **/
#ifdef __cplusplus
#define IB_DIRMAP_INIT_CB_HELPER(name, cb) { (ib_void_fn_t)(cb) }
#else
#define IB_DIRMAP_INIT_CB_HELPER(name, cb) { .name = (cb) }
#endif
/** @endcond */

/**
 * Defines a configuration directive map initialization structure.
 *
 * @param name Name of structure
 */
#define IB_DIRMAP_INIT_STRUCTURE(name) const ib_dirmap_init_t name[]


/**
 * Directive with a single On/Off/True/False/Yes/No parameter.
 *
 * @param name Directive name
 * @param cb Callback
 * @param cbdata Callback data
 */
#define IB_DIRMAP_INIT_ONOFF(name, cb, cbdata) \
    { (name), IB_DIRTYPE_ONOFF, IB_DIRMAP_INIT_CB_HELPER(fn_onoff, (cb)), NULL, (cbdata), NULL, NULL }

/**
 * Directive with a single string parameter.
 *
 * @param name Directive name
 * @param cb Callback
 * @param cbdata Callback data
 */
#define IB_DIRMAP_INIT_PARAM1(name, cb, cbdata) \
    { (name), IB_DIRTYPE_PARAM1, IB_DIRMAP_INIT_CB_HELPER(fn_param1, (cb)), NULL, (cbdata), NULL, NULL }

/**
 * Directive with two string parameters.
 *
 * @param name Directive name
 * @param cb Callback
 * @param cbdata Callback data
 */
#define IB_DIRMAP_INIT_PARAM2(name, cb, cbdata) \
    { (name), IB_DIRTYPE_PARAM2, IB_DIRMAP_INIT_CB_HELPER(fn_param2, (cb)), NULL, (cbdata), NULL, NULL }

/**
 * Directive with list of string parameters.
 *
 * @param name Directive name
 * @param cb Callback
 * @param cbdata Callback data
 */
#define IB_DIRMAP_INIT_LIST(name, cb, cbdata) \
    { (name), IB_DIRTYPE_LIST, IB_DIRMAP_INIT_CB_HELPER(fn_list, (cb)), NULL, (cbdata), NULL, NULL }

/**
 * Directive with list of unique options string parameters which are
 * converted flags (bitmask) in a single @ref ib_num_t value.
 *
 * Options can be explicit, or can add/remove from current value (those
 * prefixed with '-' are removed and '+' added).
 *
 * EX: DirectiveName [+|-]option ...
 *
 * @param name Directive name
 * @param cb Callback
 * @param cbdata Callback data
 * @param valmap Array of @ref ib_strval_t structures mapping options to values
 */
#define IB_DIRMAP_INIT_OPFLAGS(name, cb, cbdata, valmap) \
    { (name), IB_DIRTYPE_OPFLAGS, IB_DIRMAP_INIT_CB_HELPER(fn_opflags, (cb)), NULL, (cbdata), NULL, (valmap) }

/**
 * Block with single parameter enclosing more directives.
 *
 * @todo Should probably move blkend param to the end.
 *
 * @param name Directive name
 * @param cb Callback
 * @param blkend Block end callback
 * @param cbdata Callback data for @a cb
 * @param blkenddata Callback data for @a blkend
 */
#define IB_DIRMAP_INIT_SBLK1(name, cb, blkend, cbdata, blkenddata) \
    { (name), IB_DIRTYPE_SBLK1, IB_DIRMAP_INIT_CB_HELPER(fn_sblk1, (cb)), (blkend), (cbdata), (blkenddata), NULL }

/** Required last entry. */
#define IB_DIRMAP_INIT_LAST { .name = NULL }


/**
 * Create a new configuration parser.
 *
 * @param pcp Address where config parser handle will be written
 * @param ib Engine
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_create(ib_cfgparser_t **pcp,
                                           ib_engine_t *ib);

/// @todo Create a ib_cfgparser_parse_ex that can parse non-files (DBs, etc)

/**
 * Open @a file and parse it.
 *
 * Parsing is done by reading in 8k at a time and passing 1 line
 * at a time to ib_cfgparser_ragel_parse_chunk. Should the buffer
 * fill up, the current line is shifted down the first index in
 * the buffer. If a line is larger than ~8k, it is an error and parsing
 * fails.
 *
 * @param[in,out] cp The configuration parser to be used and populated.
 * @param[in] file The file to be opened and read.
 * @returns IB_OK on success or another values on failure. Errors are logged.
 */
ib_status_t DLL_PUBLIC ib_cfgparser_parse(ib_cfgparser_t *cp,
                                          const char *file);

/**
 * Parse @a buffer.
 *
 * @param[in,out] cp     The configuration parser to be used and populated.
 * @param[in]     buffer The buffer to parser
 * @param[in]     length Length of @a buffer.
 * @param[in]     file   Name of configuration file being parsed
 * @param[in]     lineno Line number of text in @a buffer
 * @param[in]     more   Use true if more data available and false otherwise.
 * @returns IB_OK on success and error code on failure.
 **/
ib_status_t DLL_PUBLIC ib_cfgparser_parse_buffer(ib_cfgparser_t *cp,
                                                 const char     *buffer,
                                                 size_t          length,
                                                 const char     *file,
                                                 unsigned        lineno,
                                                 bool            more);

/**
 * Push a new context onto the stack and make it the current.
 *
 * @param cp Parser
 * @param ctx New context
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_context_push(ib_cfgparser_t *cp,
                                                 ib_context_t *ctx);

/**
 * Pop the current context off the stack and make the previous the current.
 *
 * @param cp Parser
 * @param pctx Address which the removed context will be written (if non-NULL)
 * @param pcctx Address of the now current context (if non-NULL)
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_context_pop(ib_cfgparser_t *cp,
                                                ib_context_t **pctx,
                                                ib_context_t **pcctx);

/**
 * Get the current context off the stack
 *
 * @param cp Parser
 * @param pctx Address which the current context will be written
 *
 * @returns Status code (IB_OK)
 */
ib_status_t ib_cfgparser_context_current(const ib_cfgparser_t *cp,
                                         ib_context_t **pctx);

/**
 * Push a new block name onto the stack and make it the current.
 *
 * @param cp Parser
 * @param name New block name
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_block_push(ib_cfgparser_t *cp,
                                               const char *name);

/**
 * Pop the current block name off the stack and make the previous the current.
 *
 * @param cp Parser
 * @param pname Address which the removed name will be written (if non-NULL)
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_block_pop(ib_cfgparser_t *cp,
                                              const char **pname);

/**
 * Destroy the parser.
 *
 * @param cp Parser
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_cfgparser_destroy(ib_cfgparser_t *cp);


/**
 * Register directives with the engine.
 *
 * @param ib Engine
 * @param init Directive mapping
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_config_register_directives(ib_engine_t *ib,
                                                     const ib_dirmap_init_t *init);

/**
 * Register a directive with the engine.
 *
 * Primarily this is meant for non-C languages that cannot easily build
 * the map structure for @ref ib_config_register_directives.
 *
 * @param ib Engine
 * @param name Directive name
 * @param type Directive type
 * @param fn_config Callback function handling the config
 * @param fn_blkend Callback function called at the end of a block (or NULL)
 * @param cbdata_config Data passed to @a fn_config
 * @param cbdata_blkend Data passed to @a fn_blkend
 * @param valmap        Value map for opflags directives.
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_config_register_directive(
     ib_engine_t              *ib,
     const char               *name,
     ib_dirtype_t              type,
     ib_void_fn_t              fn_config,
     ib_config_cb_blkend_fn_t  fn_blkend,
     void                     *cbdata_config,
     void                     *cbdata_blkend,
     ib_strval_t              *valmap
);

/**
 * Process a directive.
 *
 * @param cp Config parser
 * @param name Directive name
 * @param args Directive arguments
 * @todo Need to pass back an error msg???
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_config_directive_process(ib_cfgparser_t *cp,
                                                   const char *name,
                                                   ib_list_t *args);

/**
 * Start a block.
 *
 * @param cp Config parser
 * @param name Block name
 * @param args Block arguments
 * @todo Need to pass back an error msg???
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_config_block_start(ib_cfgparser_t *cp,
                                             const char *name,
                                             ib_list_t *args);

/**
 * Process a block.
 *
 * This is called when the end of a block is reached. Any arguments
 * must be saved when @ref ib_config_block_start() is called when the
 * block started.
 *
 * @param cp Config parser
 * @param name Block name
 * @todo Need to pass back an error msg???
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_config_block_process(ib_cfgparser_t *cp,
                                               const char *name);

/**
 * Lookup a name/value pair mapping
 *
 * @param[in] str String to lookup in @a map
 * @param[in] map String / value mapping
 * @param[out] pval Matching value
 *
 * @returns IB_OK, IB_EINVAL if @a string not found in @a map
 */
ib_status_t DLL_PUBLIC ib_config_strval_pair_lookup(const char *str,
                                                    const ib_strval_t *map,
                                                    ib_num_t *pval);

/**
 * Log Generic (Configuration form)
 */

/** @cond internal */
/**
 * Log configuration data.
 *
 * @sa ib_cfg_log()
 *
 * @param[in] cp Configuration parser
 * @param[in] level Log level
 * @param[in] file Source file name
 * @param[in] line Source file line number
 * @param[in] fmt printf-style format string
 */
void DLL_PUBLIC ib_cfg_log_f(ib_cfgparser_t *cp,
                             ib_log_level_t level,
                             const char *file,
                             int line,
                             const char *fmt, ...)
                             PRINTF_ATTRIBUTE(5, 6);
/** @endcond */

/**
 * Log configuration data (variable args version)
 *
 * @param[in] cp Configuration parser
 * @param[in] level Log level
 * @param[in] file Source file name
 * @param[in] line Source file line number
 * @param[in] fmt printf-style format string
 * @param[in] ap Variable args list
 */
void DLL_PUBLIC ib_cfg_vlog(ib_cfgparser_t *cp,
                            ib_log_level_t level,
                            const char *file,
                            int line,
                            const char *fmt,
                            va_list ap)
                            VPRINTF_ATTRIBUTE(5);

/** @cond internal */
/**
 * Log configuration data (ex version).
 *
 * @sa ib_cfg_log_ex()
 *
 * @param[in] ib IronBee engine
 * @param[in] cfgfile Configuration file name
 * @param[in] cfgline Configuration file line number
 * @param[in] level Log level
 * @param[in] file Source file name
 * @param[in] line Source file line number
 * @param[in] fmt printf-style format string
 */
void DLL_PUBLIC ib_cfg_log_ex_f(const ib_engine_t *ib,
                                const char *cfgfile, unsigned int cfgline,
                                ib_log_level_t level,
                                const char *file, int line,
                                const char *fmt, ...)
                                PRINTF_ATTRIBUTE(7, 8);
/** @endcond */

/**
 * Log configuration data (variable args / ex version)
 *
 * @param[in] ib IronBee engine
 * @param[in] cfgfile Configuration file name
 * @param[in] cfgline Configuration file line number
 * @param[in] level Log level
 * @param[in] file Source file name
 * @param[in] line Source file line number
 * @param[in] fmt printf-style format string
 * @param[in] ap Variable args list
 */
void DLL_PUBLIC ib_cfg_vlog_ex(const ib_engine_t *ib,
                               const char *cfgfile, unsigned int cfgline,
                               ib_log_level_t level,
                               const char *file, int line,
                               const char *fmt, va_list ap)
                               VPRINTF_ATTRIBUTE(7);


/** Log (Configuration form) */
#define ib_cfg_log(cp, level, ...) ib_cfg_log_f((cp), (level), __FILE__, __LINE__, __VA_ARGS__)
/** Log Emergency (Configuration form) */
#define ib_cfg_log_emergency(cp, ...) \
    ib_cfg_log((cp), IB_LOG_EMERGENCY, __VA_ARGS__)
/** Log Alert (Configuration form) */
#define ib_cfg_log_alert(cp, ...) \
    ib_cfg_log((cp), IB_LOG_ALERT, __VA_ARGS__)
/** Log Critical (Configuration form) */
#define ib_cfg_log_critical(cp, ...) \
    ib_cfg_log((cp), IB_LOG_CRITICAL, __VA_ARGS__)
/** Log Error (Configuration form) */
#define ib_cfg_log_error(cp, ...) \
    ib_cfg_log((cp), IB_LOG_ERROR, __VA_ARGS__)
/** Log Warning (Configuration form) */
#define ib_cfg_log_warning(cp, ...) \
    ib_cfg_log((cp), IB_LOG_WARNING, __VA_ARGS__)
/** Log Notice (Configuration form) */
#define ib_cfg_log_notice(cp, ...) \
    ib_cfg_log((cp), IB_LOG_NOTICE, __VA_ARGS__)
/** Log Info (Configuration form) */
#define ib_cfg_log_info(cp, ...) \
    ib_cfg_log((cp), IB_LOG_INFO, __VA_ARGS__)
/** Log Debug (Configuration form) */
#define ib_cfg_log_debug(cp, ...) \
    ib_cfg_log((cp), IB_LOG_DEBUG, __VA_ARGS__)
/** Log Debug2 (Configuration form) */
#define ib_cfg_log_debug2(cp, ...) \
    ib_cfg_log((cp), IB_LOG_DEBUG2, __VA_ARGS__)
/** Log Debug3 (Configuration form) */
#define ib_cfg_log_debug3(cp, ...) \
    ib_cfg_log((cp), IB_LOG_DEBUG3, __VA_ARGS__)
/** Log Trace (Configuration form) */
#define ib_cfg_log_trace(cp, ...) \
    ib_cfg_log((cp), IB_LOG_TRACE, __VA_ARGS__)

/** Log (Configuration / ex form) */
#define ib_cfg_log_ex(ib, cfgfile, cfgline, level, ...) \
    ib_cfg_log_ex_f((ib), (cfgfile), (cfgline), (level), __FILE__, __LINE__, __VA_ARGS__)
/** Log Emergency (Configuration / ex form) */
#define ib_cfg_log_emergency_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_EMERGENCY, __VA_ARGS__)
/** Log Alert (Configuration / ex form) */
#define ib_cfg_log_alert_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_ALERT, __VA_ARGS__)
/** Log Critical (Configuration / ex form) */
#define ib_cfg_log_critical_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_CRITICAL, __VA_ARGS__)
/** Log Error (Configuration / ex form) */
#define ib_cfg_log_error_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_ERROR, __VA_ARGS__)
/** Log Warning (Configuration / ex form) */
#define ib_cfg_log_warning_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_WARNING, __VA_ARGS__)
/** Log Notice (Configuration / ex form) */
#define ib_cfg_log_notice_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_NOTICE, __VA_ARGS__)
/** Log Info (Configuration / ex form) */
#define ib_cfg_log_info_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_INFO, __VA_ARGS__)
/** Log Debug (Configuration / ex form) */
#define ib_cfg_log_debug_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_DEBUG, __VA_ARGS__)
/** Log Debug2 (Configuration / ex form) */
#define ib_cfg_log_debug2_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_DEBUG2, __VA_ARGS__)
/** Log Debug3 (Configuration / ex form) */
#define ib_cfg_log_debug3_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_DEBUG3, __VA_ARGS__)
/** Log Trace (Configuration / ex form) */
#define ib_cfg_log_trace_ex(ib, cfgfile, cfgline, ...) \
    ib_cfg_log_ex((ib), (cfgfile), (cfgline), IB_LOG_TRACE, __VA_ARGS__)

/**
 * @} IronBeeConfig
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_CONFIG_H_ */
