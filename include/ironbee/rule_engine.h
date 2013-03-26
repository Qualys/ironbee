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

#ifndef _IB_RULE_ENGINE_H_
#define _IB_RULE_ENGINE_H_

/**
 * @file
 * @brief IronBee --- Rule engine definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/action.h>
#include <ironbee/build.h>
#include <ironbee/config.h>
#include <ironbee/operator.h>
#include <ironbee/rule_defs.h>
#include <ironbee/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
* @defgroup IronBeeRule Rule Engine
* @ingroup IronBeeEngine
*
* The rule engine supports writing rules that trigger on certain inputs
* and execute actions as a result.
*
* @{
*/


/**
 * Rule meta data
 */
typedef struct {
    const char            *id;              /**< Rule ID */
    const char            *full_id;         /**< Rule's "full" ID */
    const char            *chain_id;        /**< Rule's chain ID */
    const char            *msg;             /**< Rule message */
    const char            *data;            /**< Rule logdata */
    ib_list_t             *tags;            /**< Rule tags */
    ib_rule_phase_num_t    phase;           /**< Phase number */
    uint8_t                severity;        /**< Rule severity */
    uint8_t                confidence;      /**< Rule confidence */
    uint16_t               revision;        /**< Rule revision # */
    ib_flags_t             flags;           /**< Rule meta-data flags */
    const char            *config_file;     /**< File rule defined in */
    unsigned int           config_line;     /**< Line number of rule def */
} ib_rule_meta_t;

/**
 * Rule phase meta data
 */
typedef struct ib_rule_phase_meta_t ib_rule_phase_meta_t;

/**
 * Basic rule object.
 */
struct ib_rule_t {
    ib_rule_meta_t         meta;            /**< Rule meta data */
    const ib_rule_phase_meta_t *phase_meta; /**< Phase meta data */
    ib_operator_inst_t    *opinst;          /**< Rule operator */
    ib_list_t             *target_fields;   /**< List of target fields */
    ib_list_t             *true_actions;    /**< Actions if condition True */
    ib_list_t             *false_actions;   /**< Actions if condition False */
    ib_list_t             *parent_rlist;    /**< Parent rule list */
    ib_context_t          *ctx;             /**< Parent context */
    ib_rule_t             *chained_rule;    /**< Next rule in the chain */
    ib_rule_t             *chained_from;    /**< Ptr to rule chained from */
    ib_flags_t             flags;           /**< External, etc. */
};

/**
 * Rule engine parser data
 */
typedef struct {
    ib_rule_t             *previous;     /**< Previous rule parsed */
} ib_rule_parser_data_t;

/**
 * Rule execution data
 */
struct ib_rule_exec_t {
    ib_engine_t            *ib;          /**< The IronBee engine */
    ib_tx_t                *tx;          /**< The executing transaction */
    ib_rule_phase_num_t     phase;       /**< The phase being executed */
    bool                    is_stream;   /**< Is this a stream rule phase? */
    ib_rule_t              *rule;        /**< The currently executing rule */
    ib_rule_target_t       *target;      /**< The current rule target */
    ib_num_t                result;      /**< Rule execution result */

    /* Logging objects */
    ib_rule_log_tx_t       *tx_log;      /**< Rule TX logging object */
    ib_rule_log_exec_t     *exec_log;    /**< Rule execution logging object */

    /* The below members are for rule engine internal use only, and should
     * never be accessed by actions, injection functions, etc. */

    /* Rule stack (for chains) */
    ib_list_t              *rule_stack;  /**< Stack of rules */

    /* List of all rules to run during the current phase. */
    ib_list_t              *phase_rules; /**< List of ib_rule_t */

    /* Stack of values for the FIELD* targets */
    ib_list_t              *value_stack; /**< Stack of values */
};

/**
 * External rule driver function.
 *
 * Function is passed configuration parser, tag, location, and callback data.
 *
 * @param[in] cp Configuration parser
 * @param[in] rule Rule being processed
 * @param[in] tag Driver tag
 * @param[in] location Location of rule file
 * @param[in] cbdata Generic callback data
 */
typedef ib_status_t (*ib_rule_driver_fn_t)(
    ib_cfgparser_t             *cp,
    ib_rule_t                  *rule,
    const char                 *tag,
    const char                 *location,
    void                       *cbdata
);

/**
 * A driver is simply a function and its callback data.
 */
