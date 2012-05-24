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
 *****************************************************************************/

/**
 * @file
 * @brief IronBee
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <assert.h>

#include <ironbee/bytestr.h>
#include <ironbee/rule_engine.h>
#include <ironbee/util.h>
#include <ironbee/field.h>
#include <ironbee/debug.h>
#include <ironbee/config.h>
#include <ironbee/mpool.h>
#include <ironbee/transformation.h>
#include <ironbee/operator.h>
#include <ironbee/action.h>
#include <ironbee/rule_engine.h>

#include <ironbee/debug.h>
#include <ironbee/mpool.h>

#include "ironbee_private.h"


/**
 * Phase Flags
 */
#define PHASE_FLAG_NONE          (0x0)     /**< No phase flags */
#define PHASE_FLAG_IS_VALID      (1 << 0)  /**< Phase is valid */
#define PHASE_FLAG_IS_STREAM     (1 << 1)  /**< Phase is steam inspection */
#define PHASE_FLAG_ALLOW_CHAIN   (1 << 2)  /**< Rule allows chaining */
#define PHASE_FLAG_ALLOW_TFNS    (1 << 3)  /**< Rule allows transformations */

/**
 * Max # of data types (IB_DTYPE_*) per rule phase
 */
#define MAX_PHASE_DATA_TYPES 4

/**
 * Data on each rule phase, one per phase.
 */
struct ib_rule_phase_meta_t {
    ib_bool_t                   is_stream;
    ib_rule_phase_t             phase_num;
    ib_state_hook_type_t        hook_type;
    ib_flags_t                  flags;
    const char                 *name;
    ib_flags_t                  required_op_flags;
    ib_state_event_type_t       event;
};

/* Rule definition data */
static const ib_rule_phase_meta_t rule_phase_meta[] =
{
    {
        IB_FALSE,
        PHASE_NONE,
        (ib_state_hook_type_t) -1,
        (PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Generic 'Phase' Rule",
        IB_OP_FLAG_PHASE,
        (ib_state_event_type_t) -1
    },
    {
        IB_FALSE,
        PHASE_REQUEST_HEADER,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Request Header",
        IB_OP_FLAG_PHASE,
        handle_request_header_event
    },
    {
        IB_FALSE,
        PHASE_REQUEST_BODY,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Request Body",
        IB_OP_FLAG_PHASE,
        handle_request_event
    },
    {
        IB_FALSE,
        PHASE_RESPONSE_HEADER,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Response Header",
        IB_OP_FLAG_PHASE,
        handle_response_header_event
    },
    {
        IB_FALSE,
        PHASE_RESPONSE_BODY,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Response Body",
        IB_OP_FLAG_PHASE,
        handle_response_event
    },
    {
        IB_FALSE,
        PHASE_POSTPROCESS,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_ALLOW_CHAIN | PHASE_FLAG_ALLOW_TFNS),
        "Post Process",
        IB_OP_FLAG_PHASE,
        handle_postprocess_event
    },

    /* Stream rule phases */
    {
        IB_TRUE,
        PHASE_NONE,
        (ib_state_hook_type_t) -1,
        (PHASE_FLAG_IS_STREAM),
        "Generic 'Stream Inspection' Rule",
        IB_OP_FLAG_STREAM,
        (ib_state_event_type_t) -1
    },
    {
        IB_TRUE,
        PHASE_STR_REQUEST_HEADER,
        IB_STATE_HOOK_TX,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_IS_STREAM),
        "Request Header Stream",
        IB_OP_FLAG_STREAM,
        handle_context_tx_event
    },
    {
        IB_TRUE,
        PHASE_STR_REQUEST_BODY,
        IB_STATE_HOOK_TXDATA,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_IS_STREAM),
        "Request Body Stream",
        IB_OP_FLAG_STREAM,
        request_body_data_event
    },
    {
        IB_TRUE,
        PHASE_STR_RESPONSE_HEADER,
        IB_STATE_HOOK_HEADER,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_IS_STREAM),
        "Response Header Stream",
        IB_OP_FLAG_STREAM,
        response_header_data_event
    },
    {
        IB_TRUE,
        PHASE_STR_RESPONSE_BODY,
        IB_STATE_HOOK_TXDATA,
        (PHASE_FLAG_IS_VALID | PHASE_FLAG_IS_STREAM),
        "Response Body Stream",
        IB_OP_FLAG_STREAM,
        response_body_data_event
    },
    {
        IB_FALSE,
        PHASE_INVALID,
        (ib_state_hook_type_t) -1,
        PHASE_FLAG_NONE,
        "Invalid",
        IB_OP_FLAG_NONE,
        (ib_state_event_type_t) -1
    }
};

/**
 * The rule engine uses recursion to walk through lists and chains.  These
 * define the limits to the depth of those recursions.
 */
#define MAX_LIST_RECURSION   (5)       /**< Max list recursion limit */
#define MAX_CHAIN_RECURSION  (10)      /**< Max chain recursion limit */

/**
 * Test the validity of a phase number
 * @internal
 *
 * @param phase_num Phase number to check
 *
 * @returns IB_TRUE if @phase_num is valid, IB_FALSE if not.
 */
static inline ib_bool_t is_phase_num_valid(ib_rule_phase_t phase_num)
{
    IB_FTRACE_INIT();
    IB_FTRACE_RET_UINT(
        ( (phase_num >= PHASE_NONE) && (phase_num < IB_RULE_PHASE_COUNT) )
        ? IB_TRUE : IB_FALSE);
}

/**
 * Find a rule's matching phase meta data, matching the only the phase number.
 * @internal
 *
 * @param[in] phase_num Phase number (PHASE_xxx)
 * @param[out] phase_meta Matching rule phase meta-data
 *
 * @returns Status code
 */
static ib_status_t find_phase_meta(ib_rule_phase_t phase_num,
                                   const ib_rule_phase_meta_t **phase_meta)
{
    IB_FTRACE_INIT();
    const ib_rule_phase_meta_t *meta;

    assert (phase_meta != NULL);
    assert (is_phase_num_valid(phase_num) == IB_TRUE);

    /* Loop through all parent rules */
    for (meta = rule_phase_meta;  meta->phase_num != PHASE_INVALID;  ++meta)
    {
        if (meta->phase_num == phase_num) {
            *phase_meta = meta;
            IB_FTRACE_RET_STATUS(IB_OK);
        }
    }
    IB_FTRACE_RET_STATUS(IB_ENOENT);
}

/**
 * Find a rule's matching phase meta data, matching the phase number and
 * the phase's stream type.
 * @internal
 *
 * @param[in] is_stream IB_TRUE if this is a "stream inspection" rule
 * @param[in] phase_num Phase number (PHASE_xxx)
 * @param[out] phase_meta Matching rule phase meta-data
 *
 * @note If @a is_stream is IB_TRI_UNSET, this function will return the
 * first phase meta-data object that matches the phase number.
 *
 * @returns Status code
 */
static ib_status_t find_phase_stream_meta(
    ib_bool_t is_stream,
    ib_rule_phase_t phase_num,
    const ib_rule_phase_meta_t **phase_meta)
{
    IB_FTRACE_INIT();
    const ib_rule_phase_meta_t *meta;

    assert (phase_meta != NULL);
    assert (is_phase_num_valid(phase_num) == IB_TRUE);

    /* Loop through all parent rules */
    for (meta = rule_phase_meta;  meta->phase_num != PHASE_INVALID;  ++meta)
    {
        if ( (meta->phase_num == phase_num) &&
             (is_stream == meta->is_stream) )
        {
            *phase_meta = meta;
            IB_FTRACE_RET_STATUS(IB_OK);
        }
    }
    IB_FTRACE_RET_STATUS(IB_ENOENT);
}

/**
 * Execute a field's transformations.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] tx Transaction
 * @param[in] target Target field
 * @param[in] value Initial value of the target field
 * @param[out] result Pointer to field in which to store the result
 *
 * @returns Status code
 */
