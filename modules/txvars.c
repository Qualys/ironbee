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
 * @brief IronBee --- TxVars Module
 *
 * This module can be used to add add various IDs to the transaction vars.
 *
 * @note This module can be disabled by configuring with the "--disable-devel"
 * option.
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/bytestr.h>
#include <ironbee/cfgmap.h>
#include <ironbee/config.h>
#include <ironbee/context.h>
#include <ironbee/core.h>
#include <ironbee/engine_state.h>
#include <ironbee/field.h>
#include <ironbee/list.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        txvars
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/**
 * TxVars item data
 */
struct txvars_item_t {
    const char      *name;         /**< Name of the field / var */
    ib_var_source_t *var_source;   /**< Vars source data */
};
typedef struct txvars_item_t txvars_item_t;

/**
 * TxVars all items
 */
struct txvars_items_t {
    txvars_item_t  engine_id;       /**< Engine instance ID */
    txvars_item_t  sensor_id;       /**< Sensor ID */
    txvars_item_t  conn_id;         /**< Connection ID */
    txvars_item_t  tx_id;           /**< Transaction ID */
};
typedef struct txvars_items_t txvars_items_t;

/**
 * TxVars module data
 */
struct txvars_module_data_t {
    txvars_items_t all;             /**< Items to add to vars */
};
typedef struct txvars_module_data_t txvars_module_data_t;

/**
 * TxVars configuration
 */
struct txvars_config_t {
    bool  enabled;                 /**< TxVars enabled? */
};
typedef struct txvars_config_t txvars_config_t;

/**
 * TxVars global configuration
 */
static txvars_config_t txvars_config = {
    .enabled = false,
};

/**
 * Store an var item into TX vars
 *
 * @param[in] tx Transaction
 * @param[in] item TxVars item
 * @param[in] value Value string
 *
 * @returns Status code
 */
static ib_status_t store_var_item(
    ib_tx_t       *tx,
    txvars_item_t *item,
    const char    *value
)
{
    ib_bytestr_t *bs;
    ib_field_t   *f;
    ib_status_t   rc;

    /* Create the byte string */
    rc = ib_bytestr_dup_nulstr(&bs, tx->mp, value);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating bytestr for \"%s\" [\"%s\"]: %s",
                        item->name, value, ib_status_to_string(rc));
        return rc;
    }

    /* Create the field */
    rc = ib_field_create(&f, tx->mp,
                         IB_FIELD_NAME(item->name),
                         IB_FTYPE_BYTESTR,
                         ib_ftype_bytestr_in(bs));
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating field for \"%s\": %s",
                        item->name, ib_status_to_string(rc));
        return rc;
    }
    rc = ib_var_source_set(item->var_source, tx->var_store, f);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to add field \"%s\" to TX var store.",
                        item->name);
        return rc;
    }

    return IB_OK;
}

/**
 * Handle response_header events to add headers.
 *
 * @param[in] ib IronBee object
 * @param[in] event Event type
 * @param[in,out] tx Transaction object
 * @param[in] cbdata Callback data (module)
 *
 * @returns Status code
 */
static ib_status_t tx_header_finished(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == response_header_finished_event);
    assert(cbdata != NULL);

    const ib_module_t    *module = cbdata;
    txvars_module_data_t *mod_data = module->data;
    txvars_config_t      *config;
    ib_status_t           rc;
    txvars_item_t         *item;
    const char            *value;

    /* No module data? */
    assert(mod_data != NULL);

    /* Get my module configuration */
    rc = ib_context_module_config(tx->ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to get %s module configuration: %s",
                        module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Do nothing if not enabled */
    if (config->enabled == false) {
        return IB_OK;
    }

    /* Store the engine ID */
    item = &mod_data->all.engine_id;
    value = ib_engine_instance_id(ib);
    rc = store_var_item(tx, item, value);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx, "Error storing \"%s\" \"%s\" to tx vars: %s",
                          item->name, value, ib_status_to_string(rc));
    }

    /* Store the sensor ID */
    item = &mod_data->all.sensor_id;
    value = ib_engine_sensor_id(ib);
    rc = store_var_item(tx, item, value);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx, "Error storing \"%s\" \"%s\" to tx vars: %s",
                          item->name, value, ib_status_to_string(rc));
    }

    /* Store the connection ID */
    item = &mod_data->all.conn_id;
    value = tx->conn->id;
    rc = store_var_item(tx, item, value);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx, "Error storing \"%s\" \"%s\" to tx vars: %s",
                          item->name, value, ib_status_to_string(rc));
    }

    /* Store the transaction ID */
    item = &mod_data->all.tx_id;
    value = tx->id;
    rc = store_var_item(tx, item, value);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx, "Error storing \"%s\" \"%s\" to tx vars: %s",
                          item->name, value, ib_status_to_string(rc));
    }

    return IB_OK;
}