typedef struct {
    ib_rule_driver_fn_t     function;    /**< Driver function */
    void                   *cbdata;      /**< Driver callback data */
} ib_rule_driver_t;

/**
 * External rule ownership function, invoked during close of context.
 *
 * This function will be called during the rule selection process.  This can,
 * by returning IB_OK, inform the rule engine that the function is taking
 * ownership of @a rule, and that the rule engine should not schedule @a rule
 * to run.  Typically, a module will schedule @a rule, or one or more rules
 * in its stead, via the injection function (ib_rule_injection_fn_t and
 * ib_rule_register_injection_fn).
 *
 * @param[in] ib IronBee engine
 * @param[in] rule Rule being registered
 * @param[in] cbdata Registration function callback data
 *
 * @returns Status code:
 *   - IB_OK All OK, rule managed externally by module
 *   - IB_DECLINE Decline to manage rule
 *   - IB_Exx Other error
 */
typedef ib_status_t (* ib_rule_ownership_fn_t)(
    const ib_engine_t          *ib,
    const ib_rule_t            *rule,
    void                       *cbdata
);

/**
 * External rule injection function
 *
 * This function will be called at the start of each phase.  This gives a
 * module the opportunity to inject one or more rules into the start of the
 * phase.  It does this by appending rules, in the form of ib_rule_t pointers,
 * to @a rule_list.  @a rule_list may contain rules upon entry to this
 * function and should thus treat @a rule_list as append-only.
 *
 * @note Returning an error will cause the rule engine to abort the current
 * phase processing.
 *
 * @param[in] ib IronBee engine
 * @param[in] rule_exec Rule execution environment
 * @param[in,out] rule_list List of rules to execute (append-only)
 * @param[in] cbdata Injection function callback data
 *
 * @returns Status code:
 *   - IB_OK All OK
 *   - IB_Exx Other error
 */
typedef ib_status_t (* ib_rule_injection_fn_t)(
    const ib_engine_t          *ib,
    const ib_rule_exec_t       *rule_exec,
    ib_list_t                  *rule_list,
    void                       *cbdata
);

/**
 * Set a rule engine value (for configuration)
 *
 * @param[in] cp Configuration parser
 * @param[in] name Name of parameter
 * @param[in] value Value to set to
 *
 * @returns IB_OK / IB_EINVAL
 */
ib_status_t DLL_PUBLIC ib_rule_engine_set(
    ib_cfgparser_t             *cp,
    const char                 *name,
    const char                 *value);

/**
 * Register external rule driver.
 *
 * @param ib       Engine.
 * @param tag      Driver tag; NUL terminated.
 * @param driver   Driver function.
 * @param cbdata   Driver callback data.
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_register_external_driver(
    ib_engine_t                *ib,
    const char                 *tag,
    ib_rule_driver_fn_t         driver,
    void                       *cbdata);

/**
 * Lookup an external rule driver
 *
 * @param[in] ib IronBee engine
 * @param[in] tag Driver tag
 * @param[out] pdriver Pointer to driver
 *
 * @returns Status code
 *    - IB_OK All ok
 *    - Errors from @sa ib_hash_get()
 */
ib_status_t DLL_PUBLIC ib_rule_lookup_external_driver(
    const ib_engine_t          *ib,
    const char                 *tag,
    ib_rule_driver_t          **pdriver);

