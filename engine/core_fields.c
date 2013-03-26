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
 * @brief IronBee - Core Module Fields
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include "ironbee_config_auto.h"

#include "core_private.h"

#include <ironbee/capture.h>
#include <ironbee/core.h>
#include <ironbee/engine.h>
#include <ironbee/field.h>
#include <ironbee/provider.h>
#include <ironbee/stream.h>

#include <assert.h>

/* -- Field Generation Routines -- */

/* Placeholder for as-of-yet-initialized bytestring fields. */
static const uint8_t core_placeholder_value[] = {
    '_', '_', 'c', 'o', 'r', 'e', '_', '_',
    'p', 'l', 'a', 'c', 'e', 'h', 'o', 'l',
    'd', 'e', 'r', '_', '_', 'v', 'a', 'l',
    'u', 'e', '_', '_',  0,   0,   0,   0
};

static const ib_tx_flag_map_t core_tx_flag_map[] = {
    {
        "suspicious",
        "FLAGS:suspicious",
        IB_TX_FSUSPICIOUS,
        false,
        false
    },
    {
        "inspectRequestHeader",
        "FLAGS:inspectRequestHeader",
        IB_TX_FINSPECT_REQHDR,
        false,
        true
    },
    {
        "inspectRequestBody",
        "FLAGS:inspectRequestBody",
        IB_TX_FINSPECT_REQBODY,
        false,
        false
    },
    {
        "inspectResponseHeader",
        "FLAGS:inspectResponseHeader",
        IB_TX_FINSPECT_RSPHDR,
        false,
        false
    },
    {
        "inspectResponseBody",
        "FLAGS:inspectResponseBody",
        IB_TX_FINSPECT_RSPBODY,
        false,
        false
    },

    /* End */
    { NULL, NULL, IB_TX_FNONE, true, false },
};

static ib_status_t core_field_placeholder_bytestr(ib_data_t *data,
                                                  const char *name)
{
    ib_status_t rc = ib_data_add_bytestr_ex(data,
                                            (const char *)name,
                                            strlen(name),
                                            (uint8_t *)core_placeholder_value,
                                            0,
                                            NULL);
    return rc;
}

static void core_gen_tx_bytestr_alias_field(ib_tx_t *tx,
                                            const char *name,
                                            ib_bytestr_t *val)
{
    ib_field_t *f;

    assert(tx != NULL);
    assert(name != NULL);
    assert(val != NULL);

    ib_status_t rc = ib_field_create_no_copy(&f, tx->mp,
                                             name, strlen(name),
                                             IB_FTYPE_BYTESTR,
                                             val);
    if (rc != IB_OK) {
        ib_log_warning(tx->ib, "Failed to create \"%s\" field: %s",
                       name, ib_status_to_string(rc));
        return;
    }

    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx,
            "Failed add \"%s\" field to transaction data store: %s",
            name, ib_status_to_string(rc)
        );
    }
}

static void core_gen_tx_numeric_field(ib_tx_t *tx,
                                      const char *name,
                                      ib_num_t val)
{
    ib_field_t *f;

    assert(tx != NULL);
    assert(name != NULL);

    ib_num_t num = val;
    ib_status_t rc = ib_field_create(&f, tx->mp,
                                     name, strlen(name),
                                     IB_FTYPE_NUM,
                                     &num);
    if (rc != IB_OK) {
        ib_log_warning(tx->ib, "Failed to create \"%s\" field: %s",
                       name, ib_status_to_string(rc));
        return;
    }

    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        ib_log_warning_tx(tx,
            "Failed add \"%s\" field to transaction data store: %s",
            name, ib_status_to_string(rc)
        );
    }
}

/* -- Hooks -- */

