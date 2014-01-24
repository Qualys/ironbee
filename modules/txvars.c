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
 * This module is used to add various items to a transaction's vars.
 *
 * The following vars are added for each transaction (when available):
 *  - <tt>engine_id</tt>: The IronBee engine's instance ID
 *  - <tt>sensor_id</tt>: The IronBee engine's Sensor ID
 *  - <tt>conn_id</tt>: The connection's ID
 *  - <tt>conn_start</tt>: The connection start time
 *  - <tt>tx_id</tt>: The transaction's ID
 *  - <tt>tx_start</tt>: The transaction's start time
 *  - <tt>ctx_name</tt>: The name of the context chosen for the transaction
 *  - <tt>site_id</tt>: The context's site ID
 *  - <tt>site_name</tt>: The context's site name
 *  - <tt>location_path</tt>: The context's location path
 *
 * Sample values published into vars:
 *  - <tt>conn_id = "e68a8286-f012-49ae-b607-5ed98e8ab46f"</tt>
 *  - <tt>conn_start = 2014-01-24T11:22:40.0221-0600</tt>
 *  - <tt>context_name = "Validation:location:/"</tt>
 *  - <tt>engine_id = "8e08a33e-6321-49ca-bc7f-7a875c9818a5"</tt>
 *  - <tt>location_path = "/"</tt>
 *  - <tt>sensor_id = "AAAABBBB-1111-2222-3333-FFFF00000023"</tt>
 *  - <tt>site_id = "AAAABBBB-1111-2222-3333-000000006661"</tt>
 *  - <tt>site_name = "Validation"</tt>
 *  - <tt>tx_id = "4074d870-a93e-4f24-a9c2-09210a8230c0"</tt>
 *  - <tt>tx_start = 2014-01-24T11:22:40.0223-0600</tt> 
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
#include <ironbee/site.h>
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
 * Which TxVar?
 */
typedef enum {
    TXVAR_ENGINE_ID = 0,       /**< Engine instance ID */
    TXVAR_SENSOR_ID,           /**< Sensor ID */
    TXVAR_CONN_ID,             /**< Connection ID */
    TXVAR_CONN_START,          /**< Connection start time */
    TXVAR_TX_ID,               /**< Transaction ID */
    TXVAR_TX_START,            /**< Transaction start time */
    TXVAR_CTX_NAME,            /**< Context name */
    TXVAR_SITE_ID,             /**< Site ID */
    TXVAR_SITE_NAME,           /**< Site name */
    TXVAR_LOCATION_PATH,       /**< Location's path */
    TXVAR_NONE,                /**< None, used for end of list */
    TXVAR_COUNT,               /**< Count of TxVars, for array sizes */
} txvars_which_t;

/**
 * TxVars item initializer
 */
struct txvars_item_init_t {
    txvars_which_t  which;           /**< Which item are we initializing? */
    ib_ftype_t      type;            /**< Value source type */
    const char     *name;            /**< Name of the field / var */
    bool            register_src;    /**< Register this var source? */
};
typedef struct txvars_item_init_t txvars_item_init_t;

/**
 * TxVars Initializer array
 */
static const txvars_item_init_t txvars_init_table[TXVAR_COUNT] = {
    {
        .which = TXVAR_ENGINE_ID,
        .type = IB_FTYPE_NULSTR,
        .name = "engine_id",
        .register_src = true,
    },
    {
        .which = TXVAR_SENSOR_ID,
        .type = IB_FTYPE_NULSTR,
        .name = "sensor_id",
        .register_src = true,
    },
    {
        .which = TXVAR_CONN_ID,
        .type = IB_FTYPE_NULSTR,
        .name = "conn_id",
        .register_src = false,
    },
    {
        .which = TXVAR_CONN_START,
        .type = IB_FTYPE_TIME,
        .name = "conn_start",
        .register_src = false,
    },
    {
        .which = TXVAR_TX_ID,
        .type = IB_FTYPE_NULSTR,
        .name = "tx_id",
        .register_src = false,
    },
    {
        .which = TXVAR_TX_START,
        .type = IB_FTYPE_TIME,
        .name = "tx_start",
        .register_src = false,
    },
    {
        .which = TXVAR_CTX_NAME,
        .type = IB_FTYPE_NULSTR,
        .name = "context_name",
        .register_src = false,
    },
    {
        .which = TXVAR_SITE_ID,
        .type = IB_FTYPE_NULSTR,
        .name = "site_id",
        .register_src = false,
    },
    {
        .which = TXVAR_SITE_NAME,
        .type = IB_FTYPE_NULSTR,
        .name = "site_name",
        .register_src = false,
    },
    {
        .which = TXVAR_LOCATION_PATH,
        .type = IB_FTYPE_NULSTR,
        .name = "location_path",
        .register_src = false,
    },
    {
        .which = TXVAR_NONE,
        .type = IB_FTYPE_GENERIC,
        .name = NULL,
        .register_src = false,
    },
};