/**
 * Register a rule ownership function.
 *
 * @param[in] ib IronBee engine.
 * @param[in] name Logical name (for logging)
 * @param[in] ownership_fn Ownership hook function
 * @param[in] cbdata Registration hook callback data
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_register_ownership_fn(
    ib_engine_t            *ib,
    const char             *name,
    ib_rule_ownership_fn_t  ownership_fn,
    void                   *cbdata);

/**
 * Register a rule injection function.
 *
 * @param[in] ib IronBee engine.
 * @param[in] name Logical name (for logging)
 * @param[in] phase Phase execution phase to hook into
 * @param[in] injection_fn Injection function
 * @param[in] cbdata Injection callback data
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_register_injection_fn(
    ib_engine_t            *ib,
    const char             *name,
    ib_rule_phase_num_t     phase,
    ib_rule_injection_fn_t  injection_fn,
    void                   *cbdata);

/**
 * Create a rule.
 *
 * Allocates a rule for the rule engine, initializes it.
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx Current IronBee context
 * @param[in] file Name of configuration file being parsed
 * @param[in] lineno Line number in configuration file
 * @param[in] is_stream true if this is an inspection rule else false
 * @param[out] prule Address which new rule is written
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_create(
    ib_engine_t                *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno,
    bool                        is_stream,
    ib_rule_t                 **prule);

/**
 * Lookup rule by ID
 *
 * @param[in] ib IronBee Engine.
 * @param[in] ctx Context to look in (or NULL).
 * @param[in] id ID to match.
 * @param[out] rule Matching rule.
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_lookup(
    ib_engine_t                *ib,
    ib_context_t               *ctx,
    const char                 *id,
    ib_rule_t                 **rule);

/**
 * Find rule matching a reference rule.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] ctx Context to look in (or NULL).
 * @param[in] ref Reference rule.
 * @param[out] rule Matching rule.
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_match(
    ib_engine_t                *ib,
    ib_context_t               *ctx,
    const ib_rule_t            *ref,
    ib_rule_t                 **rule);

/**
 * Add an enable All/ID/Tag to the enable list for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] etype Enable type (ID/Tag)
 * @param[in] name String description of @a etype
 * @param[in] enable true:Enable, false:Disable
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 * @param[in] str String of the id/tag
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_enable(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    ib_rule_enable_type_t       etype,
    const char                 *name,
    bool                        enable,
    const char                 *file,
    unsigned int                lineno,
    const char                 *str);

/**
 * Enable all rules for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_enable_all(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno);

/**
 * Add an enable ID to the enable list for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 * @param[in] id String of the id
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_enable_id(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno,
    const char                 *id);

/**
 * Add an enable tag to the enable list for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 * @param[in] tag String of the tag
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_enable_tag(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno,
    const char                 *tag);

/**
 * Disable all rules for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_disable_all(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno);

/**
 * Add an ID to the disable list for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 * @param[in] id String of the id
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_disable_id(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno,
    const char                 *id);

/**
 * Add a tag to the disable list for the specified context
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx IronBee context
 * @param[in] file Configuration file name
 * @param[in] lineno Line number in @a file
 * @param[in] tag String of the tag
 *
 * @returns Status code (IB_EINVAL for invalid ID, errors from ib_list_push())
 */
ib_status_t DLL_PUBLIC ib_rule_disable_tag(
    const ib_engine_t          *ib,
    ib_context_t               *ctx,
    const char                 *file,
    unsigned int                lineno,
    const char                 *tag);

/**
 * Set the execution phase of a rule (for phase rules).
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] phase Rule execution phase
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_set_phase(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    ib_rule_phase_num_t         phase);

/**
 * A utility function that converts a string to the appropriate phase number.
 *
 * This is used when developers are trying to create and register a new rule
 * during configuration time.
 *
 * @param[in] phase Phase string
 * @param[in] is_stream true if the phase is a stream phase, false if not.
 * @return
 *   - PHASE_INVALID when an error occures.
 *   - The appropriate phase number for the named phase if the given
 *     stream value is approprikate.
 */
ib_rule_phase_num_t DLL_PUBLIC ib_rule_lookup_phase(
    const char *phase,
    bool        is_stream);

/**
 * Get the name associated with a phase number
 *
 * @param[in] phase Phase number
 *
 * @returns Phase name string (or NULL if @a phase is invalid)
 */
const char DLL_PUBLIC *ib_rule_phase_name(
    ib_rule_phase_num_t         phase);

/**
 * Query as to whether a rule allow transformations
 *
 * @param[in,out] rule Rule to query
 *
 * @returns true or false
 */
bool DLL_PUBLIC ib_rule_allow_tfns(
    const ib_rule_t            *rule);

/**
 * Query as to whether a rule allow chains
 *
 * @param[in,out] rule Rule to query
 *
 * @returns true or false
 */
bool DLL_PUBLIC ib_rule_allow_chain(
    const ib_rule_t            *rule);

/**
 * Query as to whether is a stream inspection rule
 *
 * @param[in,out] rule Rule to query
 *
 * @returns true or false
 */
bool DLL_PUBLIC ib_rule_is_stream(
    const ib_rule_t            *rule);

/**
 * Get the operator flags required for this rule.
 *
 * @param[in] rule Rule to get flags for.
 *
 * @returns Required operator flags
 */
ib_flags_t DLL_PUBLIC ib_rule_required_op_flags(
    const ib_rule_t            *rule);

