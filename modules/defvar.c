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
 * @brief IronBee --- DefVar Module
 *
 * This module can be used to create fields in the TX Data.  These fields are
 * defined through the use of the DefVar configuration directive.
 *
 * Below is an example configuration snippet that uses the DefVar directive
 * to create number, unsigned number, NUL-terminated string, Byte-string and
 * List.  The named fields "Num1", "Num2", ... will be created for every
 * transaction processed by the engine.
 *
 * Examples:
 * - <tt>DefVar Num1      NUM      1</tt>
 * - <tt>DefVar Num2      NUM      5</tt>
 * - <tt>DefVar Float1    FLOAT    1</tt>
 * - <tt>DefVar Float2    FLOAT    5.5</tt>
 * - <tt>DefVar Str1      NULSTR   "abc"</tt>
 * - <tt>DefVar Str2      NULSTR   "ABC"</tt>
 * - <tt>DefVar BStr1     BYTESTR  "ABC"</tt>
 * - <tt>DefVar BStr2     BYTESTR  "DEF"</tt>
 * - <tt>DefVar List0     LIST</tt>
 * - <tt>DefVar List1     LIST:NUM 1 2 3 4 5</tt>
 * - <tt>DefVar List2     LIST:NULSTR a bc def foo</tt>
 * - <tt>DefVar List3     LIST</tt>
 * - <tt>DefVar List3:Lst LIST:NULSTR a bc def foo</tt>
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
#define MODULE_NAME        defvar
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * DefVar configuration
 */
struct defvar_config_t {
    ib_list_t   *field_list;  /**< List of field pointers */
    ib_mpool_t  *mp;          /**< Memory pool for allocations */
};
typedef struct defvar_config_t defvar_config_t;

/**
 * Global configuration data
 */
static defvar_config_t defvar_config = {
    .field_list = NULL,
    .mp = NULL
};

/**
 * @param[in] cp Configuration parser
 * @param[in] mp Memory pool to use for allocations
 * @param[in] str String to parse as a type name
 * @param[out] type Field type
 * @param[out] element_type If @a type is list, the type of the elements in
 *             the list, if specified, otherwise IB_FTYPE_GENERIC
 *
 * @return Status code
 */
static ib_status_t parse_type(
    ib_cfgparser_t *cp,
    ib_mpool_t     *mp,
    const char     *str,
    ib_ftype_t     *type,
    ib_ftype_t     *element_type)
{
    /* Parse the type name */
    if (strcasecmp(str, "NUM") == 0) {
        *type = (ib_ftype_t)IB_FTYPE_NUM;
    }
    else if (strcasecmp(str, "FLOAT") == 0) {
        *type = (ib_ftype_t)IB_FTYPE_FLOAT;
    }
    else if (strcasecmp(str, "NULSTR") == 0) {
        *type = (ib_ftype_t)IB_FTYPE_NULSTR;
    }
    else if (strcasecmp(str, "BYTESTR") == 0) {
        *type = (ib_ftype_t)IB_FTYPE_BYTESTR;
    }
    else if (strcasecmp(str, "LIST") == 0) {
        *type = (ib_ftype_t)IB_FTYPE_LIST;
        if (element_type != NULL) {
            *element_type = (ib_ftype_t)IB_FTYPE_GENERIC;
        }
    }
    else if (strncasecmp(str, "LIST:", 5) == 0) {
        *type = (ib_ftype_t)IB_FTYPE_LIST;
        if (element_type != NULL) {
            ib_status_t rc;
            rc = parse_type(cp, mp, str+5, element_type, NULL);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp, "Invalid type \"%s\".", str);
            }
        }
    }
    else {
        ib_cfg_log_error(cp, "Invalid type \"%s\".", str);
        return IB_EINVAL;
    }

    ib_cfg_log_debug2(cp, "Parsed type \"%s\" -> %d.", str, (int)(*type) );

    /* Done */
    return IB_OK;
}

/**
 * @param[in] cp Configuration parser
 * @param[in] mp Memory pool to use for allocations
 * @param[in] str String to parse as a type name
 * @param[in] type Field type
 * @param[in] name Field name
 * @param[out] pfield Field to create
 *
 * @return Status code
 */