// FIXME: This needs to go away and be replaced with dynamic fields
static ib_status_t core_gen_placeholder_fields(ib_engine_t *ib,
                                               ib_tx_t *tx,
                                               ib_state_event_type_t event,
                                               void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->data != NULL);
    assert(event == tx_started_event);

    ib_status_t rc;
    ib_field_t *tmp;

    /* Core Request Fields */
    rc = core_field_placeholder_bytestr(tx->data, "request_line");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_method");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_protocol");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_raw");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_scheme");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_username");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_password");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_host");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_host");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_port");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_path");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_query");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_uri_fragment");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_content_type");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "request_filename");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "auth_type");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "auth_username");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "auth_password");
    if (rc != IB_OK) {
        return rc;
    }

    /* Core Request Collections */
    rc = ib_data_add_list(tx->data, "request_headers", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_list(tx->data, "request_cookies", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_list(tx->data, "request_uri_params", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_list(tx->data, "request_body_params", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* ARGS collection */
    rc = ib_data_get(tx->data, "ARGS", &tmp);
    if (rc == IB_ENOENT) {
        rc = ib_data_add_list(tx->data, "ARGS", NULL);
        if (rc != IB_OK) {
            return rc;
        }
    }
    else if (rc != IB_OK) {
        return rc;
    }

    /* Flags collection */
    rc = ib_data_get(tx->data, "FLAGS", &tmp);
    if (rc == IB_ENOENT) {
        rc = ib_data_add_list(tx->data, "FLAGS", NULL);
        if (rc != IB_OK) {
            return rc;
        }
    }
    else if (rc != IB_OK) {
        return rc;
    }

    /* Initialize CAPTURE */
    rc = ib_capture_clear(tx, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Core Response Fields */
    rc = core_field_placeholder_bytestr(tx->data, "response_line");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "response_protocol");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "response_status");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "response_message");
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "response_content_type");
    if (rc != IB_OK) {
        return rc;
    }

    /* Core Response Collections */
    rc = ib_data_add_list(tx->data, "response_headers", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = core_field_placeholder_bytestr(tx->data, "FIELD_NAME");
    if (rc != IB_OK) {
        return rc;
    }
    rc = core_field_placeholder_bytestr(tx->data, "FIELD_NAME_FULL");
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_list(tx->data, "response_cookies", NULL);

    return rc;
}

/*
 * Callback used to generate connection fields.
 */
static ib_status_t core_gen_connect_fields(ib_engine_t *ib,
                                           ib_state_event_type_t event,
                                           ib_conn_t *conn,
                                           void *cbdata)
{
    ib_status_t rc;

    assert(ib != NULL);
    assert(conn != NULL);
    assert(event == handle_connect_event);

    rc = ib_data_add_bytestr(conn->data,
                             "server_addr",
                             (uint8_t *)conn->local_ipstr,
                             strlen(conn->local_ipstr),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_num(conn->data,
                         "server_port",
                         conn->local_port,
                         NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_bytestr(conn->data,
                             "remote_addr",
                             (uint8_t *)conn->remote_ipstr,
                             strlen(conn->remote_ipstr),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_add_num(conn->data,
                         "remote_port",
                         conn->remote_port,
                         NULL);
    if (rc != IB_OK) {
        return rc;
    }


    return IB_OK;
}

static ib_status_t core_gen_flags_collection(ib_engine_t *ib,
                                             ib_tx_t *tx,
                                             ib_state_event_type_t event,
                                             void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->data != NULL);
    assert(event == tx_started_event);

    ib_status_t rc;
    const ib_tx_flag_map_t *flag;

    for (flag = ib_core_fields_tx_flags();  flag->name != NULL;  ++flag) {
        rc = ib_data_add_num(tx->data, flag->tx_name, (tx->flags & flag->tx_flag ? 1 : 0), NULL);
        if (rc != IB_OK) {
            return rc;
        }
    }

    ib_log_debug(ib, "FLAGS CREATED");
    return IB_OK;
}

/**
 * Create an alias list collection.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param name Collection name
 * @param header Header list to alias
 *
 * @returns Status code
 */
static ib_status_t create_header_alias_list(
    ib_engine_t *ib,
    ib_tx_t *tx,
    const char *name,
    ib_parsed_header_wrapper_t *header)
{
    ib_field_t *f;
    ib_list_t *header_list;
    ib_status_t rc;
    ib_parsed_name_value_pair_list_t *nvpair;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(name != NULL);
    assert(header != NULL);

    /* Create the list */
    rc = ib_data_get(tx->data, name, &f);
    if (rc == IB_ENOENT) {
        rc = ib_data_add_list(tx->data, name, &f);
        if (rc != IB_OK) {
            return rc;
        }
    }
    else if (rc != IB_OK) {
        return rc;
    }
    rc = ib_field_mutable_value(f, ib_ftype_list_mutable_out(&header_list));
    if (rc != IB_OK) {
        return rc;
    }

    /* Loop through the list & alias everything */
    for(nvpair = header->head;  nvpair != NULL;  nvpair = nvpair->next) {
        assert(nvpair);
        assert(nvpair->value);
        ib_bytestr_t *bs = NULL;
        if (ib_bytestr_ptr(nvpair->value) != NULL) {
            rc = ib_bytestr_alias_mem(
                &bs,
                tx->mp,
                ib_bytestr_ptr(nvpair->value),
                ib_bytestr_length(nvpair->value)
            );
        }
        else {
            rc = ib_bytestr_dup_mem(&bs, tx->mp, (const uint8_t *)"", 0);
        }
        if (rc != IB_OK) {
            ib_log_error_tx(
                tx,
                "Error creating bytestring of '%.*s' for %s: %s",
                (int)ib_bytestr_length(nvpair->name),
                (const char *)ib_bytestr_ptr(nvpair->name),
                name,
                ib_status_to_string(rc)
            );
            return rc;
        }

        /* Create a byte string field */
        rc = ib_field_create(
            &f,
            tx->mp,
            (const char *)ib_bytestr_const_ptr(nvpair->name),
            ib_bytestr_length(nvpair->name),
            IB_FTYPE_BYTESTR,
            ib_ftype_bytestr_in(bs)
        );
        if (rc != IB_OK) {
            ib_log_error_tx(tx,
                            "Error creating field of '%.*s' for %s: %s",
                            (int)ib_bytestr_length(nvpair->name),
                            (const char *)ib_bytestr_ptr(nvpair->name),
                            name,
                            ib_status_to_string(rc));
            return rc;
        }

        /* Add the field to the list */
        rc = ib_list_push(header_list, f);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Error adding alias of '%.*s' to %s list: %s",
                            (int)ib_bytestr_length(nvpair->name),
                            (const char *)ib_bytestr_ptr(nvpair->name),
                            name,
                            ib_status_to_string(rc));
            return rc;
        }
    }

    return IB_OK;
}

/*
 * Callback used to generate request header fields.
 */
static ib_status_t core_gen_request_header_fields(ib_engine_t *ib,
                                                  ib_tx_t *tx,
                                                  ib_state_event_type_t event,
                                                  void *cbdata)
{
    ib_field_t *f;
    ib_status_t rc;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == request_header_finished_event);

    /**
     * Alias connection remote and server addresses
     */

    rc = ib_data_get(tx->conn->data, "server_addr", &f);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_get(tx->conn->data, "server_port", &f);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_get(tx->conn->data, "remote_addr", &f);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_data_get(tx->conn->data, "remote_port", &f);
    if (rc != IB_OK) {
        return rc;
    }
    rc = ib_data_add(tx->data, f);
    if (rc != IB_OK) {
        return rc;
    }

    core_gen_tx_numeric_field(tx, "conn_tx_count",
                              tx->conn->tx_count);

    core_gen_tx_bytestr_alias_field(tx, "request_line",
                                    tx->request_line->raw);

    core_gen_tx_bytestr_alias_field(tx, "request_method",
                                    tx->request_line->method);

    core_gen_tx_bytestr_alias_field(tx, "request_uri_raw",
                                    tx->request_line->uri);

    core_gen_tx_bytestr_alias_field(tx, "request_protocol",
                                    tx->request_line->protocol);

    /* Populate the ARGS collection. */
    rc = ib_data_get(tx->data, "ARGS", &f);
    if (rc == IB_OK) {
        ib_field_t *param_list;

        /* Add request URI parameters to ARGS collection. */
        rc = ib_data_get(tx->data, "request_uri_params", &param_list);
        if (rc == IB_OK) {
            ib_list_t *field_list;
            ib_list_node_t *node = NULL;

            rc = ib_field_mutable_value(
                param_list,
                ib_ftype_list_mutable_out(&field_list)
            );
            if (rc != IB_OK) {
                return rc;
            }

            IB_LIST_LOOP(field_list, node) {
                ib_field_t *param = (ib_field_t *)ib_list_node_data(node);

                /* Add the field to the ARGS collection. */
                rc = ib_field_list_add(f, param);
                if (rc != IB_OK) {
                    ib_log_notice_tx(tx,
                                     "Failed to add parameter to "
                                     "ARGS collection: %s",
                                     ib_status_to_string(rc));
                }
            }
        }
    }

    /* Create the aliased request header list */
    if (tx->request_header != NULL) {
        rc = create_header_alias_list(ib,
                                      tx,
                                      "request_headers",
                                      tx->request_header);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/*
 * Callback used to generate request body fields.
 */
static ib_status_t core_gen_request_body_fields(ib_engine_t *ib,
                                                ib_tx_t *tx,
                                                ib_state_event_type_t event,
                                                void *cbdata)
{
    ib_field_t *f;
    ib_status_t rc;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == request_finished_event);

    /* Populate the ARGS collection. */
    rc = ib_data_get(tx->data, "ARGS", &f);
    if (rc == IB_OK) {
        ib_field_t *param_list;

        /* Add request body parameters to ARGS collection. */
        rc = ib_data_get(tx->data, "request_body_params", &param_list);
        if (rc == IB_OK) {
            ib_list_t *field_list;
            ib_list_node_t *node = NULL;

            rc = ib_field_mutable_value(
                param_list,
                ib_ftype_list_mutable_out(&field_list)
            );
            if (rc != IB_OK) {
                return rc;
            }

            IB_LIST_LOOP(field_list, node) {
                ib_field_t *param = (ib_field_t *)ib_list_node_data(node);

                /* Add the field to the ARGS collection. */
                rc = ib_field_list_add(f, param);
                if (rc != IB_OK) {
                    ib_log_notice_tx(tx,
                                     "Failed to add parameter to "
                                     "ARGS collection: %s",
                                     ib_status_to_string(rc));
                }
            }
        }
    }

    return IB_OK;
}

/*
 * Callback used to generate response header fields.
 */
static ib_status_t core_gen_response_header_fields(
    ib_engine_t           *ib,
    ib_tx_t               *tx,
    ib_state_event_type_t  event,
    void                  *cbdata
)
{
    ib_status_t rc;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == response_header_finished_event);

    if (tx->response_line != NULL) {
        core_gen_tx_bytestr_alias_field(tx, "response_line",
                                        tx->response_line->raw);

        core_gen_tx_bytestr_alias_field(tx, "response_protocol",
                                        tx->response_line->protocol);

        core_gen_tx_bytestr_alias_field(tx, "response_status",
                                        tx->response_line->status);

        core_gen_tx_bytestr_alias_field(tx, "response_message",
                                        tx->response_line->msg);
    }

    /* Create the aliased response header list */
    if (tx->response_header != NULL) {
        rc = create_header_alias_list(ib,
                                      tx,
                                      "response_headers",
                                      tx->response_header);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/*
 * Callback used to generate response body fields.
 */
static ib_status_t core_gen_response_body_fields(ib_engine_t *ib,
                                                 ib_tx_t *tx,
                                                 ib_state_event_type_t event,
                                                 void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(event == response_finished_event);

    return IB_OK;
}


/* -- Initialization Routines -- */

/* Initialize libhtp config object for the context. */
ib_status_t ib_core_fields_ctx_init(ib_engine_t *ib,
                                    ib_module_t *mod,
                                    ib_context_t *ctx,
                                    void *cbdata)
{
    ib_core_cfg_t *corecfg;
    ib_status_t rc;

    assert(ib != NULL);
    assert(mod != NULL);
    assert(ctx != NULL);

    /* Get the core context config. */
    rc = ib_context_module_config(ctx, mod, (void *)&corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib,
            "Failed to fetch core module context config: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }

    return IB_OK;
}

/* Initialize core field generation callbacks. */
ib_status_t ib_core_fields_init(ib_engine_t *ib,
                                ib_module_t *mod)
{
    assert(ib != NULL);
    assert(mod != NULL);


    ib_hook_conn_register(ib, handle_connect_event,
                          core_gen_connect_fields, NULL);

    ib_hook_tx_register(ib, tx_started_event,
                        core_gen_placeholder_fields, NULL);

    ib_hook_tx_register(ib, tx_started_event,
                        core_gen_flags_collection, NULL);

    ib_hook_tx_register(ib, request_header_finished_event,
                        core_gen_request_header_fields, NULL);

    ib_hook_tx_register(ib, request_finished_event,
                        core_gen_request_body_fields, NULL);

    ib_hook_tx_register(ib, response_header_finished_event,
                        core_gen_response_header_fields, NULL);

    ib_hook_tx_register(ib, response_finished_event,
                        core_gen_response_body_fields, NULL);

    return IB_OK;
}

/* Get the core TX flags */
const ib_tx_flag_map_t *ib_core_fields_tx_flags( )
{
    return core_tx_flag_map;
}