/**
 * Set a rule's operator.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] opinst Operator instance
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_set_operator(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    ib_operator_inst_t         *opinst);

/**
 * Set a rule's ID.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] id Rule ID
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_set_id(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    const char                 *id);

/**
 * Set a rule's chain flag
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_set_chain(
    ib_engine_t                *ib,
    ib_rule_t                  *rule);

/**
 * Get a rule's ID string.
 *
 * If @a rule is a chain rule, then the @c chain_id is returned.
 *
 * If @a rule has neither an id nor a @c chain_id NULL is returned
 * to allow the calling program to report the error or assign an id to
 * @a rule.
 *
 * @param[in] rule Rule to operate on
 *
 * @returns The rule's @c id, @c chain_id if @c id is not set, or NULL
 *          if neither is set.
 */
const char DLL_PUBLIC *ib_rule_id(
    const ib_rule_t            *rule);

/**
 * Check for a match against a rule's ID
 *
 * @param[in] rule Rule to match
 * @param[in] id ID to attempt to match against
 * @param[in] parents Check parent rules (in case of chains)?
 * @param[in] children Check child rules (in case of chains)?
 *
 * @returns true if match is found, false if not
 */
bool DLL_PUBLIC ib_rule_id_match(
    const ib_rule_t            *rule,
    const char                 *id,
    bool                        parents,
    bool                        children);

/**
 * Check for a match against a rule's tags
 *
 * @param[in] rule Rule to match
 * @param[in] tag Tag to attempt to match against
 * @param[in] parents Check parent rules (in case of chains)?
 * @param[in] children Check child rules (in case of chains)?
 *
 * @returns true if match is found, false if not
 */
bool DLL_PUBLIC ib_rule_tag_match(
    const ib_rule_t            *rule,
    const char                 *tag,
    bool                        parents,
    bool                        children);

/**
 * Create a rule target.
 *
 * @param[in] ib IronBee engine
 * @param[in] str Target string
 * @param[in] name Target name
 * @param[in] tfn_names List of transformations to add (or NULL)
 * @param[in,out] target Pointer to new target
 * @param[in] tfns_not_found Count of tfns names with no registered tfn
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_create_target(
    ib_engine_t                *ib,
    const char                 *str,
    const char                 *name,
    ib_list_t                  *tfn_names,
    ib_rule_target_t          **target,
    int                        *tfns_not_found);

/**
 * Add a target field to a rule.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] target Target object to add
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_add_target(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    ib_rule_target_t           *target);

/**
 * Add a transformation to all target fields of a rule.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] name Name of the transformation to add.
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_add_tfn(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    const char                 *name);

/**
 * Add an transformation to a target field.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] target Target field
 * @param[in] name Transformation name
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_target_add_tfn(
    ib_engine_t                *ib,
    ib_rule_target_t           *target,
    const char                 *name);

/**
 * Add a modifier to a rule.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] str Modifier string
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_add_modifier(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    const char                 *str);

/**
 * Add an action modifier to a rule.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] rule Rule to operate on
 * @param[in] action Action instance to add
 * @param[in] which Which action list to add to
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_add_action(
    ib_engine_t                *ib,
    ib_rule_t                  *rule,
    ib_action_inst_t           *action,
    ib_rule_action_t            which);

/**
 * Search for actions associated with a rule.
 *
 * @param[in] ib IronBee engine
 * @param[in] rule Rule to operate on
 * @param[in] which Which action list to search
 * @param[in] name Name of action to search for
 * @param[out] matches List of matching ib_action_inst_t pointers (or NULL)
 * @param[out] pcount Pointer to count of matches (or NULL)
 *
 * @returns Status code
 *   - IB_OK All ok
 *   - IB_EINVAL if both @a matches and @a pcount are NULL, or any of the
 *               other parameters are invalid
 */
ib_status_t DLL_PUBLIC ib_rule_search_action(const ib_engine_t *ib,
                                             const ib_rule_t *rule,
                                             ib_rule_action_t which,
                                             const char *name,
                                             ib_list_t *matches,
                                             size_t *pcount);

/**
 * Register a rule.
 *
 * Register a rule for the rule engine.
 *
 * @param[in] ib IronBee engine
 * @param[in,out] ctx Context in which to execute the rule
 * @param[in,out] rule Rule to register
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_register(
    ib_engine_t                *ib,
    ib_context_t               *ctx,
    ib_rule_t                  *rule);

/**
 * Invalidate an entire rule chain
 *
 * @param[in] ib IronBee engine
 * @param[in,out] ctx Context in which to execute the rule
 * @param[in,out] rule Rule to invalidate
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_rule_chain_invalidate(
    ib_engine_t                *ib,
    ib_context_t               *ctx,
    ib_rule_t                  *rule);

/**
 * Get the memory pool to use for rule allocations.
 *
 * @param[in] ib IronBee engine
 *
 * @returns Pointer to the memory pool to use.
 */
