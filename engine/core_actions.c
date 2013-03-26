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
 * @brief IronBee --- core actions
 *
 * @author Craig Forbes <cforbes@qualys.com>
 */

#include "ironbee_config_auto.h"

#include "core_private.h"
#include "engine_private.h"

#include <ironbee/action.h>
#include <ironbee/bytestr.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/flags.h>
#include <ironbee/logevent.h>
#include <ironbee/mpool.h>
#include <ironbee/rule_engine.h>
#include <ironbee/rule_logger.h>
#include <ironbee/string.h>
#include <ironbee/types.h>
#include <ironbee/util.h>

#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include <inttypes.h>

/**
 * Data types for the setvar action.
 */
typedef enum {
    SETVAR_STRSET,                /**< Set to a constant string */
    SETVAR_NUMSET,                /**< Set to a constant number */
    SETVAR_NUMADD,                /**< Add to a value (counter) */
    SETVAR_NUMSUB,                /**< Subtract from a value (counter) */
    SETVAR_NUMMULT,               /**< Multiply to a value (counter) */
    SETVAR_FLOATSET,              /**< Set to a constant float. */
    SETVAR_FLOATADD,              /**< Add to a float. */
    SETVAR_FLOATSUB,              /**< Subtract from a float. */
    SETVAR_FLOATMULT,             /**< Multiply to a float. */
} setvar_op_t;


/**
 * Actual implementation of setvar operators for numbers.
 */
typedef ib_status_t (*setvar_num_op_fn_t)(
    const ib_num_t n1,
    const ib_num_t n2,
    ib_num_t *out);

/**
 * Actual implementation of setvar operators for floats.
 */
typedef ib_status_t (*setvar_float_op_fn_t)(
    const ib_float_t f1,
    const ib_float_t f2,
    ib_float_t *out
);

static ib_status_t setvar_num_sub_op(
    const ib_num_t n1,
    const ib_num_t n2,
    ib_num_t *out)
{

    *out = n1 - n2;

    return IB_OK;
}
static ib_status_t setvar_num_mult_op(
    const ib_num_t n1,
    const ib_num_t n2,
    ib_num_t *out)
{

    *out = n1 * n2;

    return IB_OK;
}
static ib_status_t setvar_num_add_op(
    const ib_num_t n1,
    const ib_num_t n2,
    ib_num_t *out)
{

    *out = n1 + n2;

    return IB_OK;
}

static ib_status_t setvar_float_sub_op(
    const ib_float_t f1,
    const ib_float_t f2,
    ib_float_t *out)
{

    *out = f1 - f2;

    return IB_OK;
}
static ib_status_t setvar_float_mult_op(
    const ib_float_t f1,
    const ib_float_t f2,
    ib_float_t *out)
{

    *out = f1 * f2;

    return IB_OK;
}
static ib_status_t setvar_float_add_op(
    const ib_float_t f1,
    const ib_float_t f2,
    ib_float_t *out)
{

    *out = f1 + f2;

    return IB_OK;
}

typedef union {
    ib_num_t         num;         /**< Numeric value */
    ib_float_t       flt;         /**< Float value. */
    ib_bytestr_t    *bstr;        /**< String value */
} setvar_value_t;

/**
 * Structure storing setvar instance data.
 */
typedef struct {
    setvar_op_t      op;          /**< Setvar operation */
    char            *name;        /**< Field name */
    bool             name_expand; /**< Field name should be expanded */
    ib_ftype_t       type;        /**< Data type */
    setvar_value_t   value;       /**< Value. value.num, flt, or bstr. */
} setvar_data_t;

/**
 * Data types for the setflag action.
 */
typedef enum {
    setflag_op_set,               /**< Set the flag */
    setflag_op_clear              /**< Clear the flag */
} setflag_op_t;

typedef struct {
    const ib_tx_flag_map_t *flag;
    setflag_op_t            op;
} setflag_data_t;

/**
 * Data types for the event action.
 */
typedef struct {
    ib_logevent_type_t      event_type; /**< Type of the event */
} event_data_t;

/**
 * Unwrap two fields, perform the operation, and write the result.
 *
 * @param[in,out] tx Transaction object. tx->data is updated with the result.
 * @param[in] rule_exec Rule execution environment.
 * @param[in] setvar_data SetVar data.
 * @param[in] cur_field The current field being used for the left hand side.
 *            If cur_field == NULL a value of zero is substituted.
 * @param[in] name The name of the new field to create and add to tx->data.
 * @param[in] nlen The length of name.
 * @param[in] op The operator to perform on the two fields.
 *            The value in cur_field is passed as the first argument
 *            to @a op. The second argument to @a op is the
 *            parameter presented at configuration time.
 *
 * @returns
 *   - IB_OK
 */
static ib_status_t setvar_float_op(
    ib_tx_t *tx,
    const ib_rule_exec_t *rule_exec,
    const setvar_data_t *setvar_data,
    ib_field_t *cur_field,
    const char *name,
    size_t nlen,
    setvar_float_op_fn_t op)
{

    assert(setvar_data->type == IB_FTYPE_FLOAT);

    ib_status_t rc;

    /* If it doesn't exist, create the variable with a value of zero */
    if (cur_field == NULL) {

        /* Create the new_field field */
        rc = ib_data_add_num_ex(tx->data, name, nlen, 0, &cur_field);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to add field "
                              "\"%.*s\": %s",
                              (int)nlen, name,
                              ib_status_to_string(rc));
            return rc;
        }
    }

    /* Handle float types. */
    if (cur_field->type == IB_FTYPE_FLOAT) {
        ib_float_t flt;
        rc = ib_field_value(cur_field, ib_ftype_float_out(&flt));
        if (rc != IB_OK) {
            return rc;
        }

        op(flt, setvar_data->value.flt, &flt);

        ib_field_setv(cur_field, ib_ftype_float_in(&flt));
    }
    else {
        ib_rule_log_error(rule_exec,
                          "setvar: field \"%.*s\" type %d "
                          "invalid for NUMADD",
                          (int)nlen, name, cur_field->type);
        return IB_EINVAL;
    }
    return IB_OK;
}
/**
 * Unwrap two fields, perform the operation, and write the result.
 *
 * @param[in,out] tx Transaction object. tx->data is updated with the result.
 * @param[in] rule_exec Rule execution environment.
 * @param[in] setvar_data SetVar data.
 * @param[in] cur_field The current field being used for the left hand side.
 *            If cur_field == NULL a value of zero is substituted.
 * @param[in] name The name of the new field to create and add to tx->data.
 * @param[in] nlen The length of name.
 * @param[in] op The operator to perform on the two fields.
 *            The value in cur_field is passed as the first argument
 *            to @a op. The second argument to @a op is the
 *            parameter presented at configuration time.
 *
 * @returns
 *   - IB_OK
 */
static ib_status_t setvar_num_op(
    ib_tx_t *tx,
    const ib_rule_exec_t *rule_exec,
    const setvar_data_t *setvar_data,
    ib_field_t *cur_field,
    const char *name,
    size_t nlen,
    setvar_num_op_fn_t op)
{

    assert(setvar_data->type == IB_FTYPE_NUM);

    ib_status_t rc;

    /* If it doesn't exist, create the variable with a value of zero */
    if (cur_field == NULL) {

        /* Create the new_field field */
        rc = ib_data_add_num_ex(tx->data, name, nlen, 0, &cur_field);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to add field "
                              "\"%.*s\": %s",
                              (int)nlen, name,
                              ib_status_to_string(rc));
            return rc;
        }
    }

    /* Handle num and unum types */
    if (cur_field->type == IB_FTYPE_NUM) {
        ib_num_t num;
        rc = ib_field_value(cur_field, ib_ftype_num_out(&num));
        if (rc != IB_OK) {
                return rc;
        }

        op(num, setvar_data->value.num, &num);

        ib_field_setv(cur_field, ib_ftype_num_in(&num));
    }
    else {
        ib_rule_log_error(rule_exec,
                          "setvar: field \"%.*s\" type %d "
                          "invalid for NUMADD",
                          (int)nlen, name, cur_field->type);
        return IB_EINVAL;
    }
    return IB_OK;
}