/**
 * TxVars item data
 */
struct txvars_item_t {
    const txvars_item_init_t *init;     /**< The associate initializer */
    ib_var_source_t          *source;   /**< The vars source */
};
typedef struct txvars_item_t txvars_item_t;

/**
 * TxVars module data
 */
struct txvars_module_data_t {
    txvars_item_t *items[TXVAR_COUNT]; /**< Items to add to vars */
    ib_time_t      base_time;          /**< Base time for relative times */
};
typedef struct txvars_module_data_t txvars_module_data_t;

/**
 * TxVars configuration
 */
struct txvars_config_t {
    bool enabled;                      /**< TxVars enabled? */
};
typedef struct txvars_config_t txvars_config_t;

/**
 * TxVars global configuration
 */
static txvars_config_t txvars_config = {
    .enabled = false,
};

/**
 * Store a var string item into TX vars
 *
 * @param[in] tx Transaction
 * @param[in] item TxVars item
 * @param[in] value Value string
 *
 */
static void store_var_str_item(
    ib_tx_t       *tx,
    txvars_item_t *item,
    const char    *value
)
{
    assert(tx != NULL);
    assert(item != NULL);

    ib_bytestr_t *bs;
    ib_field_t   *f;
    ib_status_t   rc;

    /* Create the byte string */
    rc = ib_bytestr_dup_nulstr(&bs, tx->mp, value);
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating bytestr for \"%s\" [\"%s\"]: %s",
                        item->init->name, value, ib_status_to_string(rc));
        return;
    }

    /* Create the field */
    rc = ib_field_create(&f, tx->mp,
                         IB_FIELD_NAME(item->init->name),
                         IB_FTYPE_BYTESTR,
                         ib_ftype_bytestr_in(bs));
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating field for \"%s\": %s",
                        item->init->name, ib_status_to_string(rc));
        return;
    }

    /* Point the var source at the field */
    rc = ib_var_source_set(item->source, tx->var_store, f);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to add field \"%s\" to TX var store.",
                        item->init->name);
        return;
    }
}

/**
 * Store a var time item into TX vars
 *
 * @param[in] tx Transaction
 * @param[in] item TxVars item
 * @param[in] tbase Base time
 * @param[in] tval Time value
 *
 */
static void store_var_time_item(
    ib_tx_t       *tx,
    txvars_item_t *item,
    ib_time_t      tbase,
    ib_time_t      tval
)
{
    assert(tx != NULL);
    assert(item != NULL);

    ib_field_t  *f;
    ib_status_t  rc;

    /* Add in the base time */
    tval += tbase;

    /* Create the field */
    rc = ib_field_create(&f, tx->mp,
                         IB_FIELD_NAME(item->init->name),
                         IB_FTYPE_TIME,
                         ib_ftype_time_in(&tval));
    if (rc != IB_OK) {
        ib_log_error_tx(tx,
                        "Error creating field for \"%s\": %s",
                        item->init->name, ib_status_to_string(rc));
        return;
    }

    /* Point the var source at the field */
    rc = ib_var_source_set(item->source, tx->var_store, f);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to add field \"%s\" to TX var store.",
                        item->init->name);
        return;
    }

}

/**
 * Handle tx context selected events to add headers.
 *
 * @param[in] ib IronBee object
 * @param[in] event Event type
 * @param[in,out] tx Transaction object
 * @param[in] cbdata Callback data (module)
 *
 * @returns Status code
 */