ib_mpool_t DLL_PUBLIC *ib_rule_mpool(
    ib_engine_t                *ib);

/**
 * Determine of operator results should be captured
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] result Operator result value
 *
 * @returns true if the results should be captured, false otherwise
 */
bool DLL_PUBLIC ib_rule_should_capture(
    const ib_rule_exec_t       *rule_exec,
    ib_num_t                    result);

/**
 * Perform logging of a rule's execution
 *
 * @param[in] rule_exec Rule execution object
 */
void DLL_PUBLIC ib_rule_log_execution(
    const ib_rule_exec_t       *rule_exec);

/**
 * Generic Logger for rule execution.
 *
 * This is intended to be used when a rule execution object is available.
 *
 * @warning There is currently a 1024 byte formatter limit when prefixing the
 *          log header data.
 *
 * @param[in] level Rule log level
 * @param[in] rule_exec Rule execution object (or NULL)
 * @param[in] file Filename (or NULL)
 * @param[in] line Line number (or 0)
 * @param[in] fmt Printf-like format string
 */
void DLL_PUBLIC ib_rule_log_exec(
    ib_rule_dlog_level_t        level,
    const ib_rule_exec_t       *rule_exec,
    const char                 *file,
    int                         line,
    const char                 *fmt, ...)
    PRINTF_ATTRIBUTE(5, 6);

/**
 * Log a fatal rule execution error
 *
 * This will cause @sa ib_rule_log_execution() to @sa assert(), and thus
 * should be used only in development environments.
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] file Filename (or NULL)
 * @param[in] line Line number (or 0)
 * @param[in] fmt Printf-like format string
 */
void DLL_PUBLIC ib_rule_log_fatal_ex(
    const ib_rule_exec_t       *rule_exec,
    const char                 *file,
    int                         line,
    const char                 *fmt, ...)
    PRINTF_ATTRIBUTE(4, 5);

/** Rule execution fatal error logging */
#define ib_rule_log_fatal(rule_exec, ...) \
    ib_rule_log_fatal_ex(rule_exec, __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution error logging */
#define ib_rule_log_error(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_ERROR, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution warning logging */
#define ib_rule_log_warn(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_WARNING, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution notice logging */
#define ib_rule_log_notice(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_NOTICE, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution info logging */
#define ib_rule_log_info(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_INFO, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution debug logging */
#define ib_rule_log_debug(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_DEBUG, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/** Rule execution trace logging */
#define ib_rule_log_trace(rule_exec, ...) \
    ib_rule_log_exec(IB_RULE_DLOG_TRACE, rule_exec, \
                     __FILE__, __LINE__, __VA_ARGS__)

/**
 * Generic Logger for with transaction
 *
 * This is intended to be used when no rule execution object is available.
 *
 * @warning There is currently a 1024 byte formatter limit when prefixing the
 *          log header data.
 *
 * @param[in] level Rule log level
 * @param[in] tx Transaction information
 * @param[in] file Filename (or NULL)
 * @param[in] line Line number (or 0)
 * @param[in] fmt Printf-like format string
 */
void ib_rule_log_tx(ib_rule_dlog_level_t level,
                    const ib_tx_t *tx,
                    const char *file,
                    int line,
                    const char *fmt, ...)
    PRINTF_ATTRIBUTE(5, 6);

/** Rule error logging (TX version) */
#define ib_rule_log_tx_error(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_ERROR, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** Rule warning logging (TX version) */
#define ib_rule_log_tx_warn(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_WARNING, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** Rule notice logging (TX version) */
#define ib_rule_log_tx_notice(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_NOTICE, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** Rule info logging (TX version) */
#define ib_rule_log_tx_info(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_INFO, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** Rule debug logging (TX version) */
#define ib_rule_log_tx_debug(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_DEBUG, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** Rule trace logging (TX version) */
#define ib_rule_log_tx_trace(tx, ...) \
    ib_rule_log_tx(IB_RULE_DLOG_TRACE, tx, \
                   __FILE__, __LINE__, __VA_ARGS__)

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* _IB_RULE_ENGINE_H_ */