/**
 * Create function for the setflags action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] ctx Current context.
 * @param[in] mp Memory pool to use for allocation
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_setflags_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *parameters,
    ib_action_inst_t *inst,
    void *cbdata)
{
    const ib_tx_flag_map_t *flag;
    setflag_op_t op;

    if (parameters == NULL) {
        return IB_EINVAL;
    }

    if (*parameters == '!') {
        op = setflag_op_clear;
        ++parameters;
    }
    else {
        op = setflag_op_set;
    }

    for (flag = ib_core_fields_tx_flags();  flag->name != NULL;  ++flag) {
        if (strcasecmp(flag->name, parameters) == 0) {
            setflag_data_t *data;

            if (flag->read_only) {
                return IB_EINVAL;
            }
            data = ib_mpool_alloc(mp, sizeof(*data));
            if (data == NULL) {
                return IB_EALLOC;
            }
            data->op = op;
            data->flag = flag;
            inst->data = (void *)data;
            return IB_OK;
        }
    }
    return IB_EINVAL;
}

/**
 * Execute function for the "set flag" action
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Name of the flag to set
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_setflag_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    /* Data will be a setflag_data_t */
    const setflag_data_t *opdata = (const setflag_data_t *)data;
    ib_num_t value;
    ib_status_t rc;

    switch (opdata->op) {

    case setflag_op_set:
        ib_tx_flags_set(rule_exec->tx, opdata->flag->tx_flag);
        value = 1;
        break;

    case setflag_op_clear:
        ib_tx_flags_unset(rule_exec->tx, opdata->flag->tx_flag);
        value = 0;
        break;

    default:
        return IB_EINVAL;
    }

    /* This fails because ib_data_remove() doesn't handle fields within
     * collections */
    rc = ib_data_remove(rule_exec->tx->data, opdata->flag->tx_name, NULL);
    if (rc != IB_OK) {
        /* Do nothing */
    }

    rc = ib_data_add_num(rule_exec->tx->data, opdata->flag->tx_name,
                         value, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Create function for the event action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] ctx Current context.
 * @param[in] mp Memory pool to use for allocation
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_event_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *parameters,
    ib_action_inst_t *inst,
    void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(mp != NULL);
    assert(inst != NULL);

    event_data_t *event_data;
    ib_logevent_type_t event_type;

    if (parameters == NULL) {
        event_type = IB_LEVENT_TYPE_OBSERVATION;
    }
    else if (strcasecmp(parameters, "observation") == 0) {
        event_type = IB_LEVENT_TYPE_OBSERVATION;
    }
    else if (strcasecmp(parameters, "alert") == 0) {
        event_type = IB_LEVENT_TYPE_ALERT;
    }
    else {
        return IB_EINVAL;
    }

    /* Allocate an event data object, populate it */
    event_data = ib_mpool_alloc(mp, sizeof(*event_data));
    if (event_data == NULL) {
        return IB_EALLOC;
    }
    event_data->event_type = event_type;
    inst->data = (void *)event_data;

    return IB_OK;
}

/**
 * Event action execution callback.
 *
 * Create and event and log it.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 */
static ib_status_t act_event_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec != NULL);
    assert(data != NULL);

    ib_status_t  rc;
    ib_logevent_t *event;
    const char *expanded;
    ib_field_t *field;
    const ib_rule_t *rule = rule_exec->rule;
    ib_tx_t *tx = rule_exec->tx;
    const event_data_t *event_data = (const event_data_t *)data;

    ib_rule_log_debug(rule_exec, "Creating event via action");

    /* Expand the message string */
    if ( (rule->meta.flags & IB_RULEMD_FLAG_EXPAND_MSG) != 0) {
        char *tmp;
        rc = ib_data_expand_str(tx->data, rule->meta.msg, false, &tmp);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "event: Failed to expand string '%s': %s",
                              rule->meta.msg, ib_status_to_string(rc));
            return rc;
        }
        expanded = tmp;
    }
    else if (rule->meta.msg != NULL) {
        expanded = rule->meta.msg;
    }
    else {
        expanded = "";
    }

    /* Create the event */
    rc = ib_logevent_create(
        &event,
        tx->mp,
        ib_rule_id(rule),
        event_data->event_type,
        IB_LEVENT_ACTION_UNKNOWN,
        rule->meta.confidence,
        rule->meta.severity,
        expanded
    );
    if (rc != IB_OK) {
        return rc;
    }

    /* Set the data */
    if (rule->meta.data != NULL) {
        if ( (rule->meta.flags & IB_RULEMD_FLAG_EXPAND_DATA) != 0) {
            char *tmp;
            rc = ib_data_expand_str(tx->data, rule->meta.data, false, &tmp);
            if (rc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "event: Failed to expand data '%s': %s",
                                  rule->meta.data, ib_status_to_string(rc));
                return rc;
            }
            expanded = tmp;
        }
        else {
            expanded = rule->meta.data;
        }
        rc = ib_logevent_data_set(event, expanded, strlen(expanded));
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "event: Failed to set data: %s",
                              ib_status_to_string(rc));
            return rc;
        }
    }

    /* Link to rule tags. */
    /// @todo Probably need to copy here
    event->tags = rule->meta.tags;

    /* Populate fields */
    if (! ib_flags_any(rule->flags, IB_RULE_FLAG_NO_TGT)) {
        rc = ib_data_get(tx->data, "FIELD_NAME_FULL", &field);
        if ( (rc == IB_OK) && (field->type == IB_FTYPE_NULSTR) ) {
            const char *name = NULL;
            rc = ib_field_value(field, ib_ftype_nulstr_out(&name));
            if ( (rc != IB_OK) || (name != NULL) ) {
                ib_logevent_field_add(event, name);
            }
        }
        else if ( (rc == IB_OK) && (field->type == IB_FTYPE_BYTESTR) ) {
            const ib_bytestr_t *bs;
            rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
            if (rc == IB_OK) {
                ib_logevent_field_add_ex(
                    event,
                    (const char *)ib_bytestr_const_ptr(bs),
                    ib_bytestr_length(bs)
                );
            }
        }
    }

    /* Set the actions if appropriate */
    if (ib_tx_flags_isset(tx,
                          (IB_TX_BLOCK_ADVISORY |
                           IB_TX_BLOCK_PHASE |
                           IB_TX_BLOCK_IMMEDIATE)) )
    {
        event->rec_action = IB_LEVENT_ACTION_BLOCK;
    }

    /* Log the event. */
    rc = ib_logevent_add(tx, event);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add the event to the rule execution */
    rc = ib_rule_log_exec_add_event(rule_exec->exec_log, event);
    if (rc != IB_OK) {
        /* todo: Ignore this? */
    }

    return IB_OK;
}

