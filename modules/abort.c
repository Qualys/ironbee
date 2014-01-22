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
 * @brief IronBee --- Abort module
 *
 * This is a module that defines the "Abort" and "AbortIf" actions,
 * useful primarily for development purposes.
 *
 * @note This module can be disabled by configuring with the "--disable-devel"
 * option.
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/action.h>
#include <ironbee/context.h>
#include <ironbee/hash.h>
#include <ironbee/engine.h>
#include <ironbee/engine_state.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>

#include <assert.h>
#include <inttypes.h>
#include <strings.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        abort
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Abort mode
 */
typedef enum {
    ABORT_IMMEDIATE,              /**< Immediate abort */
    ABORT_TX_END,                 /**< Assert at end of transaction */
    ABORT_OFF                     /**< Don't assert, just log loudly */
} abort_mode_t;

/**
 * Abort module configuration
 */
struct abort_config_t {
    abort_mode_t  abort_mode;       /**< Abort mode. */
};
typedef struct abort_config_t abort_config_t;

/**
 * Abort module global configuration
 */
static abort_config_t abort_config = {
    .abort_mode = ABORT_IMMEDIATE,
};

/**
 * Abort types
 */
typedef enum {
    ABORT_ALWAYS,                 /**< Abort any time the abort fires */
    ABORT_OP_TRUE,                /**< Abort if operation true */
    ABORT_OP_FALSE,               /**< Abort if operation false */
    ABORT_OP_OK,                  /**< Abort if operator failed */
    ABORT_OP_FAIL,                /**< Abort if operator succeeded */
    ABORT_ACT_OK,                 /**< Abort if all actions succeeded */
    ABORT_ACT_FAIL,               /**< Abort if any actions failed */
} abort_type_t;

/**
 * Abort per-TX data.
 */
struct abort_tx_data_t {
    ib_list_t     *abort_list;      /**< List of rules that asserted */
};
typedef struct abort_tx_data_t abort_tx_data_t;

/**
 * Data passed to the abort action
 */
struct abort_action_t {
    abort_type_t     abort_type;    /**< Type of abort action */
    bool             false_action;  /**< Abort action inverted? */ 
    const char      *abort_str;     /**< String version of abort_type */
    ib_var_expand_t *message;       /**< Message */
};
typedef struct abort_action_t abort_action_t;

/**
 * Rule + associated abort actions
 */
struct abort_rule_t {
    const ib_rule_t *rule;          /**< The rule itself */
    ib_list_t       *abort_actions; /**< Associated aborts <abort_action_t> */
};
typedef struct abort_rule_t abort_rule_t;

/**
 * Abort module data
 */
struct abort_module_data_t {
    ib_hash_t *op_rules;       /**< Rules with operator aborts <abort_rule_t> */
    ib_hash_t *act_rules;      /**< Rules with action aborts <abort_rule_t> */
};
typedef struct abort_module_data_t abort_module_data_t;

/**
 * Abort action filter function
 *
 * @params[in] action Abort action to filter
 *
 * @returns true if @a action matches, otheriwse false
 */
typedef bool (* abort_filter_fn_t)(
    const abort_action_t *action
);

/**
 * Create the abort rule object associated with the @a rule (if required)
 *
 * @param[in] ib IronBee engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] rules Hash of rules to look up rule
 * @param[in] rule Rule to look up
 * @param[out] pabort_rule Pointer to abort rule object
 *
 * @returns Status code
 */
