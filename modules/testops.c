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
 * @brief IronBee --- TestOps module
 *
 * This is a module that defines some rule operators for development purposes.
 *
 * @note This module can be disabled by configuring with the "--disable-devel"
 * option.
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/action.h>
#include <ironbee/bytestr.h>
#include <ironbee/capture.h>
#include <ironbee/context.h>
#include <ironbee/engine.h>
#include <ironbee/field.h>
#include <ironbee/list.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        testops
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/* IsType operators */
typedef enum {
    IsTypeByteStr,
    IsTypeInt,
    IsTypeFloat,
} istype_t;

/* IsType operator data */
typedef struct {
    istype_t    istype;
    int         numtypes;
    ib_ftype_t  type;
} istype_params_t;
typedef istype_params_t istype_params_t;

/* IsType operators data */
static istype_params_t istype_params[] = {
    { IsTypeByteStr, 1, IB_FTYPE_BYTESTR },
    { IsTypeInt,     1, IB_FTYPE_NUM },
    { IsTypeFloat,   1, IB_FTYPE_FLOAT },
};

/**
 * Execute function for the "True" operator
 *
 * @param[in] tx Current transaction.
 * @param[in] instance_data Instance data needed for execution.
 * @param[in] field The field to operate on.
 * @param[in] capture If non-NULL, the collection to capture to.
 * @param[out] result The result of the operator 1=true 0=false.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t op_true_execute(
    ib_tx_t          *tx,
    void             *instance_data,
    const ib_field_t *field,
    ib_field_t       *capture,
    ib_num_t         *result,
    void             *cbdata
)
{
    /* Always return true */
    *result = 1;

    /* Set the capture */
    if (capture != NULL && *result) {
        ib_capture_clear(capture);
        ib_capture_set_item(capture, 0, tx->mp, field);
    }
    return IB_OK;
}

/**
 * Execute function for the "False" operator
 *
 * @param[in] tx Current transaction.
 * @param[in] instance_data Instance data needed for execution.
 * @param[in] field The field to operate on.
 * @param[in] capture If non-NULL, the collection to capture to.
 * @param[out] result The result of the operator 1=true 0=false.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t op_false_execute(
    ib_tx_t          *tx,
    void             *instance_data,
    const ib_field_t *field,
    ib_field_t       *capture,
    ib_num_t         *result,
    void             *cbdata
)
{
    *result = 0;
    /* Don't check for capture, because we always return zero */

    return IB_OK;
}

/**
 * Execute function for the "Exists" operator
 *
 * @param[in] tx Current transaction.
 * @param[in] instance_data Instance data needed for execution.
 * @param[in] field The field to operate on.
 * @param[in] capture If non-NULL, the collection to capture to.
 * @param[out] result The result of the operator 1=true 0=false.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t op_exists_execute(
    ib_tx_t          *tx,
    void             *instance_data,
    const ib_field_t *field,
    ib_field_t       *capture,
    ib_num_t         *result,
    void             *cbdata
)
{
    /* Return true of field is not NULL */
    *result = (field != NULL);

    /* Set the capture */
    if (capture != NULL && *result) {
        ib_capture_clear(capture);
        ib_capture_set_item(capture, 0, tx->mp, field);
    }

    return IB_OK;
}

/**
 * Execute function for the "IsType" operator family
 *
 * @param[in] tx Current transaction.
 * @param[in] instance_data Instance data needed for execution.
 * @param[in] field The field to operate on.
 * @param[in] capture If non-NULL, the collection to capture to.
 * @param[out] result The result of the operator 1=true 0=false.
 * @param[in] cbdata Callback data (istype_params_t pointer).
 *
 * @returns Status code
 */
static ib_status_t op_istype_execute(
    ib_tx_t          *tx,
    void             *instance_data,
    const ib_field_t *field,
    ib_field_t       *capture,
    ib_num_t         *result,
    void             *cbdata
)
{
    assert(field != NULL);
    assert(cbdata != NULL);
    const istype_params_t *params = cbdata;

    *result = (params->type == field->type);

    return IB_OK;
}

/**
 * Initialize the testops module.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t testops_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata)
{
    ib_status_t rc;

    /**
     * Simple True / False operators.
     */

    /* Register the True operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "True",
        ( IB_OP_CAPABILITY_ALLOW_NULL |
          IB_OP_CAPABILITY_NON_STREAM |
              IB_OP_CAPABILITY_STREAM |
        IB_OP_CAPABILITY_CAPTURE ),
        NULL, NULL, /* No create function */
        NULL, NULL, /* no destroy function */
        op_true_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the False operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "False",
        ( IB_OP_CAPABILITY_ALLOW_NULL |
          IB_OP_CAPABILITY_NON_STREAM |
              IB_OP_CAPABILITY_STREAM ),
        NULL, NULL, /* No create function */
        NULL, NULL, /* no destroy function */
        op_false_execute, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the field exists operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "Exists",
        ( IB_OP_CAPABILITY_ALLOW_NULL |
          IB_OP_CAPABILITY_NON_STREAM |
          IB_OP_CAPABILITY_CAPTURE ),
        NULL, NULL, /* No create function */
        NULL, NULL, /* no destroy function */
        op_exists_execute, NULL
    );
    if (rc != IB_OK) {
        return rc;
    }

    /**
     * IsType operators
     */

    /* Register the IsByteStr operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "IsByteStr",
        ( IB_OP_CAPABILITY_NON_STREAM |
          IB_OP_CAPABILITY_STREAM ),
        NULL, NULL, /* no create function */
        NULL, NULL, /* no destroy function */
        op_istype_execute, &istype_params[IsTypeByteStr]
    );
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the IsInt operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "IsInt",
        ( IB_OP_CAPABILITY_NON_STREAM |
          IB_OP_CAPABILITY_STREAM ),
        NULL, NULL, /* no create function */
        NULL, NULL, /* no destroy function */
        op_istype_execute, &istype_params[IsTypeInt]
    );
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the IsFloat operator */
    rc = ib_operator_create_and_register(
        NULL,
        ib,
        "IsFloat",
        ( IB_OP_CAPABILITY_NON_STREAM |
          IB_OP_CAPABILITY_STREAM ),
        NULL, NULL, /* no create function */
        NULL, NULL, /* no destroy function */
        op_istype_execute, &istype_params[IsTypeFloat]
    );
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
    IB_MODULE_CONFIG_NULL,                   /* Global config data */
    NULL,                                    /* Module config map */
    NULL,                                    /* Module directive map */
    testops_init,                            /* Initialize function */
    NULL,                                    /* Callback data */
    NULL,                                    /* Finish function */
    NULL,                                    /* Callback data */
);