/**
 * Create function for the setvar action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] ctx Current context.
 * @param[in] mp Memory pool to use for allocation
 * @param[in] params Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_setvar_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *params,
    ib_action_inst_t *inst,
    void *cbdata)
{
    size_t nlen;                 /* Name length */
    const char *eq;              /* '=' character in @a params */
    const char *mod;             /* '+'/'-'/'*' character in params */
    const char *value;           /* Value in params */
    bool compat_syntax = false;  /* Using old 0.5.x =+ compatibility? */
    size_t vlen;                 /* Length of value */
    setvar_data_t *data;         /* Data for the execute function */
    ib_status_t rc;              /* Status code */

    if (params == NULL) {
        return IB_EINVAL;
    }

    /* Simple checks; params should look like '<name>=[<value>]' */
    eq = strchr(params, '=');
    if ( (eq == NULL) || (eq == params) ) {
        return IB_EINVAL;
    }

    /* Calculate name length */
    if (*(eq-1) == '*' || *(eq-1) == '-' || *(eq-1) == '+') {
        mod = eq - 1;
        nlen = (mod - params);
    }
    /* For backward compatibility, support the old =+ and =- (yuck) */
    else if ( (*(eq+1) == '-') || (*(eq+1) == '+') ) {
        mod = eq + 1;
        nlen = (eq - params);
        ++eq;   /* Make the value / vlen log below work */
        compat_syntax = true;
    }
    else {
        mod = NULL;
        nlen = (eq - params);
    }

    /* Determine the type of the value */
    value = (eq + 1);
    vlen = strlen(value);

    /* Create the data structure for the execute function */
    data = ib_mpool_alloc(mp, sizeof(*data) );
    if (data == NULL) {
        return IB_EALLOC;
    }

    /* Does the name need to be expanded? */
    rc = ib_data_expand_test_str_ex(params, nlen, &(data->name_expand));
    if (rc != IB_OK) {
        return rc;
    }

    /* Copy the name */
    data->name = ib_mpool_memdup_to_str(mp, params, nlen);
    if (data->name == NULL) {
        return IB_EALLOC;
    }

    /* Create the value */
    rc = ib_string_to_num_ex(value, vlen, 0, &(data->value.num));
    if (rc == IB_OK) {
        data->type = IB_FTYPE_NUM;
        if (mod == NULL) {
            data->op = SETVAR_NUMSET;
        }
        else if (*mod == '+') {
            data->op = SETVAR_NUMADD;
        }
        else if (*mod == '-') {
            data->op = SETVAR_NUMSUB;
        }
        else if (*mod == '*') {
            data->op = SETVAR_NUMMULT;
        }

        goto success;
    }

    rc = ib_string_to_float(value, &(data->value.flt));
    if (rc == IB_OK) {
        data->type = IB_FTYPE_FLOAT;
        if (mod == NULL) {
            data->op = SETVAR_FLOATSET;
        }
        else if (*mod == '+') {
            data->op = SETVAR_FLOATADD;
        }
        else if (*mod == '-') {
            data->op = SETVAR_FLOATSUB;
        }
        else if (*mod == '*') {
            data->op = SETVAR_FLOATMULT;
        }

        goto success;
    }
    else {
        bool expand = false;

        /* Special case for handling =+ compatibility */
        if (compat_syntax) {
            --value;
            ++vlen;
        }
        else if (mod != NULL) {
            ib_log_error(ib, "setvar: '%c' not supported for strings", *mod);
            return IB_EINVAL;
        }

        rc = ib_data_expand_test_str_ex(value, vlen, &expand);
        if (rc != IB_OK) {
            return rc;
        }
        else if (expand) {
            inst->flags |= IB_ACTINST_FLAG_EXPAND;
        }

        rc = ib_bytestr_dup_nulstr(&(data->value.bstr), mp, value);
        if (rc != IB_OK) {
            return rc;
        }
        data->type = IB_FTYPE_BYTESTR;
        data->op = SETVAR_STRSET;
    }

success:
    inst->data = data;
    return IB_OK;
}

/**
 * Expand a name from the DPI
 *
 * @param[in] rule_exec The rule executing this action
 * @param[in] label Label to use for debug / error messages
 * @param[in] setvar_data Setvar parameters
 * @param[out] exname Expanded name
 * @param[out] exnlen Length of @a exname
 *
 * @returns Status code
 */
static ib_status_t expand_name(const ib_rule_exec_t *rule_exec,
                               const char *label,
                               const setvar_data_t *setvar_data,
                               const char **exname,
                               size_t *exnlen)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(label);
    assert(setvar_data);
    assert(exname);
    assert(exnlen);

    /* Readability: Alias a common field. */
    const char *name = setvar_data->name;
    ib_tx_t *tx = rule_exec->tx;

    /* If it's expandable, expand it */
    if (setvar_data->name_expand) {
        char *tmp;
        size_t len;
        ib_status_t rc;

        rc = ib_data_expand_str_ex(tx->data,
                                   name, strlen(name),
                                   false, false,
                                   &tmp, &len);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "%s: Failed to expand name \"%s\": %s",
                              label, name, ib_status_to_string(rc));
            return rc;
        }
        *exname = tmp;
        *exnlen = len;
        ib_rule_log_debug(rule_exec,
                          "%s: Expanded variable name from \"%s\" to \"%.*s\"",
                          label, name, (int)len, tmp);
    }
    else {
        *exname = name;
        *exnlen = strlen(name);

        ib_rule_log_debug(rule_exec, "%s: No expansion of %s", label, name);
    }


    return IB_OK;
}

/**
 * Get a field from the DPI
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] name Name of the value
 * @param[in] namelen Length of @a name
 * @param[out] field The field from the DPI
 *
 * @returns Status code
 */
static ib_status_t get_data_value(const ib_rule_exec_t *rule_exec,
                                  const char *name,
                                  size_t namelen,
                                  ib_field_t **field)
{
    assert(rule_exec != NULL);
    assert(name != NULL);
    assert(field != NULL);

    ib_field_t *cur = NULL;
    ib_status_t rc;
    ib_list_t *list;
    ib_list_node_t *first;
    size_t elements;
    ib_tx_t *tx = rule_exec->tx;

    rc = ib_data_get_ex(tx->data, name, namelen, &cur);
    if ( (rc == IB_ENOENT) || (cur == NULL) ) {
        *field = NULL;
        return IB_OK;
    }
    else if (rc != IB_OK) {
        *field = NULL;
        return rc;
    }

    /* If we got back something other than a list, or it's name matches
     * what we asked for, we're done */
    if ( (cur->type != IB_FTYPE_LIST) ||
         ((cur->nlen == namelen) && (memcmp(name, cur->name, namelen) == 0)) )
    {
        *field = cur;
        return IB_OK;
    }

    /*
     * If we got back a list and the field name doesn't match the name we
     * requested, assume that we got back a filtered list.
     */
    rc = ib_field_value(cur, ib_ftype_list_mutable_out(&list) );
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "setvar: Failed to get list from \"%.*s\": %s",
                          (int)namelen, name, ib_status_to_string(rc));
        return rc;
    }

    /* No elements?  Filtered list with no values.  Return NULL. */
    elements = ib_list_elements(list);
    if (elements == 0) {
        *field = NULL;
        return IB_OK;
    }

    if (elements != 1) {
        ib_rule_log_notice(rule_exec,
                           "setvar:Got back list with %zd elements", elements);
        return IB_EINVAL;
    }

    /* Use the first (only) element in the list as our field */
    first = ib_list_first(list);
    if (first == NULL) {
        ib_rule_log_error(rule_exec,
                          "setvar: Failed to get first list element "
                          "from \"%.*s\": %s",
                          (int)namelen, name, ib_status_to_string(rc));
        return IB_EUNKNOWN;
    }

    /* Finally, take the data from the first node.  Check and mate. */
    *field = (ib_field_t *)first->data;
    return IB_OK;
}