static ib_status_t execute_field_tfns(ib_engine_t *ib,
                                      ib_tx_t *tx,
                                      ib_rule_target_t *target,
                                      ib_field_t *value,
                                      ib_field_t **result)
{
    IB_FTRACE_INIT();
    ib_status_t     rc;
    ib_num_t        n = 0;
    ib_list_node_t *node = NULL;
    ib_field_t     *in_field;
    ib_field_t     *out = NULL;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(target != NULL);
    assert(result != NULL);

    /* No transformations?  Do nothing. */
    if (IB_LIST_ELEMENTS(target->tfn_list) == 0) {
        *result = value;
        ib_log_debug3_tx(tx,
                         "No transformations for field \"%s\"",
                         target->field_name);
        IB_FTRACE_RET_STATUS(IB_OK);
    }
    else if (value == NULL) {
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    ib_log_debug3_tx(tx,
                     "Executing %d transformations on field \"%s\"",
                     IB_LIST_ELEMENTS(target->tfn_list), target->field_name);

    /*
     * Loop through all of the field operators.
     */
    in_field = value;
    IB_LIST_LOOP(target->tfn_list, node) {
        ib_tfn_t  *tfn = (ib_tfn_t *)node->data;
        ib_flags_t flags = 0;

        /* Run it */
        ++n;
        ib_log_debug3_tx(tx,
                         "Executing field transformation #%d \"%s\" on \"%s\"",
                         n, tfn->name, target->field_name);
        ib_log_rule_field(ib, "before tfn", in_field);
        rc = ib_tfn_transform(ib, tx->mp, tfn, in_field, &out, &flags);
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error executing field operator #%d "
                            "field \"%s\": %s",
                            n, target->field_name, ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
        ib_log_rule_field(ib, "after tfn", out);

        /* Verify that out isn't NULL */
        if (out == NULL) {
            ib_log_error_tx(tx,
                            "Field operator #%d field \"%s\" returned NULL",
                            n, target->field_name);
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }

        /* The output of the operator is now input for the next field op. */
        in_field = out;
    }

    /* The output of the final operator is the result */
    *result = out;

    /* Done. */
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Perform a block operation by signaling an error to the server.
 *
 * The server is signaled with
 * ib_server_error_response(ib_server_t *, ib_context_t *, int) using
 * @a tx and @a ib to provide the @c ib_context_t* and @c ib_server_t*,
 * respectively.
 *
 * @param ib IronBee engine containing the server callbacks.
 * @param tx Transaction containing the active context.
 *
 * @returns The result of calling ib_server_error_response().
 */
static ib_status_t report_block_to_server(ib_engine_t *ib, ib_tx_t *tx)
{
    IB_FTRACE_INIT();

    /* Store the final return code here. */
    ib_status_t rc;

    assert(ib);
    assert(ib->server);
    assert(tx);
    assert(tx->ctx);

    ib_log_debug_tx(tx, "Setting HTTP error response: status=%d",
                    tx->block_status);
    rc = ib_server_error_response(ib->server, tx, tx->block_status);
    if (rc == IB_DECLINED) {
        ib_log_notice_tx(tx,
                         "Server not willing to set HTTP error response.");
    }
    else if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Server failed to set HTTP error response: %s",
                        ib_status_to_string(rc));
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Execute a rule on a list of values
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] tx Transaction
 * @param[in] opinst Operator instance
 * @param[in] fname Field name
 * @param[in] value Field value to operate on
 * @param[in] recursion Recursion limit -- won't recurse if recursion is zero
 * @param[in, out] rule_result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t execute_rule_operator(ib_engine_t *ib,
                                         ib_tx_t *tx,
                                         ib_operator_inst_t *opinst,
                                         const char *fname,
                                         ib_field_t *value,
                                         ib_num_t recursion,
                                         ib_num_t *rule_result)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(tx != NULL);
    assert(opinst != NULL);
    assert(fname != NULL);

    ib_status_t rc;

    /* Limit recursion */
    --recursion;
    if (recursion <= 0) {
        ib_log_error_tx(tx, "Rule engine: List recursion limit reached");
        IB_FTRACE_RET_STATUS(IB_EOTHER);
    }

    /* Handle a list by looping through it */
    if ( (value != NULL) && (value->type == IB_FTYPE_LIST) ) {
        // @todo Remove mutable once list is const correct.
        ib_list_t *vlist;
        rc = ib_field_mutable_value(value, ib_ftype_list_mutable_out(&vlist));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_list_node_t *node = NULL;
        ib_num_t n = 0;

        IB_LIST_LOOP(vlist, node) {
            ib_field_t *nvalue = (ib_field_t *)ib_list_node_data(node);
            ++n;

            rc = execute_rule_operator(
                ib, tx, opinst, fname, nvalue, recursion, rule_result);
            if (rc != IB_OK) {
                ib_log_debug_tx(tx,
                             "Error executing \"%s\" on list element #%d: %s",
                             opinst->op->name, n, ib_status_to_string(rc));
            }
        }
        ib_log_debug3_tx(tx, "Operator \"%s\", target \"%s\" (list %zd) => %d",
                         opinst->op->name, fname, vlist->nelts, *rule_result);
    }
    else {
        /* Execute the operator */
        ib_num_t result = 0;
        rc = ib_operator_execute(ib, tx, opinst, value, &result);
        if (rc != IB_OK) {
            ib_log_debug_tx(tx,
                            "Operator \"%s\" returned an error "
                            "for field \"%s\": %s",
                            opinst->op->name, fname, ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        /* Store the result */
        if (result != 0) {
            *rule_result = result;
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute all operators on a single target.
 * @param[in] ib The IronBee engine.
 * @param[in] tx The transaction. Passed to execute_rule_operator.
 * @param[in] target Assigned to the target_result.
 * @param[in] opinst The operator instance.
 * @param[in] operand The field to operate on.
 * @param[in,out] rule_result The return code from the operator execution.
 * @param[out] target_results The results of the execution.
 * @returns IB_OK if the execution completed without error.
 *          If execute_field_tfns or execute_rule_operator fails
 *          then the return code from the failing call is returned
 *          immediately.
 */
static ib_status_t execute_phase_rule_targets_operators(
    ib_engine_t *ib,
    ib_tx_t *tx,
    ib_rule_target_t *target,
    ib_operator_inst_t  *opinst,
    ib_field_t *operand,
    ib_num_t *rule_result,
    ib_list_t *target_results)
{
    IB_FTRACE_INIT();

    ib_status_t rc;
    ib_num_t result = 0;
    ib_field_t *tfnvalue = NULL;  /* Value after transformations */

    const char *fname = target->field_name;

    /* Execute the field operators */
    rc = execute_field_tfns(ib, tx, target, operand, &tfnvalue);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error executing transformation "
                        "for \"%s\" on \"%s\": %s",
                        opinst->op->name, fname, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Execute the rule operator */
    rc = execute_rule_operator(ib,
                               tx,
                               opinst,
                               fname,
                               tfnvalue,
                               MAX_LIST_RECURSION,
                               &result);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Operator \"%s\" returned an error "
                        "for field \"%s\": %s",
                        opinst->op->name, fname, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    ib_log_debug3_tx(tx, "Operator \"%s\", field \"%s\" => %d",
                     opinst->op->name, fname, result);

    /* Store the result */
    if (result != 0) {
        *rule_result = result;
    }

    /* Create a rule target execution result object */
    if (target_results != NULL) {
        ib_rule_target_result_t *target_result =
            (ib_rule_target_result_t *)
            ib_mpool_alloc(tx->mp, sizeof(*target_result));
        if (target_result != NULL) {
            target_result->target = target;
            target_result->original = operand;
            target_result->transformed = tfnvalue;
            target_result->result = result;
            ib_list_push(target_results, target_result);
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute a single rule's operator on all target fields.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] rule Rule to execute
 * @param[in,out] tx Transaction
 * @param[in,out] rule_result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t execute_phase_rule_targets(ib_engine_t *ib,
                                              ib_rule_t *rule,
                                              ib_tx_t *tx,
                                              ib_num_t *rule_result,
                                              ib_list_t *target_results)
{
    IB_FTRACE_INIT();
    assert(ib);
    assert(rule);
    assert(tx);
    assert(rule_result);
    ib_list_node_t      *node = NULL;
    ib_operator_inst_t  *opinst = rule->opinst;
    ib_status_t          rc = IB_OK;

    /* Log what we're going to do */
    ib_log_debug3_tx(tx, "Executing rule \"%s\"", rule->meta.id);

    /* Special case: External rules */
    if ( (rule->flags & IB_RULE_FLAG_EXTERNAL) != 0) {

        /* Execute the operator */
        ib_log_debug3_tx(tx, "Executing external rule \"%s\"",
                         opinst->op->name);
        rc = ib_operator_execute(ib, tx, opinst, NULL, rule_result);
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "External operator \"%s\" returned an error: %s",
                            opinst->op->name, ib_status_to_string(rc));
        }
        IB_FTRACE_RET_STATUS(rc);
    }

    /*
     * Loop through all of the fields.
     *
     * @todo The current behavior is to keep running even after an operator
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP(rule->target_fields, node) {
        ib_rule_target_t *target = (ib_rule_target_t *)node->data;
        assert(target != NULL);
        const char       *fname = target->field_name;
        assert(fname != NULL);
        ib_field_t       *value = NULL;     /* Value from the DPI */

        /* Get the field value */
        rc = ib_data_get(tx->dpi, fname, &value);
        if (rc == IB_ENOENT) {
            ib_bool_t allow_null =
                ib_flags_all(opinst->op->flags, IB_OP_FLAG_ALLOW_NULL);
            if (allow_null == IB_FALSE) {
                continue;
            }
        }
        else if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error getting field \"%s\": %s",
                            fname, ib_status_to_string(rc));
            continue;
        }

        if ( (value != NULL) && (value->type == IB_FTYPE_LIST) ) {
            ib_list_t *value_list;
            ib_list_node_t *value_node;
            rc = ib_field_value(value, (void *)&value_list);

            if (rc != IB_OK) {
                ib_log_error_tx(tx, "Error getting field value \"%s\": %s",
                                value->name, rc);
                continue;
            }

            /* Run operations on each list element. */
            IB_LIST_LOOP(value_list, value_node) {
                const ib_status_t tmp_rc =
                    execute_phase_rule_targets_operators(
                        ib,
                        tx,
                        target,
                        opinst,
                        (ib_field_t *) value_node->data,
                        rule_result,
                        target_results);

                /* Capture failure to report back to the caller. */
                if (tmp_rc != IB_OK) {
                    rc = tmp_rc;
                }
            }
        }
        else {
            rc = execute_phase_rule_targets_operators(ib,
                                                      tx,
                                                      target,
                                                      opinst,
                                                      value,
                                                      rule_result,
                                                      target_results);
        }
    }

    /* Invert? */
    if ( (opinst->flags & IB_OPINST_FLAG_INVERT) != 0) {
        *rule_result = ( (*rule_result) == 0);
    }

    ib_log_debug3_tx(tx, "Rule \"%s\" Operator \"%s\" => %d",
                     rule->meta.id, opinst->op->name, *rule_result);

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Execute a single rule action
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] rule Rule to execute
 * @param[in,out] tx Transaction
 * @param[in] result Rule execution result
 * @param[in] action The action to execute
 *
 * @returns Status code
 */
static ib_status_t execute_action(ib_engine_t *ib,
                                  ib_rule_t *rule,
                                  ib_tx_t *tx,
                                  ib_num_t result,
                                  ib_action_inst_t *action)
{
    IB_FTRACE_INIT();
    ib_status_t   rc;
    const char   *name = (result != 0) ? "True" : "False";

    ib_log_debug3_tx(tx,
                     "Executing \"%s\" rule \"%s\" action %s",
                     rule->meta.id, name, action->action->name);

    /* Run it, check the results */
    rc = ib_action_execute(action, rule, tx);
    if (rc != IB_OK && rc != IB_DECLINED) {
        ib_log_error_tx(tx,
                        "Action \"%s\" returned an error: %s",
                        action->action->name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute a rule's actions
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] rule Rule to execute
 * @param[in,out] tx Transaction
 * @param[in] result Rule execution result
 * @param[in] actions List of actions to execute
 *
 * @returns Status code
 */
static ib_status_t execute_actions(ib_engine_t *ib,
                                   ib_rule_t *rule,
                                   ib_tx_t *tx,
                                   ib_num_t result,
                                   ib_list_t *actions)
{
    IB_FTRACE_INIT();
    ib_list_node_t   *node = NULL;
    ib_status_t       rc = IB_OK;
    const char       *name = (result != 0) ? "True" : "False";

    ib_log_debug3_tx(tx, "Executing \"%s\" rule %s actions",
                     rule->meta.id, name);

    /*
     * Loop through all of the fields
     *
     * @todo The current behavior is to keep running even after an action
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP(actions, node) {
        ib_status_t       arc;     /* Action's return code */
        ib_action_inst_t *action = (ib_action_inst_t *)node->data;

        /* Execute the action */
        arc = execute_action(ib, rule, tx, result, action);

        /* If it declined, then it may have blocked. */
        if (arc == IB_DECLINED) {
            rc = IB_DECLINED;
        }

        /* Record an error status code unless a block rc is to be reported. */
        else if (arc != IB_OK) {
            ib_log_error_tx(tx,
                            "Action %s/\"%s\" returned an error: %d",
                            name, action->action->name, arc);

            /* Only report errors codes if there is not a block signal
             * (IB_DECLINED) set to be returned. */
            if (rc != IB_DECLINED) {
                rc = arc;
            }
        }
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Execute a single phase rule, it's actions, and it's chained rules.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] event Event type
 * @param[in,out] tx Transaction
 * @param[in] recursion Recursion limit
 * @param[in,out] rule_result Result of rule execution
 *
 * @returns Status code
 */
static ib_status_t execute_phase_rule(ib_engine_t *ib,
                                      ib_rule_t *rule,
                                      ib_tx_t *tx,
                                      ib_num_t recursion,
                                      ib_num_t *rule_result)
{
    IB_FTRACE_INIT();
    ib_list_t   *actions;
    ib_list_t   *results = NULL;
    ib_status_t  rc = IB_OK;
    ib_status_t  trc;          /* Temporary status code */
    ib_bool_t    results_type; /* True results or false? */

    assert(ib != NULL);
    assert(rule != NULL);
    assert(rule->phase_meta->is_stream == IB_FALSE);
    assert(tx != NULL);
    assert(rule_result != NULL);

    /* Limit recursion */
    --recursion;
    if (recursion <= 0) {
        ib_log_error_tx(tx,
                        "Rule engine: Phase chain recursion limit reached");
        IB_FTRACE_RET_STATUS(IB_EOTHER);
    }

    /* Initialize the rule result */
    *rule_result = 0;

    /* Create the results list for logging */
    if (ib_rule_log_level(ib) == IB_RULE_LOG_FAST) {
        trc = ib_list_create(&results, tx->mp);
        if (trc != IB_OK) {
            ib_log_error_tx(tx,
                            "Rule engine: Failed to create results list");
        }
    }

    /*
     * Execute the rule operator on the target fields.
     *
     * @todo The current behavior is to keep running even after an operator
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    trc = execute_phase_rule_targets(ib, rule, tx, rule_result, results);
    if (trc != IB_OK) {
        ib_log_error_tx(tx, "Error executing rule \"%s\": %s",
                        rule->meta.id, ib_status_to_string(trc));
        rc = trc;
    }

    /*
     * Execute the actions.
     *
     * @todo The current behavior is to keep running even after action(s)
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    if (*rule_result != 0) {
        results_type = IB_TRUE;
        actions = rule->true_actions;
    }
    else {
        results_type = IB_FALSE;
        actions = rule->false_actions;
    }
    trc = execute_actions(ib, rule, tx, *rule_result, actions);
    if ( trc == IB_DECLINED ) {
        if ( tx->flags & IB_TX_BLOCK_IMMEDIATE ) {
            ib_log_info_tx(tx,
                "Rule \"%s\" caused immediate block.", rule->meta.id);
            ib_log_info_tx(tx,
                "Aborting chain rules because of immediate block.");
            IB_FTRACE_RET_STATUS(IB_DECLINED);
        }
    }
    else if (trc != IB_OK) {
        ib_log_error_tx(tx,
                        "Failed to execute action for rule \"%s\"",
                        rule->meta.id);
        rc = trc;
    }

    if (results != NULL) {
        ib_rule_log_fast(tx, rule, results_type, results, actions);
    }

    /*
     * Execute chained rule
     *
     * @todo The current behavior is to keep running even after a chained rule
     * returns an error.  This needs further discussion to determine what
     * the correct behavior should be.
     *
     * @note Chaining is currently done via recursion.
     */
    if ( (*rule_result != 0) && (rule->chained_rule != NULL) ) {
        ib_log_debug3_tx(tx,
                         "Chaining to rule \"%s\"",
                         rule->chained_rule->meta.id);
        trc = execute_phase_rule(ib,
                                 rule->chained_rule,
                                 tx,
                                 recursion,
                                 rule_result);

        if (trc == IB_DECLINED) {
            rc = trc;
        }
        else if (trc != IB_OK) {
            ib_log_error_tx(tx, "Error executing chained rule \"%s\"",
                            rule->chained_rule->meta.id);
            rc = trc;
        }
    }

    IB_FTRACE_RET_STATUS(rc);
}

static inline ib_bool_t rule_is_runnable(const ib_rule_ctx_data_t *ctx_rule,
                                         const ib_rule_t *rule)
{
    IB_FTRACE_INIT();

    /* Skip invalid / disabled rules */
    if (ib_flags_all(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED) != IB_TRUE) {
        IB_FTRACE_RET_UINT(IB_FALSE);
    }
    if (ib_flags_all(rule->flags, IB_RULE_FLAG_VALID) != IB_TRUE) {
        IB_FTRACE_RET_UINT(IB_FALSE);
    }

    IB_FTRACE_RET_UINT(IB_TRUE);
}

/**
 * Run a set of phase rules.
 *
 * @internal
 *
 * @param[in] ib Engine.
 * @param[in,out] tx Transaction.
 * @param[in] event Event type.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t).
 *
 * @returns Status code. IB_OK on success. IB_DECLINED to signal that
 *          rule processing should not continue.
 */
static ib_status_t run_phase_rules(ib_engine_t *ib,
                                   ib_tx_t *tx,
                                   ib_state_event_type_t event,
                                   void *cbdata)
{
    IB_FTRACE_INIT();
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->ctx != NULL);
    assert(cbdata != NULL);

    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;
    ib_context_t               *ctx = tx->ctx;
    ib_ruleset_phase_t         *ruleset_phase;
    ib_list_t                  *rules;
    ib_list_node_t             *node = NULL;

    /* Boolean indicating a block at the end of this phase. */
    int                         block_phase = 0;

    ruleset_phase = &(ctx->rules->ruleset.phases[meta->phase_num]);
    assert(ruleset_phase != NULL);
    rules = ruleset_phase->phase_list;
    assert(rules != NULL);

    /* Sanity check */
    if (ruleset_phase->phase_num != meta->phase_num) {
        ib_log_error_tx(tx, "Rule engine: Phase %d/\"%s\" is %d",
                        meta->phase_num, meta->name, ruleset_phase->phase_num );
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Walk through the rules & execute them */
    if (IB_LIST_ELEMENTS(rules) == 0) {
        ib_log_debug3_tx(tx,
                         "No rules for phase %d/\"%s\" in context \"%s\"",
                         meta->phase_num, meta->name, ib_context_full_get(ctx));
        IB_FTRACE_RET_STATUS(IB_OK);
    }
    ib_log_debug3_tx(tx,
                     "Executing %d rules for phase %d/\"%s\" in context \"%s\"",
                     IB_LIST_ELEMENTS(rules),
                     meta->phase_num, meta->name, ib_context_full_get(ctx));

    /*
     * Loop through all of the rules for this phase, execute them.
     *
     * @todo The current behavior is to keep running even after rule execution
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP(rules, node) {
        ib_rule_ctx_data_t *ctx_rule = (ib_rule_ctx_data_t *)node->data;
        ib_rule_t          *rule;
        ib_num_t            rule_result = 0;
        ib_status_t         rule_rc;

        /* Skip invalid / disabled rules */
        rule = ctx_rule->rule;
        if (rule_is_runnable(ctx_rule, rule) != IB_TRUE) {
            ib_log_debug2_tx(tx,
                             "Not executing invalid/disabled phase rule \"%s\"",
                             rule->meta.id);
            continue;
        }

        /* Execute the rule, it's actions and chains */
        rule_rc = execute_phase_rule(ib,
                                     rule,
                                     tx,
                                     MAX_CHAIN_RECURSION,
                                     &rule_result);

        /* Handle declined return code. Did this block? */
        if( rule_rc == IB_DECLINED ) {
            if ( tx->flags & IB_TX_BLOCK_PHASE ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in phase block: %s",
                               rule->meta.id, ib_status_to_string(rule_rc));
                block_phase = 1;
            }
            else if ( tx->flags & IB_TX_BLOCK_IMMEDIATE ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in immediate block: %s",
                               rule->meta.id, ib_status_to_string(rule_rc));
                ib_log_info_tx(tx,
                              "Rule processing is aborted by immediate block.");
                report_block_to_server(ib, tx);
                IB_FTRACE_RET_STATUS(IB_DECLINED);
            }
            else if ( tx->flags & IB_TX_BLOCK_ADVISORY ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in advisory block: %s",
                               rule->meta.id, ib_status_to_string(rule_rc));
            }
        }
        else if (rule_rc != IB_OK) {
            ib_log_error_tx(tx,
                         "Error executing rule \"%s\": %s",
                         rule->meta.id, ib_status_to_string(rule_rc));
        }
    }

    if ( block_phase != 0 ) {
        report_block_to_server(ib, tx);
        IB_FTRACE_RET_STATUS(IB_DECLINED);
    }

    /*
     * @todo Eat errors for now.  Unless something Really Bad(TM) has
     * occurred, return IB_OK to the engine.  A bigger discussion of if / how
     * such errors should be propagated needs to occur.
     */
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute a single stream txdata rule, and it's actions
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] event Event type
 * @param[in] tx Transaction
 * @param[in,out] txdata Transaction data
 * @param[in,out] rule_result Result of rule execution
 *
 * @returns Status code
 */
static ib_status_t execute_stream_txdata_rule(ib_engine_t *ib,
                                              ib_rule_t *rule,
                                              ib_tx_t *tx,
                                              ib_txdata_t *txdata,
                                              ib_num_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t          rc = IB_OK;
    ib_operator_inst_t  *opinst = rule->opinst;
    ib_field_t          *value = NULL;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(txdata != NULL);
    assert(result != NULL);
    assert(rule->phase_meta->is_stream == IB_TRUE);


    /*
     * Execute the rule operator.
     *
     * @todo The current behavior is to keep running even after action(s)
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */

    /* Create a field to hold the data */
    rc = ib_field_create_bytestr_alias(&value,
                                       tx->mp,
                                       "tmp", 3,
                                       txdata->data, txdata->dlen);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating field for stream txdata rule data: %s",
                        ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Execute the rule operator */
    rc = ib_operator_execute(ib, tx, opinst, value, result);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                     "Operator \"%s\" returned an error: %s",
                     opinst->op->name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    ib_log_debug3_tx(tx, "Operator \"%s\" => %d", opinst->op->name, *result);

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Execute a single stream header rule, and it's actions
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] event Event type
 * @param[in] tx Transaction
 * @param[in] header Parsed header
 * @param[in,out] rule_result Result of rule execution
 *
 * @returns Status code
 */
static ib_status_t execute_stream_header_rule(ib_engine_t *ib,
                                              ib_rule_t *rule,
                                              ib_tx_t *tx,
                                              ib_parsed_header_t *header,
                                              ib_num_t *rule_result)
{
    IB_FTRACE_INIT();
    ib_status_t          rc = IB_OK;
    ib_operator_inst_t  *opinst = rule->opinst;
    ib_field_t          *value;
    ib_parsed_name_value_pair_list_t *nvpair;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(header != NULL);
    assert(rule_result != NULL);
    assert(rule->phase_meta->is_stream == IB_TRUE);


    /*
     * Execute the rule operator.
     *
     * @todo The current behavior is to keep running even after action(s)
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    *rule_result = 0;
    for (nvpair = header;  nvpair != NULL;  nvpair = nvpair->next) {
        ib_num_t result = 0;

        /* Create a field to hold the data */
        ib_log_debug_tx(tx, "nvpair: \"%.*s\"=\"%.*s\"\n",
                        (int)ib_bytestr_length(nvpair->name),
                        (const char *)ib_bytestr_const_ptr(nvpair->name),
                        (int)ib_bytestr_length(nvpair->value),
                        (const char *)ib_bytestr_const_ptr(nvpair->value));
        rc = ib_field_create(&value,
                             tx->mp,
                             (const char *)ib_bytestr_const_ptr(nvpair->name),
                             ib_bytestr_length(nvpair->name),
                             IB_FTYPE_BYTESTR,
                             ib_ftype_bytestr_in(nvpair->value));
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error creating field for "
                            "header stream rule data: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        /* Execute the rule operator */
        rc = ib_operator_execute(ib, tx, opinst, value, &result);
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Operator \"%s\" returned an error: %s",
                            opinst->op->name, ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        /* Store the result */
        if (result != 0) {
            *rule_result = result;
        }
    }
    ib_log_debug3_tx(tx, "Operator \"%s\" => %d",
                     opinst->op->name, *rule_result);

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Run a set of stream rules.
 * @internal
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] event Event type.
 * @param[in] txdata Transaction data (or NULL)
 * @param[in] header Parsed header (or NULL)
 * @param[in] meta Phase meta data
 *
 * @returns Status code IB_OK or IB_DECLINED if rule processing should not
 *          continue.
 */