static ib_status_t handle_tx_context(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == handle_context_tx_event);
    assert(cbdata != NULL);

    const ib_module_t        *module = cbdata;
    txvars_module_data_t     *mod_data = module->data;
    txvars_config_t          *config;
    ib_status_t               rc;
    const ib_site_t          *site = NULL;
    const ib_site_location_t *location = NULL;
    size_t                    n;

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

    /* Get the context's site and location */
    ib_context_site_get(tx->ctx, &site);
    ib_context_location_get(tx->ctx, &location);

    /* Create the vars sources */
    for(n = 0; n < TXVAR_NONE; ++n) {
        txvars_item_t *item = mod_data->items[n];
        const char    *strval = NULL;
        ib_time_t      timeval;

        switch(item->init->which) {
        case TXVAR_ENGINE_ID:
            strval = ib_engine_instance_id(ib);
            break;
        case TXVAR_SENSOR_ID:
            strval = ib_engine_sensor_id(ib);
            break;
        case TXVAR_CONN_ID:
            strval = tx->conn->id;
            break;
        case TXVAR_CONN_START:
            timeval = tx->conn->t.started;
            break;
        case TXVAR_TX_ID:
            strval = tx->id;
            break;
        case TXVAR_TX_START:
            timeval = tx->t.started;
            break;
        case TXVAR_CTX_NAME:
            strval = ib_context_full_get(tx->ctx);
            break;
        case TXVAR_SITE_ID:
            if (site != NULL) {
                strval = site->id;
            }
            break;
        case TXVAR_SITE_NAME:
            if (site != NULL) {
                strval = site->name;
            }
            break;
        case TXVAR_LOCATION_PATH:
            if (location != NULL) {
                strval = location->path;
            }
            break;
        default:
            assert(0 && "Invalid TxVar source");
        }

        switch(item->init->type) {
        case IB_FTYPE_NULSTR:
            store_var_str_item(tx, item, strval);
            break;
        case IB_FTYPE_TIME:
            store_var_time_item(tx, item, timeval, mod_data->base_time);
            break;
        default:
            assert(0 && "Unsupported TxVar source type");
        }
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
    txvars_config_t   *config;

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
 * Create a TxVars item
 *
 * @param[in] ib IronBee Engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] init Item initializer data
 * @param[out] pitem Pointer to created item
 *
 * @returns Status code
 */
static ib_status_t create_txvar_item(
    ib_engine_t               *ib,
    ib_mpool_t                *mp,
    const txvars_item_init_t  *init,
    txvars_item_t            **pitem
)
{
    assert(ib != NULL);
    assert(mp != NULL);
    assert(init != NULL);
    assert(pitem != NULL);

    ib_status_t      rc;
    txvars_item_t   *item;
    ib_var_source_t *source;
    const char      *label;

    /* Allocate the item object */
    item = ib_mpool_alloc(mp, sizeof(*item));
    if (item == NULL) {
        return IB_EALLOC;
    }

    /* Point it at the initializer */
    item->init = init;

    /* Register or acquire the source */
    if (init->register_src) {
        label = "registering";
        rc = ib_var_source_register(&source,
                                    ib_engine_var_config_get(ib),
                                    IB_FIELD_NAME(init->name),
                                    IB_PHASE_NONE,
                                    IB_PHASE_NONE);
    }
    else {
        label = "acquiring";
        rc = ib_var_source_acquire(&source,
                                   mp,
                                   ib_engine_var_config_get(ib),
                                   IB_FIELD_NAME(init->name));
    }
    if (rc != IB_OK) {
        ib_log_debug(ib, "Error %s var source \"%s\": %s",
                     init->name, label, ib_status_to_string(rc));
        return rc;
    }
    item->source = source;

    /* Done */
    *pitem = item;
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

    ib_status_t               rc;
    ib_mpool_t               *mp;
    txvars_module_data_t     *mod_data;
    ib_timeval_t              tv;
    ib_time_t                 since_epoch;
    ib_time_t                 since_boot;
    const txvars_item_init_t *init;

    /* Get the engine's main memory pool */
    mp = ib_engine_pool_main_get(ib);
    assert(mp != NULL);

    /* Create the module data */
    mod_data = ib_mpool_calloc(mp, 1, sizeof(*mod_data));
    if (mod_data == NULL) {
        return IB_EALLOC;
    }

    /* Create the vars sources */
    for(init = txvars_init_table; init->which != TXVAR_NONE; ++init) {
        txvars_item_t *item;
        rc = create_txvar_item(ib, mp, init, &item);
        if (rc != IB_OK) {
            return rc;
        }
        mod_data->items[init->which] = item;
    }

    /* Calculate the base time. */
    since_boot = ib_clock_get_time();
    ib_clock_gettimeofday(&tv);
    since_epoch = IB_CLOCK_TIMEVAL_TIME(tv);
    mod_data->base_time = since_epoch - since_boot;

    /* Save off pointer into the module object's data pointer. */
    module->data = mod_data;

    /* Register the Txvars directive. */
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

    /* Register the TX context callback */
    rc = ib_hook_tx_register(ib,
                             handle_context_tx_event,
                             handle_tx_context,
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