/**
 * Contains logic for expanding act_setvar_execute field names for assignment.
 *
 *  @param[in] rule_exec The rule execution object
 *  @param[in] label Label (used for log messages)
 *  @param[in] setvar_data
 *  @param[in] flags
 *  @param[out] expanded The expanded string, possibly allocated from
 *              the memory pool or the result of
 *              an ib_mpool_alloc(mp, 0) call.
 *  @param[out] exlen The length of @a expanded.
 *
 *  @return
 *    - IB_OK @a expanded and @a exlen are populated.
 *    - IB_EINVAL if there was an internal error during expansion.
 *    - IB_EALLOC on memory errors.
 *
 */
static ib_status_t expand_data(
    const ib_rule_exec_t *rule_exec,
    const char *label,
    const setvar_data_t *setvar_data,
    ib_flags_t flags,
    char **expanded,
    size_t *exlen)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(label);
    assert(setvar_data);
    assert(expanded);
    assert(exlen);

    ib_status_t rc;
    ib_tx_t *tx = rule_exec->tx;

    /* If setvar_data contains a byte string, we might expand it. */
    if (setvar_data->type == IB_FTYPE_BYTESTR) {

        /* Pull the data out of the bytestr */
        const uint8_t *bsdata = ib_bytestr_ptr(setvar_data->value.bstr);
        size_t bslen = ib_bytestr_length(setvar_data->value.bstr);

        /* Expand the string */
        if (flags & IB_ACTINST_FLAG_EXPAND) {

            rc = ib_data_expand_str_ex(
                tx->data, (const char *)bsdata, bslen,
                false, false, expanded, exlen);
            if (rc != IB_OK) {
                ib_rule_log_debug(
                    rule_exec,
                    "%s: Failed to expand string \"%.*s\": %s",
                    label, (int) bslen, (const char *)bsdata,
                    ib_status_to_string(rc));
                return rc;
            }

            ib_rule_log_debug(
                rule_exec,
                "%s: Field \"%s\" was expanded.",
                label,
                setvar_data->name);

            if (ib_rule_dlog_level(tx->ctx) >= IB_RULE_DLOG_TRACE) {
                const char *escaped = ib_util_hex_escape(tx->mp, bsdata, bslen);
                if (escaped != NULL) {
                    ib_rule_log_debug(
                        rule_exec,
                        "%s: Field \"%s\" has value: %s",
                        label,
                        setvar_data->name,
                        escaped);
                }
            }
        }
        else if (bsdata == NULL) {
            assert(bslen == 0);

            /* Get a non-null pointer that should never be dereferenced. */
            *expanded = ib_mpool_alloc(tx->mp, 0);
            *exlen = bslen;

            ib_rule_log_debug(
                rule_exec,
                "%s: Field \"%s\" is null.",
                label,
                setvar_data->name);
        }
        else {
            *expanded = ib_mpool_memdup(tx->mp, bsdata, bslen);
            if (*expanded == NULL) {
                ib_rule_log_debug(
                    rule_exec,
                    "%s: Failed to copy string \"%.*s\"",
                    label,
                    (int)bslen,
                    bsdata);
                return IB_EALLOC;
            }
            *exlen = bslen;
        }
    }
    else {
        ib_rule_log_debug(
            rule_exec,
            "%s: Did not expand \"%s\" because it is not a byte string.",
            label,
            setvar_data->name
            );

    }

    return IB_OK;
}

/**
 * Execute function for the "set variable" action
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Setvar data (setvar_data_t *)
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 *    - IB_OK The operator succeeded.
 *    - IB_EINVAL if there was an internal error during expansion.
 *    - IB_EALLOC on memory errors.
 */
static ib_status_t act_setvar_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(data != NULL);
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);

    ib_status_t rc;
    ib_field_t *cur_field = NULL;
    ib_field_t *new_field;
    ib_tx_t *tx = rule_exec->tx;
    const char *name = NULL; /* Name of the field we are setting. */
    size_t nlen;             /* Name length. */
    char *value = NULL;      /* Value we are setting the field name to. */
    size_t vlen;             /* Value length. */

    /* Data should be a setvar_data_t created in our create function */
    const setvar_data_t *setvar_data = (const setvar_data_t *)data;

    /* Expand the name (if required) */
    rc = expand_name(rule_exec, "setvar", setvar_data, &name, &nlen);
    if (rc != IB_OK) {
        return rc;
    }

    /* Get the current value */
    rc = get_data_value(rule_exec, name, nlen, &cur_field);
    if (rc != IB_OK) {
        return rc;
    }

    /* If setvar_data contains a byte string, we might expand it. */
    rc = expand_data(rule_exec, "setvar", setvar_data, flags, &value, &vlen);
    if ( rc != IB_OK ) {
        return rc;
    }

    switch(setvar_data->op) {

    /* Handle bytestr operations (currently only set) */
    case SETVAR_STRSET:
        assert(setvar_data->type == IB_FTYPE_BYTESTR);
        ib_bytestr_t *bs = NULL;

        if (cur_field != NULL) {
            ib_data_remove_ex(tx->data, name, nlen, NULL);
        }

        /* Create a bytestr to hold it. */
        rc = ib_bytestr_alias_mem(&bs, tx->mp, (uint8_t *)value, vlen);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to create bytestring "
                              "for field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }

        /* Create the new_field field */
        rc = ib_field_create(&new_field,
                             tx->mp,
                             name, nlen,
                             setvar_data->type,
                             ib_ftype_bytestr_in(bs));
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to create field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }

        /* Add the field to the DPI */
        rc = ib_data_add(tx->data, new_field);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to add field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }
        break;

    /* Numerical operation : Set */
    case SETVAR_FLOATSET:
        assert(setvar_data->type == IB_FTYPE_FLOAT);

        if (cur_field != NULL) {
            ib_data_remove_ex(tx->data, name, nlen, NULL);
        }

        /* Create the new_field field */
        rc = ib_field_create(&new_field,
                             tx->mp,
                             name, nlen,
                             setvar_data->type,
                             ib_ftype_float_in(&setvar_data->value.flt));
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to create field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }

        /* Add the field to the DPI */
        rc = ib_data_add(tx->data, new_field);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to add field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }
        break;

    /* Numerical operation : Set */
    case SETVAR_NUMSET:
        assert(setvar_data->type == IB_FTYPE_NUM);

        if (cur_field != NULL) {
            ib_data_remove_ex(tx->data, name, nlen, NULL);
        }

        /* Create the new_field field */
        rc = ib_field_create(&new_field,
                             tx->mp,
                             name, nlen,
                             setvar_data->type,
                             ib_ftype_num_in(&setvar_data->value.num));
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to create field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }

        /* Add the field to the DPI */
        rc = ib_data_add(tx->data, new_field);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "setvar: Failed to add field \"%.*s\": %s",
                              (int)nlen, name, ib_status_to_string(rc));
            return rc;
        }
        break;

    /* Numerical operation : Add */
    case SETVAR_FLOATADD:
        rc = setvar_float_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_float_add_op);
        break;

    /* Numerical operation : Sub */
    case SETVAR_FLOATSUB:
        rc = setvar_float_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_float_sub_op);
        break;

    /* Numerical operation : Mult */
    case SETVAR_FLOATMULT:
        rc = setvar_float_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_float_mult_op);
        break;

    /* Numerical operation : Add */
    case SETVAR_NUMADD:
        rc = setvar_num_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_num_add_op);
        break;

    /* Numerical operation : Sub */
    case SETVAR_NUMSUB:
        rc = setvar_num_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_num_sub_op);
        break;

    /* Numerical operation : Mult */
    case SETVAR_NUMMULT:
        rc = setvar_num_op(
            tx,
            rule_exec,
            setvar_data,
            cur_field,
            name,
            nlen,
            &setvar_num_mult_op);
        break;
    }

    return rc;
}