static ib_status_t parse_value(
    ib_cfgparser_t  *cp,
    ib_mpool_t      *mp,
    const char      *str,
    ib_ftype_t       type,
    const char      *name,
    ib_field_t     **pfield)
{
    ib_status_t rc;

    /* Parse the type name */
    switch(type) {
    case IB_FTYPE_NUM :
    {
        ib_num_t val;
        rc = ib_string_to_num(str, 0, &val);
        if (rc != IB_OK) {
            return rc;
        }
        rc = ib_field_create(pfield,
                             mp,
                             IB_FIELD_NAME(name),
                             type,
                             ib_ftype_num_in(&val));
        break;
    }

    case IB_FTYPE_FLOAT :
    {
        ib_float_t val;
        rc = ib_string_to_float(str, &val);
        if (rc != IB_OK) {
            return rc;
        }
        rc = ib_field_create(pfield,
                             mp,
                             IB_FIELD_NAME(name),
                             type,
                             ib_ftype_float_in(&val));
        break;
    }

    case IB_FTYPE_NULSTR :
    {
        rc = ib_field_create(pfield,
                             mp,
                             IB_FIELD_NAME(name),
                             type,
                             ib_ftype_nulstr_in(str));
        break;
    }

    case IB_FTYPE_BYTESTR :
    {
        ib_bytestr_t *bs;
        rc = ib_bytestr_dup_nulstr(&bs, mp, str);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp,
                             "Error creating bytestr for \"%s\": %s",
                             str, ib_status_to_string(rc));
            return rc;
        }
        rc = ib_field_create(pfield,
                             mp,
                             IB_FIELD_NAME(name),
                             type,
                             ib_ftype_bytestr_in(bs));
        break;
    }

    default :
        return IB_EINVAL;
    }

    /* Done */
    return rc;
}

/**
 * Handle request_header events to add fields.
 *
 * Adds fields to the transaction DPI.
 *
 * @param[in] ib IronBee object
 * @param[in] event Event type
 * @param[in,out] tx Transaction object
 * @param[in] cbdata Callback data (module object)
 *
 * @returns Status code
 */
static ib_status_t tx_header_finished(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == request_header_finished_event);
    assert(cbdata != NULL);

    const ib_module_t     *module = cbdata;
    const defvar_config_t *config;
    const ib_list_node_t  *node;
    ib_status_t            rc = IB_OK;
    ib_var_source_t       *source;

    /* Get module context configuration */
    rc = ib_context_module_config(tx->ctx, module, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Failed to get %s module configuration: %s",
                        module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Do nothing if list is NULL */
    if (config->field_list == NULL) {
        return IB_OK;
    }

    /* Loop through the list */
    IB_LIST_LOOP_CONST(config->field_list, node) {
        const ib_field_t *field = (const ib_field_t *)node->data;
        ib_field_t *newf;

        if (field->type == IB_FTYPE_BYTESTR) {
            const ib_bytestr_t *bs;
            rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
            if (rc != IB_OK) {
                ib_log_error_tx(tx, "Error retrieving field value: %s",
                                ib_status_to_string(rc));
                continue;
            }
        }

        rc = ib_field_copy(&newf, tx->mp, field->name, field->nlen, field);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error copying field: %s",
                            ib_status_to_string(rc));
            continue;
        }
        rc = ib_var_source_acquire(
            &source,
            tx->mp,
            ib_engine_var_config_get(ib),
            field->name, field->nlen
        );
        if (rc != IB_OK) {
            ib_log_debug_tx(tx, "Error acquiring source: %s",
                            ib_status_to_string(rc));
            continue;
        }
        rc = ib_var_source_set(source, tx->var_store, newf);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to add field \"%.*s\" to TX var store.",
                            (int)field->nlen, field->name);
        }
    }

    return rc;
}


/**
 * Parse a DefVar directive.
 *
 * @details Register a DefVar directive to the engine.
 * @param[in] cp Configuration parser
 * @param[in] directive The directive name.
 * @param[in] vars The list of variables passed to @a directive.
 * @param[in] cbdata User data (module object)
 */