/**
 * Handle TxVars directives.
 *
 * @param[in] cp Config parser
 * @param[in] directive Directive name
 * @param[in] value On/Off value
 * @param[in] cbdata User data (ib_module_t)
 *
 * @returns Status code
 */
static ib_status_t txvars_handler(
    ib_cfgparser_t  *cp,
    const char      *directive,
    int              value,
    void            *cbdata)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(cbdata != NULL);

    ib_status_t        rc;
    const ib_module_t *module = cbdata;
    ib_context_t      *context;
    txvars_config_t    *config;

    /* Get my configuration context */
    rc = ib_cfgparser_context_current(cp, &context);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Txvars: Failed to get current context: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Get my module configuration */
    rc = ib_context_module_config(context, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Update the enable */
    config->enabled = (value != 0);

    /* Done */
    return IB_OK;
}

/**
 * Initialize an TxVars item
 *
 * @param[in] ib IronBee Engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] name Vars / field name
 * @param[in] item txvars_item_t to initialize
 *
 * @returns Status code
 */
static ib_status_t init_item(
    ib_engine_t  *ib,
    ib_mpool_t   *mp,
    const char   *name,
    txvars_item_t *item
)
{
    assert(ib != NULL);
    assert(mp != NULL);
    assert(name != NULL);
    assert(item != NULL);

    ib_status_t rc;

    /* Fill in the item name */
    item->name = ib_mpool_strdup(mp, name);
    if (item->name == NULL) {
        return IB_EALLOC;
    }

    /* Acquire the var source */
    rc = ib_var_source_acquire(&item->var_source,
                               mp,
                               ib_engine_var_config_get(ib),
                               IB_FIELD_NAME(name));
    if (rc != IB_OK) {
        ib_log_debug(ib, "Error acquiring source \"%s\": %s",
                     name, ib_status_to_string(rc));
        return rc;
    }

    /* Done */
    return IB_OK;
}

/**
 * Initialize the txvars module.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t txvars_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);

    ib_status_t          rc;
    ib_mpool_t          *mp;
    txvars_module_data_t *mod_data;

    /* Get the engine's main memory pool */
    mp = ib_engine_pool_main_get(ib);
    assert(mp != NULL);

    /* Create the module data */
    mod_data = ib_mpool_alloc(mp, sizeof(*mod_data));
    if (mod_data == NULL) {
        return IB_EALLOC;
    }

    /* Create the vars sources */
    rc = init_item(ib, mp, "ENGINE-ID", &mod_data->all.engine_id);
    if (rc != IB_OK) {
        return rc; 
    }
    rc = init_item(ib, mp, "SENSOR-ID", &mod_data->all.sensor_id);
    if (rc != IB_OK) {
        return rc; 
    }
    rc = init_item(ib, mp, "CONN-ID", &mod_data->all.conn_id);
    if (rc != IB_OK) {
        return rc; 
    }
    rc = init_item(ib, mp, "TX-ID", &mod_data->all.tx_id);
    if (rc != IB_OK) {
        return rc; 
    }
    module->data = mod_data;

    /* Register the Txvars directive */
    rc = ib_config_register_directive(ib,
                                      "TxVars",
                                      IB_DIRTYPE_ONOFF,
                                      (ib_void_fn_t)txvars_handler,
                                      NULL,
                                      module,
                                      NULL,
                                      NULL);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register Txvars directive: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the TX header_finished callback */
    rc = ib_hook_tx_register(ib,
                             response_header_finished_event,
                             tx_header_finished,
                             module);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error registering hook: %s", ib_status_to_string(rc));
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
    IB_MODULE_CONFIG(&txvars_config),        /* Global config data */
    NULL,                                    /* Module config map */
    NULL,                                    /* Module directive map */
    txvars_init,                             /* Initialize function */
    NULL,                                    /* Callback data */
    NULL,                                    /* Finish function */
    NULL,                                    /* Callback data */
);