/**
 * Find event from this rule
 *
 * @param[in] rule_exec The rule execution object
 * @param[out] event Matching event
 *
 * @return
 *   - IB_OK (if found)
 *   - IB_ENOENT if not found
 *   - Errors returned by ib_logevent_get_all()
 */
static ib_status_t get_event(const ib_rule_exec_t *rule_exec,
                             ib_logevent_t **event)
{
    assert(rule_exec != NULL);

    ib_status_t rc;
    ib_list_t *event_list;
    ib_list_node_t *event_node;
    ib_tx_t *tx = rule_exec->tx;

    rc = ib_logevent_get_all(tx, &event_list);
    if (rc != IB_OK) {
        return rc;
    }
    event_node = ib_list_last(event_list);
    if (event_node == NULL) {
        return IB_ENOENT;
    }
    ib_logevent_t *e = (ib_logevent_t *)event_node->data;
    if (strcmp(e->rule_id, ib_rule_id(rule_exec->rule)) == 0) {
        *event = e;
        return IB_OK;
    }

    return IB_ENOENT;
}

/**
 * Set the IB_TX_BLOCK_ADVISORY flag and set the DPI value @c FLAGS:BLOCK=1.
 *
 * @param[in] rule_exec The rule execution object
 *
 * @returns IB_OK if successful.
 *
 * @return
 *   - IB_OK on success.
 *   - Errors by ib_data_add_num.
 *   - Other if an event exists, but cannot be retrieved for this action.
 */
static ib_status_t act_block_advisory_execute(
    const ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);

    ib_tx_t *tx = rule_exec->tx;
    ib_status_t rc;
    ib_num_t ib_num_one = 1;
    ib_logevent_t *event;

    /* Don't re-set the flag because it bloats the DPI value FLAGS
     * with lots of BLOCK entries. */
    if (!ib_tx_flags_isset(tx, IB_TX_BLOCK_ADVISORY)) {

        /* Set the flag in the transaction. */
        ib_tx_flags_set(tx, IB_TX_BLOCK_ADVISORY);

        /* When doing an advisory block, mark the DPI with FLAGS:BLOCK=1. */
        rc = ib_data_add_num(tx->data, "FLAGS:BLOCK", ib_num_one, NULL);
        if (rc != IB_OK) {
            ib_rule_log_error(
                rule_exec,
                "Could not set value FLAGS:BLOCK=1: %s",
                ib_status_to_string(rc));
            return rc;
        }

        /* Update the event (if required) */
        rc = get_event(rule_exec, &event);
        if (rc == IB_OK) {
            event->rec_action = IB_LEVENT_ACTION_BLOCK;
        }
        else if (rc != IB_ENOENT) {
            ib_rule_log_error(rule_exec,
                              "Failed to fetch event "
                              "associated with this action: %s",
                              ib_status_to_string(rc));
            return rc;
        }
    }

    ib_rule_log_debug(rule_exec, "Advisory block.");

    return IB_OK;
}

/**
 * Set the IB_TX_BLOCK_PHASE flag in the tx.
 *
 * @param[in] rule_exec The rule execution object
 *
 * @return
 *   - IB_OK on success.
 *   - Other if an event exists, but cannot be retrieved for this action.
 */
static ib_status_t act_block_phase_execute(
    const ib_rule_exec_t *rule_exec)
{
    ib_status_t rc;
    ib_logevent_t *event;
    ib_tx_t *tx = rule_exec->tx;

    ib_tx_flags_set(tx, IB_TX_BLOCK_PHASE);

    /* Update the event (if required) */
    rc = get_event(rule_exec, &event);
    if (rc == IB_OK) {
        event->rec_action = IB_LEVENT_ACTION_BLOCK;
    }
    else if (rc != IB_ENOENT) {
        ib_rule_log_error(rule_exec,
                          "Failed phase block: %s.", ib_status_to_string(rc));
        return rc;
    }

    ib_rule_log_trace(rule_exec, "Phase block.");

    return IB_OK;
}

/**
 * Set the IB_TX_BLOCK_IMMEDIATE flag in the tx.
 *
 * @param[in] rule_exec The rule execution object
 *
 * @returns
 *   - IB_OK on success.
 *   - Other if an event exists, but cannot be retrieved for this action.
 */
static ib_status_t act_block_immediate_execute(
    const ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);

    ib_status_t rc;
    ib_logevent_t *event;

    ib_tx_flags_set(rule_exec->tx, IB_TX_BLOCK_IMMEDIATE);

    /* Update the event (if required) */
    rc = get_event(rule_exec, &event);
    if (rc == IB_OK) {
        event->rec_action = IB_LEVENT_ACTION_BLOCK;
    }
    else if (rc != IB_ENOENT) {
        ib_rule_log_error(rule_exec,
                          "Failed immediate block: %s.",
                          ib_status_to_string(rc));
        return rc;
    }

    ib_rule_log_debug(rule_exec, "Immediate block.");

    return IB_OK;
}

/**
 * The function that implements flagging a particular block type.
 *
 * @param[in] rule_exec The rule execution object
 *
 * @return Return code
 */
typedef ib_status_t(*act_block_execution_t)(
    const ib_rule_exec_t *rule_exec
);

/**
 * Internal block action structure.
 *
 * This holds a pointer to the block callback that will be used.
 */
struct act_block_t {
    act_block_execution_t execute; /**< What block method should be used. */
};
typedef struct act_block_t act_block_t;

/**
 * Executes the function stored in @a data.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Cast to an @c act_block_t and the @c execute field is
 *            called on the given @a tx.
 * @param[in] flags Flags. Unused.
 * @param[in] cbdata Callback data. Unused.
 */
static ib_status_t act_block_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(data);

    ib_status_t rc = ((const act_block_t *)data)->execute(rule_exec);

    return rc;
}

/**
 * Create / initialize a new instance of an action.
 *
 * @param[in] ib IronBee engine.
 * @param[in] ctx Context.
 * @param[in] mp Memory pool.
 * @param[in] params Parameters. These may be "immediate", "phase", or
 *            "advise". If null, "advisory" is assumed.
 *            These select the type of block that will be put in place
 *            by deciding which callback (act_block_phase_execute(),
 *            act_block_immediate_execute(), or act_block_advise_execute())
 *            is assigned to the rule data object.
 * @param[out] inst The instance being initialized.
 * @param[in] cbdata Unused.
 *
 * @return IB_OK on success or IB_EALLOC if the callback data
 *         cannot be initialized for the rule.
 */
static ib_status_t act_block_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *params,
    ib_action_inst_t *inst,
    void *cbdata)
{
    act_block_t *act_block =
        (act_block_t *)ib_mpool_alloc(mp, sizeof(*act_block));
    if ( act_block == NULL ) {
        return IB_EALLOC;
    }

    /* When params are NULL, use advisory blocking by default. */
    if ( params == NULL ) {
        act_block->execute = &act_block_advisory_execute;
    }

    /* Just note that a block should be done, according to this rule. */
    else if ( ! strcasecmp("advisory", params) ) {
        act_block->execute = &act_block_advisory_execute;
    }

    /* Block at the end of the phase. */
    else if ( ! strcasecmp("phase", params) ) {
        act_block->execute = &act_block_phase_execute;
    }

    /* Immediate blocking. Block ASAP. */
    else if ( ! strcasecmp("immediate", params) ) {
        act_block->execute = &act_block_immediate_execute;
    }

    /* As with params == NULL, the default is to use an advisory block. */
    else {
        act_block->execute = &act_block_advisory_execute;
    }

    /* Assign the built up context object. */
    inst->data = act_block;

    return IB_OK;
}