static ib_status_t defvar_handler(
    ib_cfgparser_t  *cp,
    const char      *directive,
    const ib_list_t *vars,
    void            *cbdata)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(vars != NULL);
    assert(cbdata != NULL);

    ib_status_t rc;
    const ib_module_t    *module = cbdata;
    ib_mpool_t           *mp = cp->mp;
    ib_context_t         *context;
    defvar_config_t      *config;
    const ib_list_node_t *name_node;
    const ib_list_node_t *type_node;
    const ib_list_node_t *value_node;
    ib_field_t           *field = NULL;
    const char           *name_str;
    const char           *type_str;
    ib_ftype_t           type_num;
    ib_ftype_t           element_type = IB_FTYPE_GENERIC;
    ib_num_t             element_num;

    /* Get my configuration context */
    rc = ib_cfgparser_context_current(cp, &context);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s current context: %s",
                         module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Get module context configuration */
    rc = ib_context_module_config(context, module, (void *)&config);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to get %s module configuration: %s",
                         module->name, ib_status_to_string(rc));
        return rc;
    }

    /* Get the field name string */
    name_node = ib_list_first_const(vars);
    if ( (name_node == NULL) || (name_node->data == NULL) ) {
        ib_cfg_log_error(cp, "No name specified for field.");
        return IB_EINVAL;
    }
    name_str = (const char *)(name_node->data);

    /* Get type name string */
    type_node = ib_list_node_next_const(name_node);
    if ( (type_node == NULL) || (type_node->data == NULL) ) {
        ib_cfg_log_error(cp, "No type specified for field.");
        return IB_EINVAL;
    }
    type_str = (const char *)(type_node->data);

    /* Parse the type name */
    rc = parse_type(cp, mp, type_str, &type_num, &element_type);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp,
                         "Error parsing type string \"%s\": %s",
                         type_str, ib_status_to_string(rc));
        return rc;
    }

    /* Find the next value node */
    value_node = ib_list_node_next_const(type_node);

    /* Parse the value(s) */
    if (type_num == IB_FTYPE_LIST) {

        /* Check for errors */
        if (element_type == IB_FTYPE_LIST) {
            if (value_node != NULL) {
                ib_cfg_log_error(cp, "Value(s) not for LIST:LIST field.");
                return IB_EINVAL;
            }
        }
        else if (element_type == IB_FTYPE_GENERIC) {
            if (value_node != NULL) {
                ib_cfg_log_error(cp, "Values but no type for LIST field.");
                return IB_EINVAL;
            }
        }
        else {
            if (value_node == NULL) {
                ib_cfg_log_error(cp, "LIST type specified, but not values.");
                return IB_EINVAL;
            }
        }

        /* Create the field */
        rc = ib_field_create(&field,
                             mp,
                             IB_FIELD_NAME(name_str),
                             type_num,
                             NULL);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error creating field: %s",
                             ib_status_to_string(rc));
            return rc;
        }

        /* Parse the values */
        element_num = 1;
        while( (value_node != NULL) && (value_node->data != NULL) ) {
            ib_field_t *vfield;
            char buf[32];

            /* Create a field name: index */
            snprintf(buf, sizeof(buf), "%ld", (long)element_num );
            ++element_num;

            /* Parse the value and create a field to contain it */
            rc = parse_value(cp, mp, value_node->data,
                             element_type, buf, &vfield);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp, "Error parsing value \"%s\" type %s: %s",
                                 (char *)value_node->data,
                                 ib_field_type_name(element_type),
                                 ib_status_to_string(rc));
                return rc;
            }

            /* Add the field to the list */
            rc = ib_field_list_add(field, vfield);
            if (rc != IB_OK) {
                ib_cfg_log_error(cp, "Error pushing value on list: %s",
                                 ib_status_to_string(rc));
                return rc;
            }

            /* Next value */
            value_node = ib_list_node_next_const(value_node);
        }
    }
    else if ( (value_node != NULL) && (value_node->data != NULL) ) {
        /* Parse the value and create a field to contain it */
        rc = parse_value(cp, mp, value_node->data, type_num, name_str, &field);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error parsing value \"%s\": %s",
                             (char *)value_node->data, ib_status_to_string(rc));
            return rc;
        }
    }
    else {
        ib_cfg_log_error(cp, "No value specified for field \"%s\".", name_str);
        return rc;
    }

    /* Create the list if required */
    if (config->field_list == NULL) {
        rc = ib_list_create(&config->field_list, mp);
        if (rc != IB_OK) {
            ib_cfg_log_error(cp, "Error creating DefVar list: %s",
                             ib_status_to_string(rc));
        }
    }

    /* Add the field to the list */
    rc = ib_list_push(config->field_list, field);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Error pushing value on DefVar list: %s",
                         ib_status_to_string(rc));
        return rc;
    }

    /* Done */
    return IB_OK;
}

/**
 * Handle copying configuration data for the DefVar development sub-module
 *
 * @param[in] ib     Engine handle
 * @param[in] module Module
 * @param[in] dst    Destination of data.
 * @param[in] src    Source of data.
 * @param[in] length Length of data.
 * @param[in] cbdata Callback data
 *
 * @returns Status code
 */
static ib_status_t defvar_config_copy(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *dst,
    const void  *src,
    size_t       length,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);
    assert(dst != NULL);
    assert(src != NULL);
    assert(length == sizeof(defvar_config));

    ib_status_t            rc;
    defvar_config_t       *dst_config = dst;
    const defvar_config_t *src_config = src;
    ib_mpool_t            *mp = ib_engine_pool_main_get(ib);

    /* If there is no source list, do nothing */
    if (src_config->field_list == NULL) {
        dst_config->field_list = NULL;
        return IB_OK;
    }

    /* Otherwise, copy nodes from the source list */
    rc = ib_list_copy(src_config->field_list, mp, &dst_config->field_list);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Initialize the DefVar module.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
ib_status_t defvar_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata)
{
    assert(ib != NULL);
    assert(module != NULL);

    ib_status_t rc;

    /* Register the DefVar directive */
    rc = ib_config_register_directive(ib,
                                      "DefVar",
                                      IB_DIRTYPE_LIST,
                                      (ib_void_fn_t)defvar_handler,
                                      NULL,
                                      module,
                                      NULL,
                                      NULL);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register DefVar directive: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the TX header_finished callback */
    rc = ib_hook_tx_register(ib,
                             request_header_finished_event,
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
    IB_MODULE_HEADER_DEFAULTS,             /* Default metadata */
    MODULE_NAME_STR,                       /* Module name */
    &defvar_config, sizeof(defvar_config), /* Global config data */
    defvar_config_copy, NULL,              /* Configuration copy function */
    NULL,                                  /* Module config map */
    NULL,                                  /* Module directive map */
    defvar_init,                           /* Initialize function */
    NULL,                                  /* Callback data */
    NULL,                                  /* Finish function */
    NULL,                                  /* Callback data */
);