static ib_status_t create_abort_rule(
    const ib_engine_t  *ib,
    ib_mpool_t         *mp,
    ib_hash_t          *rules,
    const ib_rule_t    *rule,
    abort_rule_t      **pabort_rule
)
{
    assert(rules != NULL);
    assert(rule != NULL);
    assert(pabort_rule != NULL);

    ib_status_t   rc;
    abort_rule_t *abort_rule = NULL;
    ib_list_t    *abort_actions;
    const char   *rule_id = ib_rule_id(rule);

    assert(rule_id != NULL);

    /* Look up the object in the "all" hash */
    rc = ib_hash_get(rules, &abort_rule, rule_id);
    if ( (rc != IB_OK) && (rc != IB_ENOENT) ) {
        ib_log_error(ib,
                     "Abort: Failed to get rule data for \"%s\": %s",
                     rule_id, ib_status_to_string(rc));
        return rc;
    }

    /* If it's not NULL, or we're not creating, just return it. */
    if (abort_rule != NULL) {
        *pabort_rule = abort_rule;
        return IB_OK;
    }

    /* Create the abort rule object */
    abort_rule = ib_mpool_alloc(mp, sizeof(*abort_rule));
    if (abort_rule == NULL) {
        ib_log_error(ib, "Abort: Failed to allocate abort rule object");
        return IB_EALLOC;
    }

    /* Create the action list */
    rc = ib_list_create(&abort_actions, mp);
    if (rc != IB_OK) {
        ib_log_error(ib, "Abort: Failed to create action list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    abort_rule->abort_actions = abort_actions;
    abort_rule->rule = rule;

    /* Save it into the hash */
    rc = ib_hash_set(rules, rule_id, abort_rule);
    if (rc != IB_OK) {
        ib_log_error(ib, "Abort: Failed to set rule data for \"%s\": %s",
                     rule_id, ib_status_to_string(rc));
        return rc;
    }

    /* Done */
    *pabort_rule = abort_rule;
    return IB_OK;
}

/**
 * Get the abort rule object associated with the @a rule (if it exists)
 *
 * @param[in] rules Hash of rules to look up rule
 * @param[in] rule Rule to look up
 * @param[out] pabort_rule Pointer to abort rule object
 *
 * @returns Status code
 */
static ib_status_t get_abort_rule(
    ib_hash_t        *rules,
    const ib_rule_t  *rule,
    abort_rule_t    **pabort_rule
)
{
    assert(rules != NULL);
    assert(rule != NULL);
    assert(pabort_rule != NULL);

    ib_status_t  rc;
    const char  *rule_id = ib_rule_id(rule);

    assert(rule_id != NULL);

    /* Look up the object in the "all" hash */
    rc = ib_hash_get(rules, pabort_rule, rule_id);

    /* Done */
    return rc;
}

/**
 * Get / create TX module data
 *
 * @param[in] tx Transaction
 * @param[in] module Module object
 * @param[in] create Create if required?
 * @param[out] ptx_data Pointer to module-specific TX data
 *
 * @returns Status code
 */
static ib_status_t get_create_tx_data(
    ib_tx_t            *tx,
    const ib_module_t  *module,
    bool                create,
    abort_tx_data_t   **ptx_data
)
{
    assert(tx != NULL);
    assert(module != NULL);
    assert(ptx_data != NULL);

    ib_status_t      rc;
    abort_tx_data_t *tx_data = NULL;
    ib_list_t       *abort_list;

    rc = ib_tx_get_module_data(tx, module, &tx_data);
    if ( (rc != IB_OK) && (rc != IB_ENOENT) ) {
        ib_log_error_tx(tx,
                        "%s: Failed to get TX module data: %s",
                        module->name,
                        ib_status_to_string(rc));
        return rc;
    }

    /* If it's not NULL, or we're not creating, just return it. */
    if ( (tx_data != NULL) || (!create) ) {
        *ptx_data = tx_data;
        return IB_OK;
    }

    /* Create the action data */
    tx_data = ib_mpool_alloc(tx->mp, sizeof(*tx_data));
    if (tx_data == NULL) {
        ib_log_error_tx(tx,
                        "%s: Failed to get TX module data: %s",
                        module->name,
                        ib_status_to_string(rc));
        return IB_EALLOC;
    }

    /* Create the assert list */
    rc = ib_list_create(&abort_list, tx->mp);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "%s: Failed to get TX module data: %s",
                        module->name,
                        ib_status_to_string(rc));
        return rc;
    }
    tx_data->abort_list = abort_list;

    /* Set it for the transaction */
    rc = ib_tx_set_module_data(tx, module, tx_data);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "%s: Failed to set TX module data: %s",
                        module->name,
                        ib_status_to_string(rc));
        return rc;
    }

    /* Done */
    *ptx_data = tx_data;
    return IB_OK;
}

/**
 * Create function for the Abort action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Callback data (ib_module_t)
 *
 * @returns Status code
 */