/**
 * Holds the status code that a @c status action will set in the @c tx.
 */
struct act_status_t {
    int block_status; /**< The status to copy into @c tx->block_status. */
};
typedef struct act_status_t act_status_t;

/**
 * Set the @c block_status value in @a tx.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data The act_status_t that contains the @c block_status
 *            to assign to @c tx->block_status.
 * @param[in] flags The flags used to create this rule. Unused.
 * @param[in] cbdata Callback data. Unused.
 *
 * @returns IB_OK.
 */
static ib_status_t act_status_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec != NULL);
    assert(data != NULL);

    /* NOTE: Range validation of block_status is done in act_status_create. */
    rule_exec->tx->block_status = ((act_status_t *)data)->block_status;

    return IB_OK;
}

/**
 * Create an action that sets the TX's block_status value.
 *
 * @param[in] ib The IronBee engine.
 * @param[in] ctx The current context. Unused.
 * @param[in] mp The memory pool that will allocate the act_status_t
 *            holder for the status value.
 * @param[in] params The parameters. This is a string representing
 *            an integer from 200 to 599, inclusive.
 * @param[out] inst The action instance that will be initialized.
 * @param[in] cbdata Unused.
 *
 * @return
 *   - IB_OK on success.
 *   - IB_EALLOC on an allocation error from mp.
 *   - IB_EINVAL if @a param is NULL or not convertible with
 *               @c atoi(const @c char*) to an integer in the range 200
 *               through 599 inclusive.
 */
static ib_status_t act_status_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *params,
    ib_action_inst_t *inst,
    void *cbdata)
{
    assert(inst);
    assert(mp);

    act_status_t *act_status;
    ib_num_t block_status;
    ib_status_t rc;

    act_status = (act_status_t *) ib_mpool_alloc(mp, sizeof(*act_status));
    if (act_status == NULL) {
        return IB_EALLOC;
    }

    if (params == NULL) {
        ib_log_error(ib, "Action status must be given a parameter "
                     "x where 200 <= x < 600.");
        return IB_EINVAL;
    }

    block_status = atoi(params);

    if ( (block_status < 200) || (block_status >= 600) ) {
        ib_log_error(ib,
                     "Action status must be given a parameter "
                     "x where 200 <= x < 600. It was given %s.",
                     params);
        return IB_EINVAL;
    }

    act_status->block_status = block_status;

    rc = ib_field_create(&(inst->fparam), mp, IB_FIELD_NAME("param"),
                         IB_FTYPE_NUM, ib_ftype_num_in(&block_status));
    if (rc != IB_OK) {
        /* Do nothing */
    }

    inst->data = act_status;

    return IB_OK;
}

/**
 * Expand a header name from the DPI
 *
 * @todo This should be removed, and expand_name should be used
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] label Label to use for debug / error messages
 * @param[in] name Name to expand
 * @param[in] expandable Is @a expandable?
 * @param[out] exname Expanded name
 * @param[out] exnlen Length of @a exname
 *
 * @returns Status code
 */
static ib_status_t expand_name_hdr(const ib_rule_exec_t *rule_exec,
                                   const char *label,
                                   const char *name,
                                   bool expandable,
                                   const char **exname,
                                   size_t *exnlen)
{
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);
    assert(label != NULL);
    assert(name != NULL);
    assert(exname != NULL);
    assert(exnlen != NULL);

    /* If it's expandable, expand it */
    if (expandable) {
        char *tmp;
        size_t len;
        ib_status_t rc;

        rc = ib_data_expand_str(rule_exec->tx->data, name, false, &tmp);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "%s: Failed to expand name \"%s\": %s",
                              label, name, ib_status_to_string(rc));
            return rc;
        }
        len = strlen(tmp);
        *exname = tmp;
        *exnlen = len;
        ib_log_debug_tx(rule_exec->tx,
                        "%s: Expanded variable name from "
                        "\"%s\" to \"%.*s\"",
                        label, name, (int)len, tmp);
    }
    else {
        *exname = name;
        *exnlen = strlen(name);
    }

    return IB_OK;
}

/**
 * Expand a string from the DPI
 *
 * @todo Should call ib_data_expand_str_ex()
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] label Label to use for debug / error messages
 * @param[in] str String to expand
 * @param[in] flags Action flags
 * @param[out] expanded Expanded string
 * @param[out] exlen Length of @a expanded
 *
 * @returns Status code
 */
static ib_status_t expand_str(const ib_rule_exec_t *rule_exec,
                              const char *label,
                              const char *str,
                              ib_flags_t flags,
                              const char **expanded,
                              size_t *exlen)
{
    assert(rule_exec != NULL);
    assert(label != NULL);
    assert(str != NULL);
    assert(expanded != NULL);
    assert(exlen != NULL);
    ib_tx_t *tx = rule_exec->tx;

    /* If it's expandable, expand it */
    if ( (flags & IB_ACTINST_FLAG_EXPAND) != 0) {
        char *tmp;
        size_t len;
        ib_status_t rc;

        rc = ib_data_expand_str(tx->data, str, false, &tmp);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "%s: Failed to expand \"%s\": %s",
                              label, str, ib_status_to_string(rc));
            return rc;
        }
        len = strlen(tmp);
        *expanded = tmp;
        *exlen = len;
        ib_rule_log_debug(rule_exec,
                          "%s: Expanded \"%s\" to \"%.*s\"",
                          label, str, (int)len, tmp);
    }
    else {
        *expanded = str;
        *exlen = strlen(str);
    }
    return IB_OK;
}

/**
 * Holds the name of the header and the value to set/append/merge
 * and regexp with which to edit as applicable.
 */
struct act_header_data_t {
    const char *name;        /**< Name of the header to operate on. */
    bool        name_expand; /**< Is name expandable? */
    const char *value;       /**< Value to replace the header with. */
    ib_rx_t    *rx;          /**< Regexp substitution to apply to header */
};
typedef struct act_header_data_t act_header_data_t;

/**
 * Common create routine for delResponseHeader and delRequestHeader action.
 *
 * @param[in] ib The IronBee engine.
 * @param[in] ctx The context.
 * @param[in] mp The memory pool this is allocated out of.
 * @param[in] params Parameters of the format name=&lt;header name&gt;.
 * @param[out] inst The action instance being initialized.
 * @param[in] cbdata Unused.
 *
 * @return IB_OK on success. IB_EALLOC if a memory allocation fails.
 */
static ib_status_t act_del_header_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *params,
    ib_action_inst_t *inst,
    void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(mp != NULL);
    assert(params != NULL);
    assert(inst != NULL);

    act_header_data_t *act_data =
        (act_header_data_t *)ib_mpool_alloc(mp, sizeof(*act_data));
    ib_status_t rc;

    if (act_data == NULL) {
        return IB_EALLOC;
    }

    if ( (params == NULL) || (strlen(params) == 0) ) {
        ib_log_error(ib, "Operation requires a parameter.");
        return IB_EINVAL;
    }

    act_data->name = ib_mpool_strdup(mp, params);

    if (act_data->name == NULL) {
        return IB_EALLOC;
    }

    /* Does the name need to be expanded? */
    rc = ib_data_expand_test_str_ex(params, strlen(params),
                                    &(act_data->name_expand));
    if (rc != IB_OK) {
        return rc;
    }

    inst->data = act_data;

    return IB_OK;
}