static ib_status_t run_stream_rules(ib_engine_t *ib,
                                    ib_tx_t *tx,
                                    ib_state_event_type_t event,
                                    ib_txdata_t *txdata,
                                    ib_parsed_header_t *header,
                                    const ib_rule_phase_meta_t *meta)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert( (txdata != NULL) || (header != NULL) );
    assert(meta != NULL);
    assert( (meta->hook_type != IB_STATE_HOOK_TXDATA) || (txdata != NULL) );
    assert( (meta->hook_type != IB_STATE_HOOK_HEADER) || (header != NULL) );

    ib_context_t            *ctx = tx->ctx;
    ib_ruleset_phase_t      *ruleset_phase =
        &(ctx->rules->ruleset.phases[meta->phase_num]);
    ib_list_t               *rules = ruleset_phase->phase_list;
    ib_list_node_t          *node = NULL;

    /* Boolean indicating a block at the end of this phase. */
    int                         block_phase = 0;

    /* Sanity check */
    if (ruleset_phase->phase_num != meta->phase_num) {
        ib_log_error_tx(tx, "Rule engine: Stream %d/\"%s\" is %d",
                     meta->phase_num, meta->name, ruleset_phase->phase_num);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Are there any rules?  If not, do a quick exit */
    if (IB_LIST_ELEMENTS(rules) == 0) {
        ib_log_debug3_tx(tx,
                      "No rules for stream %d/\"%s\" in context \"%s\"",
                      meta->phase_num, meta->name, ib_context_full_get(ctx));
        IB_FTRACE_RET_STATUS(IB_OK);
    }
    ib_log_debug3_tx(tx,
                  "Executing %d rules for stream %d/\"%s\" in context \"%s\"",
                  IB_LIST_ELEMENTS(rules),
                  meta->phase_num, meta->name, ib_context_full_get(ctx));

    /*
     * Loop through all of the rules for this phase, execute them.
     *
     * @todo The current behavior is to keep running even after rule execution
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP(rules, node) {
        ib_rule_ctx_data_t *ctx_rule = (ib_rule_ctx_data_t *)node->data;
        ib_rule_t          *rule;
        ib_list_t          *actions;
        ib_num_t            result = 0;
        ib_status_t         rc = IB_OK;

        /* Skip invalid / disabled rules */
        rule = ctx_rule->rule;
        if (rule_is_runnable(ctx_rule, rule) != IB_TRUE) {
            ib_log_debug2_tx(tx,
                             "Not executing invalid/disabled "
                             "stream rule \"%s\"",
                             rule->meta.id);
            continue;
        }

        /*
         * Execute the rule
         *
         * @todo The current behavior is to keep running even after an
         * operator returns an error.  This needs further discussion to
         * determine what the correct behavior should be.
         */
        if (txdata != NULL) {
            rc = execute_stream_txdata_rule(ib, rule, tx, txdata, &result);
        }
        else if (header != NULL) {
            rc = execute_stream_header_rule(ib, rule, tx, header, &result);
        }
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error executing rule \"%s\": %s",
                            rule->meta.id, ib_status_to_string(rc));
        }

        /* Invert? */
        if ( (rule->opinst->flags & IB_OPINST_FLAG_INVERT) != 0) {
            result = ( (result) == 0);
        }

        /*
         * Execute the actions.
         *
         * @todo The current behavior is to keep running even after action(s)
         * returns an error.  This needs further discussion to determine what
         * the correct behavior should be.
         */
        actions = (result)?  rule->true_actions: rule->false_actions;

        rc = execute_actions(ib, rule, tx, result, actions);

        /* Handle declined return code. Did this block? */
        if( rc == IB_DECLINED ) {
            if ( tx->flags & IB_TX_BLOCK_PHASE ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in phase block: %s",
                               rule->meta.id, ib_status_to_string(rc));
                block_phase = 1;
            }
            else if ( tx->flags & IB_TX_BLOCK_IMMEDIATE ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in immediate block: %s",
                               rule->meta.id, ib_status_to_string(rc));
                ib_log_info_tx(tx,
                              "Rule processing is aborted by immediate block.");
                report_block_to_server(ib, tx);
                IB_FTRACE_RET_STATUS(IB_DECLINED);
            }
            else if ( tx->flags & IB_TX_BLOCK_ADVISORY ) {
                ib_log_info_tx(tx,
                               "Rule \"%s\" resulted in advisory block: %s",
                               rule->meta.id, ib_status_to_string(rc));
            }
        }
        else if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error executing action for rule \"%s\": %s",
                            rule->meta.id, ib_status_to_string(rc));
        }
    }

    if ( block_phase != 0 ) {
        report_block_to_server(ib, tx);
        IB_FTRACE_RET_STATUS(IB_DECLINED);
    }

    /*
     * @todo Eat errors for now.  Unless something Really Bad(TM) has
     * occurred, return IB_OK to the engine.  A bigger discussion of if / how
     * such errors should be propagated needs to occur.
     */
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Run stream header rules
 * @internal
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] event Event type.
 * @param[in] header Parsed header
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_header_rules(ib_engine_t *ib,
                                           ib_tx_t *tx,
                                           ib_state_event_type_t event,
                                           ib_parsed_header_t *header,
                                           void *cbdata)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(header != NULL);
    assert(cbdata != NULL);

    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;
    ib_status_t rc;
    rc = run_stream_rules(ib, tx, event, NULL, header, meta);
    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Run stream TXDATA rules
 * @internal
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] event Event type.
 * @param[in] txdata Transaction data.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_txdata_rules(ib_engine_t *ib,
                                           ib_tx_t *tx,
                                           ib_state_event_type_t event,
                                           ib_txdata_t *txdata,
                                           void *cbdata)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(cbdata != NULL);

    if (txdata == NULL) {
        IB_FTRACE_RET_STATUS(IB_OK);
    }
    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;
    ib_status_t rc;
    rc = run_stream_rules(ib, tx, event, txdata, NULL, meta);
    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Run stream TX rules (aka request header)
 * @internal
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] event Event type.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_tx_rules(ib_engine_t *ib,
                                       ib_tx_t *tx,
                                       ib_state_event_type_t event,
                                       void *cbdata)
{
    IB_FTRACE_INIT();
    ib_parsed_header_wrapper_t *hdrs;
    ib_status_t rc;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(cbdata != NULL);

    /* Wrap up the request line */
    rc = ib_parsed_name_value_pair_list_wrapper_create(&hdrs, tx);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Error creating name/value pair list: %s",
                        ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    if ( (tx->request_line->method != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->method) != NULL) )
    {
        rc = ib_parsed_name_value_pair_list_add(
            hdrs,
            "method", 6,
            (const char *)ib_bytestr_const_ptr(tx->request_line->method),
            ib_bytestr_length(tx->request_line->method));
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error adding method to name/value pair list: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }
    if ( (tx->request_line->uri != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->uri) != NULL) )
    {
        rc = ib_parsed_name_value_pair_list_add(
            hdrs,
            "uri", 4,
            (const char *)ib_bytestr_const_ptr(tx->request_line->uri),
            ib_bytestr_length(tx->request_line->uri));
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error adding uri to name/value pair list: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }
    if ( (tx->request_line->protocol != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->protocol) != NULL) )
    {
        rc = ib_parsed_name_value_pair_list_add(
            hdrs,
            "protocol", 7,
            (const char *)ib_bytestr_const_ptr(tx->request_line->protocol),
            ib_bytestr_length(tx->request_line->protocol));
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error adding protocol to name/value pair list: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Now, process the request line */
    if ( (hdrs != NULL) && (hdrs->head != NULL) ) {
        ib_log_debug_tx(tx, "Running header line through stream header");
        rc = run_stream_header_rules(ib, tx, event,
                                     hdrs->head, cbdata);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error processing tx request line: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Process the request header */
    if ( (tx->request_header != NULL) && (tx->request_header->head != NULL) ) {
        ib_log_debug_tx(tx, "Running header through stream header");
        rc = run_stream_header_rules(ib, tx, event,
                                     tx->request_header->head,
                                     cbdata);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error processing tx request line: %s",
                            ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Initialize rule set objects.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in,out] rule_engine Rule engine
 *
 * @returns Status code
 */
static ib_status_t init_ruleset(ib_engine_t *ib,
                                ib_mpool_t *mp,
                                ib_rule_context_t *ctx_rules)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    ib_num_t    phase_num;

    /* Initialize the phase rules */
    for (phase_num = (ib_num_t)PHASE_NONE;
         phase_num < (ib_num_t)IB_RULE_PHASE_COUNT;
         ++phase_num)
    {
        ib_ruleset_phase_t *ruleset_phase =
            &(ctx_rules->ruleset.phases[phase_num]);
        ruleset_phase->phase_num = (ib_rule_phase_t)phase_num;
        rc = find_phase_meta(phase_num, &(ruleset_phase->phase_meta));
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Rule set initialization: "
                         "failed to find phase meta data: %s",
                         ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        rc = ib_list_create(&(ruleset_phase->phase_list), mp);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Rule set initialization: "
                         "failed to create phase ruleset list: %s",
                         ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Create a hash to hold operators by name */
    rc = ib_hash_create_nocase(&(ctx_rules->rule_hash), mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule set initialization: failed to create hash: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Register rules callbacks
 * @internal
 *
 * @param[in] ib Engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in,out] rule_engine Rule engine
 *
 * @returns Status code
 */
static ib_status_t register_callbacks(ib_engine_t *ib,
                                      ib_mpool_t *mp,
                                      ib_rule_engine_t *rule_engine)
{
    IB_FTRACE_INIT();
    const ib_rule_phase_meta_t *meta;
    const char                 *hook_type = NULL;
    ib_status_t                 rc = IB_OK;

    /* Register specific handlers for specific events, and a
     * generic handler for the rest */
    for (meta = rule_phase_meta; meta->phase_num != PHASE_INVALID; ++meta) {
        if (meta->event == (ib_state_event_type_t) -1) {
            continue;
        }

        /* Non-phase rules all use the same callback */
        switch (meta->is_stream) {

        case IB_FALSE :
            rc = ib_hook_tx_register(
                ib,
                meta->event,
                run_phase_rules,
                (void *)meta);
            hook_type = "tx";
            break;

        case IB_TRUE :
            switch (meta->hook_type) {

            case IB_STATE_HOOK_TX:
                rc = ib_hook_tx_register(
                    ib,
                    meta->event,
                    run_stream_tx_rules,
                    (void *)meta);
                hook_type = "stream-tx";
                break;

            case IB_STATE_HOOK_TXDATA:
                rc = ib_hook_txdata_register(
                    ib,
                    meta->event,
                    run_stream_txdata_rules,
                    (void *)meta);
                hook_type = "txdata";
                break;

            case IB_STATE_HOOK_HEADER:
                rc = ib_hook_parsed_header_data_register(
                    ib,
                    meta->event,
                    run_stream_header_rules,
                    (void *)meta);
                hook_type = "header";
                break;

            default:
                ib_log_error(ib,
                             "Unknown hook registration type %d for "
                             "phase %d/%d/\"%s\"",
                             meta->phase_num, meta->event, meta->name);
                IB_FTRACE_RET_STATUS(IB_EINVAL);
            }
            break;

        default:
            assert(0 && "Invalid bool for is_stream");
            break;

        }

        /* OK */
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Hook \"%s\" registration for phase "
                         "%d/%d/\"%s\" returned %s",
                         hook_type, meta->phase_num, meta->event, meta->name,
                         ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Initialize a rule engine object.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in,out] mp Memory pool to use for allocations
 * @param[out] p_rule_engine Pointer to new rule engine object
 *
 * @returns Status code
 */
static ib_status_t create_rule_engine(ib_engine_t *ib,
                                      ib_mpool_t *mp,
                                      ib_rule_engine_t **p_rule_engine)
{
    IB_FTRACE_INIT();
    ib_rule_engine_t *rule_engine;
    ib_status_t       rc;

    /* Create the rule object */
    rule_engine =
        (ib_rule_engine_t *)ib_mpool_calloc(mp, 1, sizeof(*rule_engine));
    if (rule_engine == NULL) {
        ib_log_error(ib,
                     "Rule engine failed to allocate rule engine object");
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Create the rule list */
    rc = ib_list_create(&(rule_engine->rule_list), mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to create rule list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    rc = ib_hash_create_nocase(&(rule_engine->rule_hash), mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to create rule hash: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    *p_rule_engine = rule_engine;
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Initialize a context's rule object.
 * @internal
 *
 * @param[in] ib Engine
 * @param[in,out] mp Memory pool to use for allocations
 * @param[out] p_ctx_rules Pointer to new rule context object
 *
 * @returns Status code
 */
static ib_status_t create_rule_context(ib_engine_t *ib,
                                       ib_mpool_t *mp,
                                       ib_rule_context_t **p_ctx_rules)
{
    IB_FTRACE_INIT();
    ib_rule_context_t *ctx_rules;
    ib_status_t        rc;

    /* Create the rule object */
    ctx_rules =
        (ib_rule_context_t *)ib_mpool_calloc(mp, 1, sizeof(*ctx_rules));
    if (ctx_rules == NULL) {
        ib_log_error(ib,
                     "Rule engine failed to allocate context rule object");
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Create the rule list */
    rc = ib_list_create(&(ctx_rules->rule_list), mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to initialize rule list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Create the rule enable list */
    rc = ib_list_create(&(ctx_rules->enable_list), mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to initialize rule list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    *p_ctx_rules = ctx_rules;
    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_engine_init(ib_engine_t *ib,
                                ib_module_t *mod)
{
    IB_FTRACE_INIT();
    ib_status_t rc;

    /* Create the rule engine object */
    rc = create_rule_engine(ib, ib->mp, &(ib->rule_engine));
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to create rule engine: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the rule callbacks */
    rc = register_callbacks(ib, ib->mp, ib->rule_engine);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to register phase callbacks: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_engine_ctx_init(ib_engine_t *ib,
                                    ib_module_t *mod,
                                    ib_context_t *ctx)
{
    IB_FTRACE_INIT();
    ib_status_t rc;

    /* If the rules are already initialized, do nothing */
    if (ctx->rules != NULL) {
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    /* Create the rule engine object */
    rc = create_rule_context(ib, ctx->mp, &(ctx->rules));
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to initialize context rules: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Initialize the rule sets */
    rc = init_ruleset(ib, ctx->mp, ctx->rules);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to initialize phase ruleset: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Enable rules that match tag / id
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx Current IronBee context
 * @param[in] enable Enable data
 * @param[in,out] rules List of rules to search for matches to @a enable
 *
 * @returns Status code
 */
static ib_status_t enable_rules(ib_engine_t *ib,
                                ib_context_t *ctx,
                                const ib_rule_enable_t *enable,
                                ib_list_t *ctx_rule_list)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(enable != NULL);
    assert(ctx_rule_list != NULL);

    ib_list_node_t *node;
    unsigned int matches = 0;

    switch (enable->enable_type) {

    case RULE_ENABLE_ID :
        ib_log_debug3_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                             "Looking for rule with ID \"%s\"",
                             enable->enable_str);
        IB_LIST_LOOP(ctx_rule_list, node) {
            ib_rule_ctx_data_t *ctx_rule;
            ctx_rule = (ib_rule_ctx_data_t *)ib_list_node_data(node);
            if (strcasecmp(enable->enable_str, ctx_rule->rule->meta.id) == 0) {
                ib_flags_set(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
                ib_log_debug2_cfg_ex(ib, ctx->mp,
                                     enable->file, enable->lineno,
                                     "Enabled ID matched rule \"%s\"",
                                     ctx_rule->rule->meta.id);
                IB_FTRACE_RET_STATUS(IB_OK);
                
            }
        }
        ib_log_error_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                            "No rule with ID of \"%s\" to enable",
                            enable->enable_str);
        IB_FTRACE_RET_STATUS(IB_ENOENT);
        break;

    case RULE_ENABLE_TAG :
        ib_log_debug3_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                             "Looking for rules with tag \"%s\"",
                             enable->enable_str);
        IB_LIST_LOOP(ctx_rule_list, node) {
            const ib_list_node_t *tagnode;
            ib_rule_ctx_data_t   *ctx_rule;
            ib_rule_t            *rule;

            ctx_rule = (ib_rule_ctx_data_t *)ib_list_node_data(node);
            rule = ctx_rule->rule;

            IB_LIST_LOOP_CONST(rule->meta.tags, tagnode) {
                const char *tag;

                tag = (const char *)ib_list_node_data_const(tagnode);
                if (strcasecmp(tag, enable->enable_str) == 0) {
                    ++matches;
                    ib_flags_set(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
                    ib_log_debug2_cfg_ex(ib, ctx->mp,
                                         enable->file, enable->lineno,
                                         "Enabled tag \"%s\" matched "
                                         "rule \"%s\" from ctx=\"%s\"",
                                         enable->enable_str,
                                         rule->meta.id,
                                         ib_context_full_get(rule->ctx));
                    break;
                }
            }
        }
        if (matches == 0) {
            ib_log_warning_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                                  "No rules with tag of \"%s\" to enable",
                                  enable->enable_str);
        }
        else {
            ib_log_debug_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                                "Enabled %u rules with tag of \"%s\"",
                                matches, enable->enable_str);
        }
        IB_FTRACE_RET_STATUS(IB_ENOENT);
        break;

    default:
        assert(0 && "Invalid rule enable type");
        
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_engine_ctx_close(ib_engine_t *ib,
                                     ib_module_t *mod,
                                     ib_context_t *ctx)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(mod != NULL);
    assert(ctx != NULL);

    ib_list_t      *all;
    ib_list_node_t *node;
    ib_flags_t      skip_flags;
    ib_status_t     rc;
    ib_context_t   *main_ctx = ib_context_main(ib);

    /* Don't enable rules for the main context */
    if (ctx == main_ctx) {
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    /* Create the list of all rules */
    rc = ib_list_create(&all, ctx->mp);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Rule engine failed to initialize rule list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Pass 0: Unmark all rules in the context's rule list */
    IB_LIST_LOOP(ctx->rules->rule_list, node) {
        ib_rule_t    *rule = (ib_rule_t *)ib_list_node_data(node);
        ib_flags_clear(rule->flags, IB_RULE_FLAG_MARK);
    }

    /* Pass 1: Loop through all of the rules in the main context, add them
     * to the list of all rules */
    ib_log_debug2(ib, "Adding rules from \"%s\" to ctx \"%s\" temp list",
                  ib_context_full_get(main_ctx),
                  ib_context_full_get(ctx));
    skip_flags = IB_RULE_FLAG_IN_CHAIN;
    IB_LIST_LOOP(main_ctx->rules->rule_list, node) {
        ib_rule_t          *ref = (ib_rule_t *)ib_list_node_data(node);
        ib_rule_t          *rule;
        ib_rule_ctx_data_t *ctx_rule;

        ib_log_debug3(ib, "Looking at rule \"%s\" from \"%s\"",
                      ref->meta.id, ib_context_full_get(ref->ctx));

        /* If it's a chained rule, skip it */
        if (ib_flags_any(ref->flags, skip_flags) == IB_TRUE) {
            continue;
        }

        /* Find the appropriate version of the rule to use */
        rc = ib_rule_lookup(ib, ctx, ref->meta.id, &rule);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to lookup rule \"%s\": %s",
                         ref->meta.id, ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        /* Create a rule ctx object for it, store it in the list */
        ctx_rule = ib_mpool_alloc(ctx->mp, sizeof(*ctx_rule));
        if (ctx_rule == NULL) {
            IB_FTRACE_RET_STATUS(IB_EALLOC);
        }
        ctx_rule->rule = rule;
        if (ib_flags_all(rule->flags, IB_RULE_FLAG_MAIN_CTX) == IB_TRUE) {
            ctx_rule->flags = IB_RULECTX_FLAG_NONE;
        }
        else {
            ctx_rule->flags = IB_RULECTX_FLAG_ENABLED;
            ib_flags_set(rule->flags, IB_RULE_FLAG_MARK);
        }
        rc = ib_list_push(all, ctx_rule);
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(IB_EALLOC);
        }
        ib_log_debug3(ib, "Adding rule \"%s\" from \"%s\" to ctx temp list",
                      rule->meta.id, ib_context_full_get(rule->ctx));
    }

    /* Pass 2: Loop through all of the context's rules, add them
     * to the list of all rules if they're not marked... */
    ib_log_debug2(ib, "Adding ctx rules to ctx \"%s\" temp list",
                  ib_context_full_get(ctx));
    skip_flags = (IB_RULE_FLAG_MARK | IB_RULE_FLAG_IN_CHAIN);
    IB_LIST_LOOP(ctx->rules->rule_list, node) {
        ib_rule_t          *rule = (ib_rule_t *)ib_list_node_data(node);
        ib_rule_ctx_data_t *ctx_rule;

        /* If the rule is chained or marked */
        if (ib_flags_all(rule->flags, skip_flags) == IB_TRUE) {
            ib_log_debug3(ib, "Skipping marked/chained rule \"%s\" from \"%s\"",
                          rule->meta.id, ib_context_full_get(rule->ctx));
            continue;
        }

        /* Create a ctx object for it, store it in the list */
        ctx_rule = ib_mpool_alloc(ctx->mp, sizeof(*ctx_rule));
        if (ctx_rule == NULL) {
            IB_FTRACE_RET_STATUS(IB_EALLOC);
        }
        ctx_rule->rule = rule;
        ib_flags_set(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
        rc = ib_list_push(all, ctx_rule);
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(IB_EALLOC);
        }
        ib_log_debug3(ib, "Adding rule \"%s\" from \"%s\" to ctx temp list",
                      rule->meta.id, ib_context_full_get(rule->ctx));
    }

    /* Pass 3: Enable all tagged / enabled rules */
    ib_log_debug2(ib, "Enabling id/tagged rules in \"%s\" temp list",
                  ib_context_full_get(ctx));
    IB_LIST_LOOP(ctx->rules->enable_list, node) {
        const ib_rule_enable_t *enable;

        enable = (const ib_rule_enable_t *)ib_list_node_data(node);

        /* Find rule */
        rc = enable_rules(ib, ctx, enable, all);
        if (rc != IB_OK) {
            ib_log_error_cfg_ex(ib, ctx->mp, enable->file, enable->lineno,
                                "Error enabling id/tagged rules "
                                "in \"%s\" temp list",
                                ib_context_full_get(ctx));
        }
    }

    /* Pass 4: Add all enabled rules to the appropriate execution list */
    ib_log_debug2(ib, "Adding enabled rules to ctx \"%s\" phase list",
                  ib_context_full_get(ctx));
    skip_flags = (IB_RULECTX_FLAG_ENABLED);
    IB_LIST_LOOP(all, node) {
        ib_rule_ctx_data_t *ctx_rule;
        ib_ruleset_phase_t *ruleset_phase;
        ib_list_t          *phase_list;
        ib_rule_phase_t     phase_num;
        ib_rule_t          *rule;

        ctx_rule = (ib_rule_ctx_data_t *)ib_list_node_data(node);
        assert(ctx_rule != NULL);
        rule = ctx_rule->rule;

        /* If it's not enabled, skip to the next rule */
        if (ib_flags_all(ctx_rule->flags, skip_flags) == IB_FALSE) {
            ib_log_debug3(ib, "Skipping disabled rule \"%s\" from \"%s\"",
                          rule->meta.id, ib_context_full_get(rule->ctx));
            continue;
        }

        /* Determine what phase list to add it into */
        phase_num = rule->meta.phase;
        ruleset_phase = &(ctx->rules->ruleset.phases[phase_num]);
        assert(ruleset_phase != NULL);
        assert(ruleset_phase->phase_meta == rule->phase_meta);
        phase_list = ruleset_phase->phase_list;

        /* Add it to the list */
        rc = ib_list_push(phase_list, (void *)ctx_rule);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Failed to add rule type=\"%s\" phase=%d "
                         "context=\"%s\": %s",
                         rule->phase_meta->is_stream ? "Stream" : "Normal",
                         ruleset_phase,
                         ib_context_full_get(ctx),
                         ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_log_debug(ib,
                     "Enabled rule \"%s\" rev=%u type=\"%s\" phase=%d/\"%s\" "
                     "for context \"%s\"",
                     rule->meta.id, rule->meta.revision,
                     rule->phase_meta->is_stream ? "Stream" : "Normal",
                     ruleset_phase->phase_num,
                     rule->phase_meta->name,
                     ib_context_full_get(ctx));
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_mpool_t *ib_rule_mpool(ib_engine_t *ib)
{
    IB_FTRACE_INIT();

    /* Return a pointer to the configuration memory pool */
    IB_FTRACE_RET_PTR(ib_mpool_t, ib_engine_pool_config_get(ib));
}


/**
 * Calculate a rule's position in a chain.
 * @internal
 *
 * @param[in] ib IronBee Engine
 * @param[in] rule The rule
 * @param[out] pos The rule's position in the chain
 *
 * @returns Status code
 */
static ib_status_t chain_position(ib_engine_t *ib,
                                  const ib_rule_t *rule,
                                  ib_num_t *pos)
{
    IB_FTRACE_INIT();
    ib_num_t n = 0;

    /* Loop through all parent rules */
    while (rule != NULL) {
        ++n;
        rule = rule->chained_from;
    }; /* while (rule != NULL); */
    *pos = n;
    IB_FTRACE_RET_STATUS(IB_OK);
}


/**
 * Generate the id for a chained rule.
 * @internal
 *
 * @param[in] ib IronBee Engine
 * @param[in,out] rule The rule
 * @param[in] force Set the ID even if it's already set
 *
 * @returns Status code
 */
static ib_status_t chain_gen_rule_id(ib_engine_t *ib,
                                     ib_rule_t *rule,
                                     ib_bool_t force)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    char idbuf[128];
    ib_num_t pos;

    /* If it's already set, do nothing */
    if ( (rule->meta.id != NULL) && (force == IB_FALSE) ) {
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    rc = chain_position(ib, rule, &pos);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }
    snprintf(idbuf, sizeof(idbuf), "\"%s\"/%d", rule->meta.chain_id, (int)pos);
    rule->meta.id = ib_mpool_strdup(ib_rule_mpool(ib), idbuf);
    if (rule->meta.id == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t DLL_PUBLIC ib_rule_create(ib_engine_t *ib,
                                      ib_context_t *ctx,
                                      const char *file,
                                      unsigned int lineno,
                                      ib_bool_t is_stream,
                                      ib_rule_t **prule)
{
    IB_FTRACE_INIT();
    ib_status_t                 rc;
    ib_rule_t                  *rule;
    ib_list_t                  *lst;
    ib_mpool_t                 *mp = ib_rule_mpool(ib);
    ib_rule_context_t          *context_rules;
    ib_rule_t                  *previous;
    const ib_rule_phase_meta_t *phase_meta;

    assert(ib != NULL);
    assert(ctx != NULL);

    /* Initialize the context's rule set (if required) */
    rc = ib_rule_engine_ctx_init(ib, NULL, ctx);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to initialize rules for context \"%s\"",
                     ib_context_full_get(ctx));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Look up the generic rule phase */
    rc = find_phase_stream_meta(is_stream, PHASE_NONE, &phase_meta);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error looking up rule phase: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Allocate the rule */
    rule = (ib_rule_t *)ib_mpool_calloc(mp, sizeof(ib_rule_t), 1);
    if (rule == NULL) {
        ib_log_error(ib, "Failed to allocate rule: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }
    rule->flags = IB_RULE_FLAG_NONE;
    rule->phase_meta = phase_meta;
    rule->meta.phase = PHASE_NONE;
    rule->meta.revision = 1;
    rule->meta.config_file = file;
    rule->meta.config_line = lineno;
    rule->ctx = ctx;
    rule->opinst = NULL;

    /* Note if this is the main context */
    if (ctx == ib_context_main(ib)) {
        rule->flags |= IB_RULE_FLAG_MAIN_CTX;
    }

    /* Meta tags list */
    lst = NULL;
    rc = ib_list_create(&lst, mp);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to create rule meta tags list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    rule->meta.tags = lst;

    /* Target list */
    lst = NULL;
    rc = ib_list_create(&lst, mp);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to create rule target field list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    rule->target_fields = lst;

    /* True Action list */
    lst = NULL;
    rc = ib_list_create(&lst, mp);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to create rule true action list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    rule->true_actions = lst;

    /* False Action list */
    lst = NULL;
    rc = ib_list_create(&lst, mp);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to create rule false action list: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }
    rule->false_actions = lst;

    /* Get the rule engine and previous rule */
    context_rules = ctx->rules;
    previous = context_rules->parser_data.previous;

    /*
     * If the previous rule has it's CHAIN flag set,
     * chain to that rule & update the current rule.
     */
    if (  (previous != NULL) &&
          ((previous->flags & IB_RULE_FLAG_CHAIN) != 0) )
    {
        previous->chained_rule = rule;
        rule->chained_from = previous;
        rule->meta.phase = previous->meta.phase;
        rule->phase_meta = previous->phase_meta;
        rule->meta.chain_id = previous->meta.chain_id;
        rule->flags |= IB_RULE_FLAG_IN_CHAIN;
    }

    /* Good */
    rule->parent_rlist = ctx->rules->rule_list;
    *prule = rule;
    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_flags_t ib_rule_required_op_flags(const ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    IB_FTRACE_RET_UINT(rule->phase_meta->required_op_flags);
}

ib_bool_t ib_rule_allow_tfns(const ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->phase_meta->flags & PHASE_FLAG_ALLOW_TFNS) != 0) {
        IB_FTRACE_RET_UINT(IB_TRUE);
    }
    else {
        IB_FTRACE_RET_UINT(IB_FALSE);
    }
}

ib_bool_t ib_rule_allow_chain(const ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->phase_meta->flags & PHASE_FLAG_ALLOW_CHAIN) != 0) {
        IB_FTRACE_RET_UINT(IB_TRUE);
    }
    else {
        IB_FTRACE_RET_UINT(IB_FALSE);
    }
}

ib_bool_t ib_rule_is_stream(const ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->phase_meta->flags & PHASE_FLAG_IS_STREAM) != 0) {
        IB_FTRACE_RET_UINT(IB_TRUE);
    }
    else {
        IB_FTRACE_RET_UINT(IB_FALSE);
    }
}

ib_status_t ib_rule_set_chain(ib_engine_t *ib,
                              ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert ((rule->phase_meta->flags & PHASE_FLAG_ALLOW_CHAIN) != 0);

    /* Set the chain flags */
    rule->flags |= IB_RULE_FLAGS_CHAIN;

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_set_phase(ib_engine_t *ib,
                              ib_rule_t *rule,
                              ib_rule_phase_t phase_num)
{
    IB_FTRACE_INIT();
    const ib_rule_phase_meta_t *phase_meta;
    ib_status_t rc;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->meta.phase != PHASE_NONE) &&
         (rule->meta.phase != phase_num) ) {
        ib_log_error(ib,
                     "Can't set rule phase: already set to %d",
                     rule->meta.phase);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    if (is_phase_num_valid(phase_num) != IB_TRUE) {
        ib_log_error(ib, "Can't set rule phase: Invalid phase %d",
                     phase_num);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Look up the real rule phase */
    rc = find_phase_stream_meta(rule->phase_meta->is_stream,
                                phase_num,
                                &phase_meta);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error looking up rule phase: %s",
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    rule->meta.phase = phase_num;
    rule->phase_meta = phase_meta;
    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Lookup rule by ID */
ib_status_t ib_rule_lookup(ib_engine_t *ib,
                           ib_context_t *ctx,
                           const char *id,
                           ib_rule_t **rule)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);
    assert(rule != NULL);

    ib_context_t *main_ctx = ib_context_main(ib);
    assert(main_ctx != NULL);

    ib_status_t rc;

    /* First, look in the context's rule set */
    if ( (ctx != NULL) && (ctx != main_ctx) ) {
        rc = ib_hash_get(ctx->rules->rule_hash, rule, id);
        if (rc != IB_ENOENT) {
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* If not in the context's rule set, look in the main context */
    rc = ib_hash_get(main_ctx->rules->rule_hash, rule, id);
    IB_FTRACE_RET_STATUS(rc);
}

/* Find rule matching a reference rule */
ib_status_t ib_rule_match(ib_engine_t *ib,
                          ib_context_t *ctx,
                          const ib_rule_t *ref,
                          ib_rule_t **rule)
{
    IB_FTRACE_INIT();

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(ref != NULL);
    assert(rule != NULL);

    ib_status_t rc;
    ib_rule_t *match = NULL;

    /* Lookup rule with matching ID */
    rc = ib_rule_lookup(ib, ctx, ref->meta.id, &match);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Verify that phase's match */
    if (ref->meta.phase != match->meta.phase) {
        ib_log_error(ib,
                     "\"%s\":%u: Rule phase mismatch @ \"%s\":%u",
                     ref->meta.config_file, ref->meta.config_line,
                     match->meta.config_file, match->meta.config_line);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    *rule = match;
    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_register(ib_engine_t *ib,
                             ib_context_t *ctx,
                             ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    ib_status_t        rc;
    ib_rule_context_t *context_rules;
    ib_rule_phase_t    phase_num;
    ib_rule_t         *lookup;

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    phase_num = rule->meta.phase;

    /* Sanity checks */
    if( (rule->phase_meta->flags & PHASE_FLAG_IS_VALID) == 0) {
        ib_log_error(ib, "Can't register rule: Phase is invalid");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    if (is_phase_num_valid(phase_num) != IB_TRUE) {
        ib_log_error(ib, "Can't register rule: Invalid phase %d", phase_num);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    assert (rule->meta.phase == rule->phase_meta->phase_num);

    /* Verify that we have a valid operator */
    if (rule->opinst == NULL) {
        ib_log_error(ib, "Can't register rule: No operator instance");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    if (rule->opinst->op == NULL) {
        ib_log_error(ib, "Can't register rule: No operator");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    if (rule->opinst->op->fn_execute == NULL) {
        ib_log_error(ib, "Can't register rule: No operator function");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Verify that the rule has an ID */
    if ( (rule->meta.id == NULL) && (rule->meta.chain_id == NULL) ) {
        ib_log_error(ib, "Can't register rule: No ID");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* If the "chain" flag is set, the chain ID is the rule's ID */
    if (rule->flags & IB_RULE_FLAGS_CHAIN) {
        if (rule->chained_from != NULL) {
            rule->meta.chain_id = rule->chained_from->meta.chain_id;
        }
        else {
            rule->meta.chain_id = rule->meta.id;
        }
        rule->meta.id = NULL;
        rc = chain_gen_rule_id(ib, rule, IB_TRUE);
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Get the rule engine and previous rule */
    context_rules = ctx->rules;

    /* Handle chained rule */
    if (rule->chained_from != NULL) {
        if ( (rule->chained_from->flags & IB_RULE_FLAG_VALID) == 0) {
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
        ib_log_debug3(ib,
                     "Registered rule \"%s\" chained from rule \"%s\"",
                     rule->meta.id, rule->chained_from->meta.id);
    }

    /* Put this rule in the hash */
    lookup = NULL;
    rc = ib_rule_match(ib, ctx, rule, &lookup);
    if ( (rc != IB_OK) && (rc != IB_ENOENT) ) {
        ib_log_error(ib,
                     "Error finding matching rule "
                     "for \"%s\" of context=\"%s\" @ \"%s\":%u: %s",
                     rule->meta.id, ib_context_full_get(ctx),
                     rule->meta.config_file, rule->meta.config_line,
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Now, if replace the existing rule if required */
    if ( (lookup != NULL) &&
         (rule->meta.revision <= lookup->meta.revision) )
    {
        ib_log_warning(ib,
                       "Not replacing rule \"%s\" of context=\"%s\" "
                       "rev=%u with rev=%u @ \"%s\":%u",
                       rule->meta.id,
                       ib_context_full_get(ctx),
                       lookup->meta.revision,
                       rule->meta.revision,
                       rule->meta.config_file, rule->meta.config_line);
        IB_FTRACE_RET_STATUS(IB_EEXIST);
    }
    else {
        rc = ib_hash_set(context_rules->rule_hash, rule->meta.id, rule);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error adding rule \"%s\" "
                         "to context=\"%s\" hash @ \"%s\":%u: %s",
                         rule->meta.id,
                         ib_context_full_get(ctx),
                         rule->meta.config_file, rule->meta.config_line,
                         ib_status_to_string(rc));
            IB_FTRACE_RET_STATUS(rc);
        }
        else if (lookup == NULL) {
            ib_log_debug(ib,
                         "Added rule \"%s\" rev=%u "
                         "to context=\"%s\" @ \"%s\":%u",
                         rule->meta.id, rule->meta.revision,
                         ib_context_full_get(ctx),
                         rule->meta.config_file, rule->meta.config_line);
        }
        else {
            ib_log_notice(ib,
                          "Replaced rule \"%s\" of context=\"%s\" "
                          "rev=%u with rev=%u @ \"%s\":%u",
                          rule->meta.id,
                          ib_context_full_get(ctx),
                          lookup->meta.revision,
                          rule->meta.revision,
                          rule->meta.config_file, rule->meta.config_line);
        }
    }

    /* Mark the rule as valid */
    rule->flags |= IB_RULE_FLAG_VALID;
    rc = ib_list_push(context_rules->rule_list, rule);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error adding rule \"%s\" "
                     "to context=\"%s\" list @ \"%s\":%u: %s",
                     rule->meta.id,
                     ib_context_full_get(ctx),
                     rule->meta.config_file, rule->meta.config_line,
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }


    /* Store off this rule for chaining */
    context_rules->parser_data.previous = rule;

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_enable(ib_engine_t *ib,
                           ib_context_t *ctx,
                           ib_rule_enable_type_t etype,
                           const char *name,
                           const char *file,
                           unsigned int lineno,
                           const char *str)
{
    IB_FTRACE_INIT();
    ib_status_t        rc;
    ib_rule_enable_t  *enable;

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(name != NULL);
    assert(str != NULL);

    /* Check the string name */
    if (*str == '\0') {
        ib_log_error(ib, "Invalid %s \"\" @ \"%s\":%u: %s",
                     name, file, lineno);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Just push it on the list */
    enable = ib_mpool_alloc(ctx->mp, sizeof(*enable));
    enable->enable_type = etype;
    enable->enable_str = str;
    enable->file = file;
    enable->lineno = lineno;

    /* Mark the rule as valid */
    rc = ib_list_push(ctx->rules->enable_list, enable);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error adding enable %s \"%s\" "
                     "to context=\"%s\" list @ \"%s\":%u: %s",
                     str, name,
                     ib_context_full_get(ctx),
                     file, lineno,
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t ib_rule_enable_id(ib_engine_t *ib,
                              ib_context_t *ctx,
                              const char *file,
                              unsigned int lineno,
                              const char *id)
{
    IB_FTRACE_INIT();
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(ib, ctx, RULE_ENABLE_ID, "id", file, lineno, id);

    IB_FTRACE_RET_STATUS(rc);
}

ib_status_t ib_rule_enable_tag(ib_engine_t *ib,
                               ib_context_t *ctx,
                               const char *file,
                               unsigned int lineno,
                               const char *tag)
{
    IB_FTRACE_INIT();
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(tag != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(ib, ctx, RULE_ENABLE_TAG, "tag", file, lineno, tag);

    IB_FTRACE_RET_STATUS(rc);
}

ib_status_t DLL_PUBLIC ib_rule_set_operator(ib_engine_t *ib,
                                            ib_rule_t *rule,
                                            ib_operator_inst_t *opinst)
{
    IB_FTRACE_INIT();

    if ( (rule == NULL) || (opinst == NULL) ) {
        ib_log_error(ib,
                     "Can't set rule operator: Invalid rule or operator");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    rule->opinst = opinst;

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t DLL_PUBLIC ib_rule_set_id(ib_engine_t *ib,
                                      ib_rule_t *rule,
                                      const char *id)
{
    IB_FTRACE_INIT();
    assert(ib != NULL);
    assert(rule != NULL);

    if ( (rule == NULL) || (id == NULL) ) {
        ib_log_error(ib, "Can't set rule id: Invalid rule or id");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    if (rule->chained_from != NULL) {
        ib_log_error(ib, "Can't set rule id of chained rule");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    if (rule->meta.id != NULL) {
        ib_log_error(ib, "Can't set rule id: already set to \"%s\"",
                     rule->meta.id);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    rule->meta.id = id;

    IB_FTRACE_RET_STATUS(IB_OK);
}

const char DLL_PUBLIC *ib_rule_id(const ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    assert(rule != NULL);
    IB_FTRACE_RET_CONSTSTR(rule->meta.id);
}

ib_status_t DLL_PUBLIC ib_rule_create_target(ib_engine_t *ib,
                                             const char *name,
                                             ib_list_t *tfn_names,
                                             ib_rule_target_t **target,
                                             ib_num_t *tfns_not_found)
{
    IB_FTRACE_INIT();
    ib_status_t rc;

    assert(ib != NULL);
    assert(target != NULL);

    /* Basic checks */
    if (name == NULL) {
        ib_log_error(ib,
                     "Can't add rule target: Invalid rule or target");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Allocate a rule field structure */
    *target = (ib_rule_target_t *)
        ib_mpool_calloc(ib_rule_mpool(ib), sizeof(**target), 1);
    if (target == NULL) {
        ib_log_error(ib,
                     "Error allocating rule target object \"%s\"", name);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Copy the name */
    (*target)->field_name = (char *)ib_mpool_strdup(ib_rule_mpool(ib), name);
    if ((*target)->field_name == NULL) {
        ib_log_error(ib, "Error copying target field name \"%s\"", name);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Create the field transformation list */
    rc = ib_list_create(&((*target)->tfn_list), ib_rule_mpool(ib));
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating field operator list for target \"%s\": %s",
                     name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Add the transformations in the list (if provided) */
    *tfns_not_found = 0;
    if (tfn_names != NULL) {
        ib_list_node_t *node = NULL;

        IB_LIST_LOOP(tfn_names, node) {
            const char *tfn = (const char *)ib_list_node_data(node);
            rc = ib_rule_target_add_tfn(ib, *target, tfn);
            if (rc == IB_ENOENT) {
                ++(*tfns_not_found);
            }
            else if (rc != IB_OK) {
                IB_FTRACE_RET_STATUS(rc);
            }
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Add a target to a rule */
ib_status_t DLL_PUBLIC ib_rule_add_target(ib_engine_t *ib,
                                          ib_rule_t *rule,
                                          ib_rule_target_t *target)
{
    IB_FTRACE_INIT();
    ib_status_t rc = IB_OK;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(target != NULL);

    /* Push the field */
    rc = ib_list_push(rule->target_fields, (void *)target);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to add target \"%s\" to rule \"%s\": %s",
                     target->field_name, rule->meta.id,
                     ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Add a transformation to a target */
ib_status_t DLL_PUBLIC ib_rule_target_add_tfn(ib_engine_t *ib,
                                              ib_rule_target_t *target,
                                              const char *name)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    ib_tfn_t *tfn;

    assert(ib != NULL);
    assert(target != NULL);
    assert(name != NULL);

    /* Lookup the transformation by name */
    rc = ib_tfn_lookup(ib, name, &tfn);
    if (rc == IB_ENOENT) {
        ib_log_error(ib, "Transformation \"%s\" not found", name);
        IB_FTRACE_RET_STATUS(rc);
    }
    else if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error looking up trans \"%s\" for target \"%s\": %s",
                     name, target->field_name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Add the transformation to the list */
    rc = ib_list_push(target->tfn_list, tfn);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error adding transformation \"%s\" to list: %s",
                     name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Add a transformation to all targets of a rule */
ib_status_t DLL_PUBLIC ib_rule_add_tfn(ib_engine_t *ib,
                                       ib_rule_t *rule,
                                       const char *name)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    ib_tfn_t *tfn;
    ib_list_node_t *node = NULL;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(name != NULL);

    /* Lookup the transformation by name */
    rc = ib_tfn_lookup(ib, name, &tfn);
    if (rc == IB_ENOENT) {
        ib_log_error(ib, "Transformation \"%s\" not found", name);
        IB_FTRACE_RET_STATUS(rc);
    }
    else if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error looking up trans \"%s\" for rule \"%s\": %s",
                     name, rule->meta.id, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Walk through the list of targets, add the transformation to it */
    IB_LIST_LOOP(rule->target_fields, node) {
        ib_rule_target_t *target = (ib_rule_target_t *)ib_list_node_data(node);
        rc = ib_rule_target_add_tfn(ib, target, name);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error adding tfn \"%s\" to target \"%s\" "
                         "rule \"%s\":%d",
                         name, target->field_name, rule->meta.id);
        }
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Add an action to a rule */
ib_status_t DLL_PUBLIC ib_rule_add_action(ib_engine_t *ib,
                                          ib_rule_t *rule,
                                          ib_action_inst_t *action,
                                          ib_rule_action_t which)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    assert(ib != NULL);

    if ( (rule == NULL) || (action == NULL) ) {
        ib_log_error(ib,
                     "Can't add rule action: Invalid rule or action");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Add the rule to the appropriate action list */
    if (which == RULE_ACTION_TRUE) {
        rc = ib_list_push(rule->true_actions, (void *)action);
    }
    else if (which == RULE_ACTION_FALSE) {
        rc = ib_list_push(rule->false_actions, (void *)action);
    }
    else {
        rc = IB_EINVAL;
    }

    /* Problems? */
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to add rule action \"%s\": %s",
                     action->action->name, ib_status_to_string(rc));
        IB_FTRACE_RET_STATUS(rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t DLL_PUBLIC ib_rule_chain_invalidate(ib_engine_t *ib,
                                                ib_rule_t *rule)
{
    IB_FTRACE_INIT();
    ib_status_t rc = IB_OK;
    ib_status_t tmp_rc;
    ib_flags_t orig;

    if (rule == NULL) {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    orig = rule->flags;
    rule->flags &= ~IB_RULE_FLAG_VALID;

    /* Invalidate the entire chain upwards */
    if (rule->chained_from != NULL) {
        tmp_rc = ib_rule_chain_invalidate(ib, rule->chained_from);
        if (tmp_rc != IB_OK) {
            rc = tmp_rc;
        }
    }

    /* If this rule was previously valid, walk down the chain, too */
    if ( (orig & IB_RULE_FLAG_VALID) && (rule->chained_rule != NULL) ) {
        tmp_rc = ib_rule_chain_invalidate(ib, rule->chained_rule);
        if (tmp_rc != IB_OK) {
            rc = tmp_rc;
        }
    }

    IB_FTRACE_RET_STATUS(rc);
}