static ib_status_t abort_create(
    ib_engine_t      *ib,
    const char       *parameters,
    ib_action_inst_t *inst,
    void             *cbdata)
{
    assert(ib != NULL);
    assert(inst != NULL);
    assert(cbdata != NULL);

    const char      *message;
    ib_var_expand_t *expand;
    ib_mpool_t      *mp = ib_engine_pool_main_get(ib);
    abort_action_t  *action;
    ib_status_t      rc;

    assert(mp != NULL);

    /* The first argument is the type, second is message string. */
    message = (parameters == NULL) ? "" : parameters;

    /* Expand the message string as required */
    rc = ib_var_expand_acquire(&expand,
                               mp,
                               IB_S2SL(message),
                               ib_engine_var_config_get(ib),
                               NULL, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Allocate an assert instance object */
    action = ib_mpool_alloc(mp, sizeof(*action));
    if (action == NULL) {
        return IB_EALLOC;
    }
    action->abort_str = "Always";
    action->abort_type = ABORT_ALWAYS;
    action->message = expand;

    inst->data = action;
    return IB_OK;
}

/**
 * Create function for the AbortIf action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Callback data (ib_module_t)
 *
 * @returns Status code
 */
static ib_status_t abort_if_create(
    ib_engine_t      *ib,
    const char       *parameters,
    ib_action_inst_t *inst,
    void             *cbdata)
{
    assert(ib != NULL);
    assert(inst != NULL);
    assert(cbdata != NULL);

    ib_var_expand_t *expand;
    ib_mpool_t      *mp = ib_engine_pool_main_get(ib);
    ib_mpool_t      *tmp = ib_engine_pool_temp_get(ib);
    abort_action_t  *action;
    const char      *type_str;
    abort_type_t     abort_type;
    const char      *message;
    ib_status_t      rc;

    assert(mp != NULL);

    /* The first argument is the type, second is message string. */
    type_str = parameters;
    if (parameters != NULL) {
        const char *colon = strchr(parameters, ':');
        if (colon == NULL) {
            message = "";
        }
        else {
            message = colon + 1;
            if (colon == parameters) {
                type_str = NULL;
            }
            else {
                type_str = ib_mpool_memdup_to_str(tmp, parameters, colon-parameters);
            }
        }
    }
    else {
        message = "";
    }

    /* Check for assert type */
    if (type_str == NULL) {
        ib_log_error(ib, "AbortIf: No type specified");
        return IB_EINVAL;
    }
    else if (strncasecmp(type_str, "OpTrue", 6) == 0) {
        abort_type = ABORT_OP_TRUE;
        type_str = "True";
    }
    else if (strncasecmp(type_str, "OpFalse", 7) == 0) {
        abort_type = ABORT_OP_FALSE;
        type_str = "False";
    }
    else if (strncasecmp(type_str, "OpOK", 4) == 0) {
        abort_type = ABORT_OP_OK;
        type_str = "Operator/OK";
    }
    else if (strncasecmp(type_str, "OpFail", 6) == 0) {
        abort_type = ABORT_OP_FAIL;
        type_str = "Operator/Fail";
    }
    else if (strncasecmp(type_str, "ActOk", 5) == 0) {
        abort_type = ABORT_ACT_OK;
        type_str = "Action/OK";
    }
    else if (strncasecmp(type_str, "ActFail", 7) == 0) {
        abort_type = ABORT_ACT_FAIL;
        type_str = "Action/Fail";
    }
    else {
        ib_log_error(ib, "AbortIf: Invalid type \"%s\"", type_str);
        return IB_EINVAL;
    }

    /* Expand the message string as required */
    rc = ib_var_expand_acquire(&expand,
                               mp,
                               IB_S2SL(message),
                               ib_engine_var_config_get(ib),
                               NULL, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Allocate an assert instance object */
    action = ib_mpool_alloc(mp, sizeof(*action));
    if (action == NULL) {
        return IB_EALLOC;
    }
    action->abort_str = type_str;
    action->abort_type = abort_type;
    action->message = expand;

    inst->data = action;
    return IB_OK;
}

/**
 * Execute function for the "Abort" and "AbortIf" actions
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data data (assert action data)
 * @param[in] cbdata (module object)
 *
 * @returns Status code
 */
static ib_status_t abort_execute(
    const ib_rule_exec_t *rule_exec,
    void                 *data,
    void                 *cbdata)
{
    assert(rule_exec != NULL);

    /* Do nothing */
    return IB_OK;
}

/**
 * Check status for Abort/AbortIf actions
 *
 * @param[in] ib IronBee engine (for debug)
 * @param[in] rc Operator/action result code
 * @param[in] expect_ok Expect an OK status?
 * @param[in] invert Invert the result?
 * @param[in,out] match Set to true on match
 *
 * @returns Status code
 */
static ib_status_t status_check(
    const ib_engine_t *ib,
    ib_status_t  rc,
    bool         expect_ok,
    bool         invert,
    bool        *match
)
{
    bool status_match;

    /* Based on abort type, determine if we have a match. */
    if (expect_ok) {
        status_match = (rc == IB_OK);
    }
    else {
        status_match = (rc != IB_OK);
    }

    /* Interpret the result */
    *match = (invert ? !status_match : status_match);
    ib_log_trace(ib, "status_check(%d [%s], %s, %s) -> %s\n",
                 rc, ib_status_to_string(rc),
                 expect_ok ? "True" : "False",
                 invert ? "True" : "False",
                 *match ? "True" : "False");

    return IB_OK;
}

/**
 * Check result for Abort/AbortIf actions
 *
 * @param[in] ib IronBee engine (for debug)
 * @param[in] result Operator/action result
 * @param[in] rc Operator/action result code
 * @param[in] expect_true Expect a true (non-zero) result?
 * @param[in] invert Invert the result?
 * @param[in,out] match Set to true on match
 *
 * @returns Status code
 */
static ib_status_t result_check(
    const ib_engine_t *ib,
    ib_num_t     result,
    ib_status_t  rc,
    bool         expect_true,
    bool         invert,
    bool        *match
)
{
    bool result_match;

    /* Based on abort type, determine if we have a match. */
    if (expect_true) {
        result_match = (result != 0);
    }
    else {
        result_match = (result == 0);
    }

    /* Interpret the result */
    *match = (rc == IB_OK) && (invert ? !result_match : result_match);

    ib_log_error(ib, "result_check(%"PRId64", %d [%s], %s, %s) -> %s\n",
                 result,
                 rc, ib_status_to_string(rc),
                 expect_true ? "True" : "False",
                 invert ? "True" : "False",
                 *match ? "True" : "False");

    return IB_OK;
}

/**
 * Execute function for the "Abort" and "AbortIf" actions
 *
 * @param[in] module Module object
 * @param[in] rule_exec The rule execution object
 * @param[in] label Label string
 * @param[in] name Name of action / operator
 * @param[in] aborts List of fired abort actions
 * @param[in] result Operator/action result value
 * @param[in] inrc Operator/action status code
 *
 * @returns void
 */
static ib_status_t abort_now(
    const ib_module_t    *module,
    const ib_rule_exec_t *rule_exec,
    const char           *label,
    const char           *name,
    const ib_list_t      *aborts,
    ib_num_t              result,
    ib_status_t           inrc
)
{
    assert(module != NULL);
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);
    assert(label != NULL);
    assert(aborts != NULL);

    const abort_config_t *config;
    abort_tx_data_t      *tx_data;
    const ib_list_node_t *node;
    const char           *expanded = NULL;
    size_t                expanded_length;
    ib_status_t           rc;

    /* Get my module configuration */
    rc = ib_context_module_config(rule_exec->tx->ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error_tx(rule_exec->tx,
                        "Failed to get %s module configuration: %s",
                        module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Log the results */
    ib_rule_log_error(rule_exec,
                      "ABORT: %s [%s] status=%d \"%s\" result=%"PRIu64,
                      label, name, inrc, ib_status_to_string(inrc), result);

    /* Log all of the related aborts */
    if (aborts != NULL) {
        IB_LIST_LOOP_CONST(aborts, node) {
            const abort_action_t *action = ib_list_node_data_const(node);

            /* Expand the string */
            rc = ib_var_expand_execute(action->message,
                                       &expanded, &expanded_length,
                                       rule_exec->tx->mp,
                                       rule_exec->tx->var_store);
            if (rc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "abort: Failed to expand string: %s",
                                  ib_status_to_string(rc));
                return rc;
            }

            ib_rule_log_error(rule_exec,
                              "  ABORT: %s \"%.*s\"",
                              action->abort_str, (int)expanded_length, expanded);
        }
    }

    switch(config->abort_mode) {
    case ABORT_OFF:
        return IB_OK;

    case ABORT_IMMEDIATE:
        abort();

    case ABORT_TX_END:
        /* Get the TX module data */
        rc = get_create_tx_data(rule_exec->tx, module, true, &tx_data);
        if (rc != IB_OK) {
            return rc;
        }

        /* Add the rule to the list */
        rc = ib_list_push(tx_data->abort_list, rule_exec->rule);
        if (rc != IB_OK) {
            return rc;
        }
        break;
    }

    return IB_OK;
}

/**
 * Post operator function
 *
 * @param[in] rule_exec Rule execution environment.
 * @param[in] op Operator just executed.
 * @param[in] instance_data Instance data of @a op.
 * @param[in] invert True iff this operator is inverted.
 * @param[in] value Input to operator.
 * @param[in] op_rc Result code of operator execution.
 * @param[in] result Result of operator.
 * @param[in] capture Capture collection of operator.
 * @param[in] cbdata Callback data (module data).
 */
void abort_post_operator(
    const ib_rule_exec_t *rule_exec,
    const ib_operator_t  *op,
    void                 *instance_data,
    bool                  invert,
    const ib_field_t     *value,
    ib_status_t           op_rc,
    ib_num_t              result,
    ib_field_t           *capture,
    void                 *cbdata
)
{
    assert(rule_exec != NULL);
    assert(op != NULL);
    assert(cbdata != NULL);

    ib_status_t                rc;
    const ib_module_t         *module = cbdata;
    const abort_module_data_t *module_data;
    const ib_list_node_t      *node;
    abort_rule_t              *abort_rule;
    bool                       do_abort = false;
    ib_list_t                 *aborts = NULL;

    assert(module->data != NULL);
    module_data = module->data;

    /* Find assocated abort rule item (if there is one) */
    rc = get_abort_rule(module_data->op_rules, rule_exec->rule, &abort_rule);
    if (rc == IB_ENOENT) {
        return;
    }
    else if (rc != IB_OK) {
        ib_rule_log_error(rule_exec, "Abort: Failed to get rule data: %s",
                          ib_status_to_string(rc));
        return;
    }

    /* Loop through the rule's abort operator actions */
    IB_LIST_LOOP_CONST(abort_rule->abort_actions, node) {
        const abort_action_t *action = ib_list_node_data_const(node);
        const ib_engine_t *ib = rule_exec->ib;

        /* Based on abort type, determine if we have a match. */
        switch(action->abort_type) {
        case ABORT_OP_TRUE:
            result_check(ib, result, op_rc, true,
                         (invert ^ action->false_action), &do_abort);
            break;
        case ABORT_OP_FALSE:
            result_check(ib, result, op_rc, false,
                         (invert ^ action->false_action), &do_abort);
            break;

        case ABORT_OP_OK:
            status_check(ib, op_rc, true, action->false_action, &do_abort);
            break;
        case ABORT_OP_FAIL:
            status_check(ib, op_rc, false, action->false_action, &do_abort);
            break;

        case ABORT_ALWAYS:
            result_check(ib, 1, op_rc, true, action->false_action, &do_abort);
            break;

        default:
            assert(0 && "Action abort at operator post!");
            break;
        }

        /* Create the actions list if required */
        if (do_abort) {
            if (aborts == NULL) {
                ib_list_create(&aborts, rule_exec->tx->mp);
            }
            if (aborts != NULL) {
                ib_list_push(aborts, (void *)action);
            }
        }
    }

    /* If any of the actions set the abort flag, do it now */
    if (do_abort) {
        abort_now(module, rule_exec, "Operator", ib_operator_get_name(op),
                  aborts, result, op_rc);
    }
}

/**
 * Post action function
 *
 * @param[in] rule_exec Rule execution environment.
 * @param[in] action_inst Instance data for action just executed.
 * @param[in] op_rc Result code of operator execution.
 * @param[in] result Result of operator.
 * @param[in] cbdata Callback data (module data).
 */
void abort_post_action(
    const ib_rule_exec_t   *rule_exec,
    const ib_action_inst_t *action_inst,
    ib_num_t                result,
    ib_status_t             act_rc,
    void                   *cbdata
)
{
    assert(rule_exec != NULL);
    assert(action_inst != NULL);
    assert(cbdata != NULL);

    ib_status_t                rc;
    const ib_module_t         *module = cbdata;
    const abort_module_data_t *module_data;
    const ib_list_node_t      *node;
    abort_rule_t              *abort_rule;
    bool                       do_abort = false;
    ib_list_t                 *aborts = NULL;

    assert(module->data != NULL);
    module_data = module->data;

    /* Ignore my own actions! */
    if (strncasecmp(action_inst->action->name, "AbortIf", 7) == 0) {
        const abort_action_t *action = action_inst->data;

        switch (action->abort_type) {
        case ABORT_ACT_OK:
        case ABORT_ACT_FAIL:
            return;
        default:
            break;
        }
    }

    /* Find assocated abort rule item (if there is one) */
    rc = get_abort_rule(module_data->act_rules, rule_exec->rule, &abort_rule);
    if (rc == IB_ENOENT) {
        return;
    }
    else if (rc != IB_OK) {
        ib_rule_log_error(rule_exec, "Abort: Failed to get rule data: %s",
                          ib_status_to_string(rc));
        return;
    }

    /* Loop through the rule's abort operator actions */
    IB_LIST_LOOP_CONST(abort_rule->abort_actions, node) {
        const abort_action_t *action = ib_list_node_data_const(node);
        const ib_engine_t *ib = rule_exec->ib;

        /* Based on abort type, determine if we have a match. */
        switch(action->abort_type) {
        case ABORT_ACT_OK:
            status_check(ib, act_rc, true, action->false_action, &do_abort);
            break;
        case ABORT_ACT_FAIL:
            status_check(ib, act_rc, false, action->false_action, &do_abort);
            break;

        case ABORT_ALWAYS:
            result_check(ib, 1, act_rc, true, action->false_action, &do_abort);
            break;

        default:
            assert(0 && "Operator abort at action post!");
            break;
        }

        /* Create the actions list if required */
        if (do_abort) {
            if (aborts == NULL) {
                ib_list_create(&aborts, rule_exec->tx->mp);
            }
            if (aborts != NULL) {
                ib_list_push(aborts, (void *)action);
            }
        }
    }

    /* If any of the actions set the abort flag, do it now */
    if (do_abort) {
        abort_now(module, rule_exec, "Action", action_inst->action->name,
                  aborts, result, act_rc);
    }

    return;
}

/**
 * Parse an AbortMode directive.
 *
 * @details Register a AbortMode directive to the engine.
 * @param[in] cp Configuration parser
 * @param[in] directive The directive name.
 * @param[in] p1 The first parameter passed to @a directive (mode).
 * @param[in] cbdata User data (module)
 */
static ib_status_t abort_mode_handler(
    ib_cfgparser_t  *cp,
    const char      *directive,
    const char      *p1,
    void            *cbdata
)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(p1 != NULL);
    assert(cbdata != NULL);

    ib_status_t      rc;
    ib_module_t     *module = cbdata;
    ib_context_t    *context;
    const char      *mode;
    abort_config_t *config;
    abort_mode_t    abort_mode;

    /* Get my configuration context */
    rc = ib_cfgparser_context_current(cp, &context);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "TxData: Failed to get current context: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Get my module context configuration */
    rc = ib_context_module_config(context, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Get the mode name string */
    mode = p1;
    if (strcasecmp(mode, "Immediate") == 0) {
        abort_mode = ABORT_IMMEDIATE;
    }
    else if (strcasecmp(mode, "TxEnd") == 0) {
        abort_mode = ABORT_TX_END;
    }
    else if (strcasecmp(mode, "Off") == 0) {
        abort_mode = ABORT_OFF;
    }
    else {
        ib_cfg_log_error(cp, "%s: Invalid AbortMode \"%s\"",
                         module->name, mode);
        return IB_EINVAL;
    }
    config->abort_mode = abort_mode;

    /* Done */
    return IB_OK;
}

/**
 * Handle TX finished event
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] event Event type.
 * @param[in] cbdata Callback data (module object)
 *
 * @returns
 *   - IB_OK on success.
 */
static ib_status_t handle_tx_finished(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == tx_finished_event);
    assert(cbdata != NULL);

    ib_status_t           rc;
    ib_module_t          *module = cbdata;
    abort_config_t       *config;
    abort_tx_data_t      *tx_data;
    const ib_list_node_t *node;

    /* Get my module configuration */
    rc = ib_context_module_config(tx->ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Failed to get %s module configuration: %s",
                        module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Check the configured mode */
    if (config->abort_mode != ABORT_TX_END) {
        return IB_OK;
    }

    /* Get the TX module data */
    rc = get_create_tx_data(tx, module, false, &tx_data);
    if (rc != IB_OK) {
        return rc;
    }
    else if (tx_data == NULL) {
        return IB_OK;
    }
    else if (IB_LIST_ELEMENTS(tx_data->abort_list) == 0) {
        return IB_OK;
    }

    /* Log it */
    ib_log_error_tx(tx, "ERROR: %"PRIu64" aborts detected in transaction:",
                    IB_LIST_ELEMENTS(tx_data->abort_list));
    IB_LIST_LOOP_CONST(tx_data->abort_list, node) {
        const ib_rule_t *rule = ib_list_node_data_const(node);
        ib_log_error_tx(tx, "  Rule \"%s\"", rule->meta.full_id);
    }

    /* We're outa here */
    abort();
    return IB_OK;
}

/**
 * Search for the rule for matching actions
 *
 * @param[in] ib IronBee engine
 * @param[in] rule Rule to search
 * @param[in] name Action name to search for
 * @param[in] true_actions Matching rule true actions
 * @param[in] false_actions Matching rule false actions
 *
 * @returns Status code
 */
static ib_status_t rule_search(
    const ib_engine_t *ib,
    const ib_rule_t   *rule,
    const char        *name,
    ib_list_t         *true_actions,
    ib_list_t         *false_actions
)
{
    ib_status_t rc;

    /* Search the True action list */
    ib_list_clear(true_actions);
    rc = ib_rule_search_action(ib, rule,
                               IB_RULE_ACTION_TRUE, name,
                               true_actions, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Search the False action list */
    ib_list_clear(false_actions);
    rc = ib_rule_search_action(ib, rule,
                               IB_RULE_ACTION_FALSE, name,
                               false_actions, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Add actions to the abort rule's action list
 *
 * @param[in] ib IronBee engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] rules_hash Rules hash to operate on
 * @param[in] rule Associated rule
 * @param[in] filter_fn Filter function or NULL
 * @param[in] true_actions List of true actions to add
 * @param[in] false_actions List of false actions to add
 *
 * @returns Status code
 */
static ib_status_t add_abort_actions(
    const ib_engine_t *ib,
    ib_mpool_t        *mp,
    ib_hash_t         *rules_hash,
    const ib_rule_t   *rule,
    abort_filter_fn_t  filter_fn,
    const ib_list_t   *true_actions,
    const ib_list_t   *false_actions
)
{
    ib_status_t           rc;
    abort_rule_t         *abort_rule;
    const ib_list_node_t *node;

    /* Create the abort rule object if required */
    rc = create_abort_rule(ib, mp, rules_hash, rule, &abort_rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add the matching true abort_action_t to the abort action list */
    IB_LIST_LOOP_CONST(true_actions, node) {
        const ib_action_inst_t *inst = ib_list_node_data_const(node);
        abort_action_t         *abort_action = inst->data;

        if ( (filter_fn == NULL) || (filter_fn(abort_action)) ) {
            abort_action->false_action = false;
            rc = ib_list_push(abort_rule->abort_actions, inst->data);
            if (rc != IB_OK) {
                return rc;
            }
        }
    }

    /* Add the matching false abort_action_t to the abort action list */
    IB_LIST_LOOP_CONST(false_actions, node) {
        const ib_action_inst_t *inst = ib_list_node_data_const(node);
        abort_action_t         *abort_action = inst->data;

        if ( (filter_fn == NULL) || (filter_fn(abort_action)) ) {
            abort_action->false_action = true;
            rc = ib_list_push(abort_rule->abort_actions, inst->data);
            if (rc != IB_OK) {
                return rc;
            }
        }
    }

    /* Done */
    return IB_OK;
}

/**
 * Filter operator aborts
 *
 * @param[in] action Abort action to filter
 *
 * @returns True if the abort is an operator abort, otherwise false
 */
static bool abort_op_filter(
    const abort_action_t *action
)
{
    switch (action->abort_type) {
    case ABORT_ALWAYS:
    case ABORT_OP_TRUE:
    case ABORT_OP_FALSE:
    case ABORT_OP_OK:
    case ABORT_OP_FAIL:
        return true;
    case ABORT_ACT_OK:
    case ABORT_ACT_FAIL:
        return false;
    }
    return false;
}

/**
 * Filter action aborts
 *
 * @param[in] action Abort action to filter
 *
 * @returns True if the abort is an action abort, otherwise false
 */
static bool abort_act_filter(
    const abort_action_t *action
)
{
    switch (action->abort_type) {
    case ABORT_OP_TRUE:
    case ABORT_OP_FALSE:
    case ABORT_OP_OK:
    case ABORT_OP_FAIL:
        return false;
    case ABORT_ALWAYS:
    case ABORT_ACT_OK:
    case ABORT_ACT_FAIL:
        return true;
    }
    return false;
}

/**
 * Handle context close events for the abort module
 *
 * @param[in] ib Engine
 * @param[in] rule Rule being registered
 * @param[in] cbdata Callback data (ib_module_t)
 *
 * @returns IB_DECLINE / Error status
 */
static ib_status_t abort_rule_ownership(
    const ib_engine_t *ib,
    const ib_rule_t   *rule,
    void              *cbdata
)
{
    assert(ib != NULL);
    assert(rule != NULL);
    assert(cbdata != NULL);

    ib_status_t           rc;
    ib_module_t          *module = cbdata;
    abort_module_data_t  *module_data = module->data;
    ib_mpool_t           *mp = ib_engine_pool_main_get(ib);
    ib_list_t            *true_actions;
    ib_list_t            *false_actions;
    ib_mpool_t           *tmp = ib_engine_pool_temp_get(ib);

    assert(module_data != NULL);

    /* Create the search lists */
    rc = ib_list_create(&true_actions, tmp);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_list_create(&false_actions, tmp);
    if (rc != IB_OK) {
        return rc;
    }

    /*
     * Handle Abort actions
     */

    /* Search for actions */
    rc = rule_search(ib, rule, "Abort", true_actions, false_actions);
    if (rc != IB_OK) {
        return rc;
    }

    /* If there are any matches, add this rule to both hashes */
    if ( (IB_LIST_ELEMENTS(true_actions) != 0) ||
         (IB_LIST_ELEMENTS(false_actions) != 0) )
    {
        rc = add_abort_actions(ib, mp, module_data->op_rules, rule, NULL,
                               true_actions, false_actions);
        if (rc != IB_OK) {
            return rc;
        }
        rc = add_abort_actions(ib, mp, module_data->act_rules, rule, NULL,
                               true_actions, false_actions);
        if (rc != IB_OK) {
            return rc;
        }
    }

    /*
     * Handle AbortIf actions
     */

    /* Search for actions */
    rc = rule_search(ib, rule, "AbortIf", true_actions, false_actions);
    if (rc != IB_OK) {
        return rc;
    }

    /* If there are any matches, add this rule to both hashes */
    if ( (IB_LIST_ELEMENTS(true_actions) != 0) ||
         (IB_LIST_ELEMENTS(false_actions) != 0) )
    {
        rc = add_abort_actions(ib, mp, module_data->op_rules, rule,
                               abort_op_filter, true_actions, false_actions);
        if (rc != IB_OK) {
            return rc;
        }
        rc = add_abort_actions(ib, mp, module_data->act_rules, rule,
                               abort_act_filter, true_actions, false_actions);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_DECLINED;
}

/**
 * Initialize the abort module
 *
 * @param[in] ib IronBee engine
 * @param[in] module Module
 * @param[in] cbdata Callback data (unused)
 *
 * @returns Status code
 **/
static ib_status_t abort_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata)
{
    ib_status_t          rc;
    ib_mpool_t          *mp = ib_engine_pool_main_get(ib);
    abort_module_data_t *module_data;

    /* Create the abort module data */
    module_data = ib_mpool_alloc(mp, sizeof(*module_data));
    if (module_data == NULL) {
        return IB_EALLOC;
    }

    /* Create the rule hashes */
    rc = ib_hash_create_nocase(&(module_data->op_rules), mp);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_hash_create_nocase(&(module_data->act_rules), mp);
    if (rc != IB_OK) {
        return rc;
    }

    /* Store the module data */
    module->data = module_data;

    /* Register the Abort action */
    rc = ib_action_register(ib,
                            "Abort",
                            abort_create, module,
                            NULL, NULL, /* no destroy function */
                            abort_execute, module);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the AbortIf action */
    rc = ib_action_register(ib,
                            "AbortIf",
                            abort_if_create, module,
                            NULL, NULL, /* no destroy function */
                            abort_execute, module);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the AbortMode directive */
    rc = ib_config_register_directive(ib,
                                      "AbortMode",
                                      IB_DIRTYPE_PARAM1,
                                      (ib_void_fn_t)abort_mode_handler,
                                      NULL,
                                      module,
                                      NULL,
                                      NULL);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register AbortMode directive: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the rule ownership function */
    rc = ib_rule_register_ownership_fn(ib, "Abort", abort_rule_ownership, module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register Abort rule ownership function: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the post operator function */
    rc = ib_rule_register_post_operator_fn(ib, abort_post_operator, module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register post operator function: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the post action function */
    rc = ib_rule_register_post_action_fn(ib, abort_post_action, module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register post action function: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the TX finished event */
    rc = ib_hook_tx_register(ib, tx_finished_event,
                             handle_tx_finished, module);
    if (rc != IB_OK) {
        ib_log_error(ib, "%s: Failed to register tx finished handler: %s",
                     module->name,
                     ib_status_to_string(rc));
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
    IB_MODULE_CONFIG(&abort_config),         /* Global config data */
    NULL,                                    /* Module config map */
    NULL,                                    /* Module directive map */
    abort_init,                              /* Initialize function */
    NULL,                                    /* Callback data */
    NULL,                                    /* Finish function */
    NULL,                                    /* Callback data */
);