/**
 * Common create routine for setResponseHeader and setRequestHeader actions.
 *
 * @param[in] ib The IronBee engine.
 * @param[in] ctx The context.
 * @param[in] mp The memory pool this is allocated out of.
 * @param[in] params Parameters of the format name=&gt;header name&lt;.
 * @param[out] inst The action instance being initialized.
 * @param[in] cbdata Unused.
 *
 * @return IB_OK on success. IB_EALLOC if a memory allocation fails.
 */
static ib_status_t act_set_header_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *params,
    ib_action_inst_t *inst,
    void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(mp != NULL);
    assert(params != NULL);
    assert(inst != NULL);

    size_t name_len;
    size_t value_len;
    size_t params_len;
    char *equals_idx;
    act_header_data_t *act_data =
        (act_header_data_t *)ib_mpool_alloc(mp, sizeof(*act_data));
    bool expand = false;
    ib_status_t rc;
    size_t value_offs = 1;

    if (act_data == NULL) {
        return IB_EALLOC;
    }

    if ( (params == NULL) || (strlen(params) == 0) ) {
        ib_log_error(ib, "Operation requires a parameter");
        return IB_EINVAL;
    }

    equals_idx = index(params, '=');

    /* If the returned value was NULL it is an error. */
    if (equals_idx == NULL) {
        ib_log_error(ib, "Format for parameter is name=value: %s", params);
        return IB_EINVAL;
    }
    else if (equals_idx[1] == '~') {
        value_offs = 2;    /* =~ for a regexp substitution arg */
    }

    /* Compute string lengths needed for parsing out name and value. */
    params_len = strlen(params);
    name_len = equals_idx - params;
    value_len = params_len - name_len - value_offs;

    act_data->name = (const char *)ib_mpool_memdup(mp, params, name_len+1);
    if (act_data->name == NULL) {
        return IB_EALLOC;
    }

    /* Terminate name with '\0'. This replaces the '=' that was copied.
     * Notice that we strip the const-ness of this value to make this one
     * assignment. */
    ((char *)act_data->name)[name_len] = '\0';

    /* Does the name need to be expanded? */
    rc = ib_data_expand_test_str_ex(act_data->name, name_len,
                                    &(act_data->name_expand));
    if (rc != IB_OK) {
        return rc;
    }

    act_data->value = (value_len == 0)?
        ib_mpool_strdup(mp, ""):
        ib_mpool_strdup(mp, equals_idx+value_offs);
    if (act_data->value == NULL) {
        return IB_EALLOC;
    }

    rc = ib_data_expand_test_str_ex(act_data->value, value_len, &expand);
    if (rc != IB_OK) {
        return rc;
    }
    else if (expand) {
        inst->flags |= IB_ACTINST_FLAG_EXPAND;
    }

    /* If we have a regexp and we're not expanding, we can compile it now */
    if (!(inst->flags & IB_ACTINST_FLAG_EXPAND) && (value_offs == 2)) {
        act_data->rx = ib_rx_compile(mp, act_data->value);
    }
    else {
        act_data->rx = NULL;
    }

    inst->data = act_data;
    return IB_OK;
}

/**
 * Set the request header in @c tx->data.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 * @param[in] rule_exec The rule execution object
 */
static ib_status_t act_set_request_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *value;
    size_t value_len;
    const char *name;
    size_t name_len;
    ib_tx_t *tx = rule_exec->tx;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "setRequestHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    rc = expand_str(rule_exec, "setRequestHeader",
                    act_data->value, flags, &value, &value_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_rule_log_debug(rule_exec, "Setting request header \"%.*s\"=\"%.*s\"",
                      (int)name_len, name, (int)value_len, value);

    /* Note: ignores lengths for now */
    rc = ib_server_header(rule_exec->ib->server, tx,
                          IB_SERVER_REQUEST, IB_HDR_SET,
                          name, value, NULL);

    return rc;
}

/**
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 */
static ib_status_t act_edit_request_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *value;
    size_t value_len;
    const char *name;
    size_t name_len;
    ib_tx_t *tx = rule_exec->tx;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "editRequestHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    rc = expand_str(rule_exec, "editRequestHeader",
                    act_data->value, flags, &value, &value_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_rule_log_debug(rule_exec,
                      "Applying regexp to request header \"%.*s\"=~\"%.*s\"",
                      (int)name_len, name, (int)value_len, value);

    /* Note: ignores lengths for now */
    rc = ib_server_header(tx->ib->server, tx, IB_SERVER_REQUEST, IB_HDR_EDIT,
                          name, value, act_data->rx);

    return rc;
}

/**
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 * @param[in] rule_exec The rule execution object
 */
static ib_status_t act_del_request_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *name;
    size_t name_len;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "delRequestHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_rule_log_debug(rule_exec, "Deleting request header \"%.*s\"",
                      (int)name_len, name);
    /* Note: ignores lengths for now */
    rc = ib_server_header(rule_exec->ib->server,
                          rule_exec->tx,
                          IB_SERVER_REQUEST,
                          IB_HDR_UNSET,
                          name,
                          "", NULL);

    return rc;
}

/**
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 */
static ib_status_t act_set_response_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *value;
    size_t value_len;
    const char *name;
    size_t name_len;
    ib_tx_t *tx = rule_exec->tx;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "setResponseHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    rc = expand_str(rule_exec, "setResponseHeader", act_data->value, flags,
                    &value, &value_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_rule_log_debug(rule_exec, "Setting response header \"%.*s\"=\"%.*s\"",
                      (int)name_len, name, (int)value_len, value);

    /* Note: ignores lengths for now */
    rc = ib_server_header(tx->ib->server, tx,
                          IB_SERVER_RESPONSE, IB_HDR_SET,
                          name, value, NULL);

    return rc;
}

/**
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 * @param[in] rule_exec The rule execution object
 *
 */
static ib_status_t act_edit_response_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *value;
    size_t value_len;
    const char *name;
    size_t name_len;
    ib_tx_t *tx = rule_exec->tx;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "editResponseHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    rc = expand_str(rule_exec, "editResponseHeader",
                    act_data->value, flags,
                    &value, &value_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_log_debug_tx(tx, "Applying regexp to response header \"%.*s\"=~\"%.*s\"",
                    (int)name_len, name, (int)value_len, value);

    /* Note: ignores lengths for now */
    rc = ib_server_header(tx->ib->server, tx,
                          IB_SERVER_RESPONSE, IB_HDR_EDIT,
                          name, value, act_data->rx);

    return rc;
}

/**
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data needed for execution.
 * @param[in] flags Action instance flags
 * @param[in] cbdata Unused.
 *
 * @returns IB_OK if successful.
 * @param[in] rule_exec The rule execution object
 */
static ib_status_t act_del_response_header_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(rule_exec);
    assert(rule_exec->tx);
    assert(rule_exec->ib);
    assert(rule_exec->ib->server);
    assert(data);

    ib_status_t rc;
    act_header_data_t *act_data = (act_header_data_t *)data;
    const char *name;
    size_t name_len;

    /* Expand the name (if required) */
    rc = expand_name_hdr(rule_exec, "delResponseHeader",
                         act_data->name, act_data->name_expand,
                         &name, &name_len);
    if (rc != IB_OK) {
        return rc;
    }

    ib_rule_log_debug(rule_exec, "Deleting response header \"%.*s\"",
                      (int)name_len, name);

    /* Note: ignores lengths for now */
    rc = ib_server_header(rule_exec->ib->server,
                          rule_exec->tx,
                          IB_SERVER_RESPONSE,
                          IB_HDR_UNSET,
                          name,
                          "", NULL);

    return rc;
}

/**
 * Create function for the allow action.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] ctx Current context.
 * @param[in] mp Memory pool to use for allocation
 * @param[in] parameters Constant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_allow_create(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_mpool_t *mp,
    const char *parameters,
    ib_action_inst_t *inst,
    void *cbdata)
{
    ib_flags_t flags = IB_TX_FNONE;
    ib_flags_t *idata;

    if (parameters == NULL) {
        flags |= IB_TX_ALLOW_ALL;
    }
    else if (strcasecmp(parameters, "phase") == 0) {
        flags |= IB_TX_ALLOW_PHASE;
    }
    else if (strcasecmp(parameters, "request") == 0) {
        flags |= IB_TX_ALLOW_REQUEST;
    }
    else {
        return IB_EINVAL;
    }

    idata = ib_mpool_alloc(mp, sizeof(*idata));
    if (idata == NULL) {
        return IB_EALLOC;
    }

    *idata = flags;
    inst->data = idata;

    return IB_OK;
}

/**
 * Allow action.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Not used.
 * @param[in] flags Flags. Unused.
 * @param[in] cbdata Unused.
 */
static ib_status_t act_allow_execute(
    const ib_rule_exec_t *rule_exec,
    void *data,
    ib_flags_t flags,
    void *cbdata)
{
    assert(data != NULL);
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);

    const ib_flags_t *pflags = (const ib_flags_t *)data;
    ib_flags_t set_flags = *pflags;

    /* For post process, treat ALLOW_ALL like ALLOW_PHASE */
    if ( (rule_exec->rule->meta.phase == PHASE_POSTPROCESS) &&
         (ib_flags_all(set_flags, IB_TX_ALLOW_ALL)) )
    {
        set_flags |= IB_TX_ALLOW_PHASE;
    }

    /* Set the flags in the TX */
    ib_tx_flags_set(rule_exec->tx, set_flags);

    /* For ALLOW_PHASE, store the current phase */
    if (ib_flags_all(set_flags, IB_TX_ALLOW_PHASE)) {
        rule_exec->tx->allow_phase = rule_exec->rule->meta.phase;
    }

    return IB_OK;
}

/**
 * Audit log parts action data
 */
typedef struct {
    const ib_list_t   *oplist;   /**< Flags operation list */
} act_auditlog_parts_t;

/**
 * Create function for the AuditLogParts action
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] ctx Current context.
 * @param[in] mp Memory pool to use for allocation
 * @param[in] parameters Con=d10-h10stant parameters from the rule definition
 * @param[in,out] inst Action instance
 * @param[in] cbdata Unused.
 *
 * @returns Status code
 */
static ib_status_t act_auditlogparts_create(
    ib_engine_t      *ib,
    ib_context_t     *ctx,
    ib_mpool_t       *mp,
    const char       *parameters,
    ib_action_inst_t *inst,
    void             *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(mp != NULL);
    assert(inst != NULL);

    ib_status_t           rc;
    act_auditlog_parts_t *idata;
    ib_list_t            *oplist;
    const ib_strval_t    *map;

    /* Create the list */
    rc = ib_list_create(&oplist, mp);
    if (rc != IB_OK) {
        return rc;
    }

    /* Get the auditlog parts map */
    rc = ib_core_auditlog_parts_map(&map);
    if (rc != IB_OK) {
        return rc;
    }

    /* Parse the parameter string */
    rc = ib_flags_oplist_parse(map, mp, parameters, ",", oplist);
    if (rc != IB_OK) {
        return rc;
    }

    /* Create and populate the instance data */
    idata = ib_mpool_alloc(mp, sizeof(*idata));
    if (idata == NULL) {
        return IB_EALLOC;
    }
    idata->oplist = oplist;

    inst->data = idata;
    return IB_OK;
}

/**
 * Execution function for the AuditLogParts action
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] data Instance data
 * @param[in] flags Flags. Unused.
 * @param[in] cbdata Unused.
 */
static ib_status_t act_auditlogparts_execute(
    const ib_rule_exec_t *rule_exec,
    void                 *data,
    ib_flags_t            flags,
    void                 *cbdata)
{
    assert(data != NULL);
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);

    const act_auditlog_parts_t *idata = (const act_auditlog_parts_t *)data;
    ib_tx_t    *tx = rule_exec->tx;
    ib_flags_t  parts = tx->auditlog_parts;
    ib_flags_t  parts_flags = 0;
    ib_flags_t  parts_mask = 0;
    ib_status_t rc;

    rc = ib_flags_oplist_apply(idata->oplist, &parts_flags, &parts_mask);
    if (rc != IB_OK) {
        return rc;
    }

    /* Merge the set flags with the previous value. */
    parts = ( (parts_flags & parts_mask) | (parts & ~parts_mask) );

    ib_rule_log_debug(rule_exec, "Updating auditlog parts from "
                      "0x%08"PRIx64" to 0x%08"PRIx64,
                      (uint64_t)tx->auditlog_parts, (uint64_t)parts);
    tx->auditlog_parts = parts;

    return IB_OK;
}

ib_status_t ib_core_actions_init(ib_engine_t *ib, ib_module_t *mod)
{
    ib_status_t  rc;

    /* Register the set flag action. */
    rc = ib_action_register(ib,
                            "setflag",
                            IB_ACT_FLAG_NONE,
                            act_setflags_create, NULL,
                            NULL, /* no destroy function */ NULL,
                            act_setflag_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the set variable action. */
    rc = ib_action_register(ib,
                            "setvar",
                            IB_ACT_FLAG_NONE,
                            act_setvar_create, NULL,
                            NULL, /* no destroy function */ NULL,
                            act_setvar_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the event action. */
    rc = ib_action_register(ib,
                            "event",
                            IB_ACT_FLAG_NONE,
                            act_event_create, NULL,
                            NULL, /* no destroy function */ NULL,
                            act_event_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the block action. */
    rc = ib_action_register(ib,
                            "block",
                            IB_ACT_FLAG_NONE,
                            act_block_create, NULL,
                            NULL, NULL,
                            act_block_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the allow actions. */
    rc = ib_action_register(ib,
                            "allow",
                            IB_ACT_FLAG_NONE,
                            act_allow_create, NULL,
                            NULL, NULL,
                            act_allow_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the AuditLogParts action. */
    rc = ib_action_register(ib,
                            "AuditLogParts",
                            IB_ACT_FLAG_NONE,
                            act_auditlogparts_create, NULL,
                            NULL, NULL,
                            act_auditlogparts_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the status action to modify how block is performed. */
    rc = ib_action_register(ib,
                            "status",
                            IB_ACT_FLAG_NONE,
                            act_status_create, NULL,
                            NULL, NULL,
                            act_status_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "setRequestHeader",
                            IB_ACT_FLAG_NONE,
                            act_set_header_create, NULL,
                            NULL, NULL,
                            act_set_request_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "editRequestHeader",
                            IB_ACT_FLAG_NONE,
                            act_set_header_create, NULL,
                            NULL, NULL,
                            act_edit_request_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "delRequestHeader",
                            IB_ACT_FLAG_NONE,
                            act_del_header_create, NULL,
                            NULL, NULL,
                            act_del_request_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "setResponseHeader",
                            IB_ACT_FLAG_NONE,
                            act_set_header_create, NULL,
                            NULL, NULL,
                            act_set_response_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "editResponseHeader",
                            IB_ACT_FLAG_NONE,
                            act_set_header_create, NULL,
                            NULL, NULL,
                            act_edit_response_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_action_register(ib,
                            "delResponseHeader",
                            IB_ACT_FLAG_NONE,
                            act_del_header_create, NULL,
                            NULL, NULL,
                            act_del_response_header_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}
