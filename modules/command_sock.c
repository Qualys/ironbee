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
 * @brief IronBee --- Command socket module
 *
 * This module defines create a command socket to control IronBee
 * from an external process.
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/context.h>
#include <ironbee/core.h>
#include <ironbee/ctrl_mod.h>
#include <ironbee/engine_state.h>
#include <ironbee/log.h>
#include <ironbee/module.h>

#include <assert.h>
#include <errno.h>
#include <memory.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        command_sock
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Module data
 */
struct module_data_t {
    const ib_engine_t    *ib;               /**< The IronBee engine */
    ib_ctrl_mod_handle_t  handle;           /**< Controller module handle */
    const char           *base_path;        /**< Server command socket path */
    const char           *server_path;      /**< Server command socket path */
    const char           *client_path;      /**< Client command socket path */
    int                   server_sock;      /**< Command socket file descriptor */
    struct sigaction      sa_orig;          /**< Original sig action for SIGIO */
};
typedef struct module_data_t module_data_t;

/**
 * Data for the SIGIO handler
 */
struct sigio_data_t {
    module_data_t     *mod_data;   /**< Pointer to module data */
};
typedef struct sigio_data_t sigio_data_t;

/*
 * Global data for sigio handler
 */
static sigio_data_t sigio_data = {
    .mod_data = NULL,
};

/**
 * Command socket responses
 */
typedef enum {
    RESPONSE_OK,            /**< Command processed, all ok */
    RESPONSE_OK_DATA,       /**< Command processed, all ok, with data */
    RESPONSE_FAILED,        /**< Command processed, failed */
    RESPONSE_INVALID,       /**< Error parsing command */
} response_code_t;

/**
 * Command socket configuration directives
 */
static const char *base_path_directive = "CmdSockBasePath";

/**
 * Max sizes, etc.
 */
static const size_t MAX_MESSAGE = 128;
static const size_t MAX_RESPONSE_DATA = 64;

/* Prototypes */
static void sigio_handler(int sig);

/**
 * Start the command socket logic
 *
 * @param[in] mod_data Command Sock module data 
 *
 * @returns Status code
 */
ib_status_t command_sock_start(
    module_data_t *mod_data
)
{
    assert(mod_data != NULL);
    assert(mod_data->ib != NULL);

    struct sockaddr_un addr;
    int                sock;
    int                rc;
    struct sigaction   sa_new;

    /* Do nothing if NULL path */
    if (mod_data->server_path == NULL) {
        ib_log_warning(mod_data->ib, "Command socket: socket path not set");
        return IB_DECLINED;
    }

    /* Create the socket */
    unlink(mod_data->server_path);
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        ib_log_error(mod_data->ib, "Error creating command socket: %d\n", errno);
        return IB_EOTHER;
    }

    /* Install the async handler */
    sa_new.sa_handler = sigio_handler;
    sigemptyset (&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGIO, &sa_new, &mod_data->sa_orig);

    /* Make it non-blocking & asynch */
    //flags = fcntl(sock, F_GETFL, 0);
    rc = fcntl(sock, F_SETFL, /*flags |*/ O_NONBLOCK | O_ASYNC | FASYNC);
    if (rc < 0) {
        ib_log_error(mod_data->ib,
                     "Error setting FASYNC for command socket: %d\n", errno);
        return IB_EOTHER;
    }

    /* Allow the process to receive SIGIO */
    fcntl(sock, F_SETOWN, getpid());

    /* Bind the socket to the UNIX path */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mod_data->server_path, sizeof(addr.sun_path)-1);
    rc = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0) {
        ib_log_error(mod_data->ib,
                     "Error binding command socket to \"%s\": %d\n",
                     mod_data->server_path, errno);
        return IB_EOTHER;
    }
    mod_data->server_sock = sock;
    ib_log_debug(mod_data->ib, "Command socket ready");
    return IB_OK;
}

/**
 * Stop the command socket logic
 *
 * @param[in] mod_data Command Sock module data 
 *
 * @returns Status code
 */
static ib_status_t command_sock_stop(
    module_data_t *mod_data
)
{
    assert(mod_data != NULL);

    if (mod_data->server_sock > 0) {
        close(mod_data->server_sock);
        mod_data->server_sock = -1;
    }
    return IB_OK;
}

/**
 * Cleanup the command socket module
 *
 * @param[in] mod_data Module data
 *
 */
static void command_sock_cleanup(
    module_data_t *mod_data
)
{
    assert(mod_data != NULL);

    command_sock_stop(mod_data);
    if (mod_data->server_path != NULL) {
        free((char *)mod_data->server_path);
        mod_data->server_path = NULL;
    }
    sigaction(SIGIO, &mod_data->sa_orig, NULL);
}

/**
 * Send response message
 *
 * @param[in] mod_data Module data object
 * @param[in] client_sock Client socket file handle
 * @param[in] status Status code
 * @param[in] rsp_code Response code
 * @param[in] rsp_data Response data
 *
 * @returns Response message sent
 */
static const char *sock_send(
    module_data_t   *mod_data,
    int              client_sock,
    ib_status_t      status,
    response_code_t  rsp_code,
    const char      *rsp_data
)
{
    assert(mod_data != NULL);
    assert(client_sock > 0);

    ib_status_t  rc = IB_OK;
    const char  *rsp_str = NULL;
    char         rsp_buf[MAX_MESSAGE+1];

    /* Force failed if something went wrong */
    if (status != IB_OK) {
        rsp_code = RESPONSE_FAILED;
    }

    /* Generate the response message */
    switch(rsp_code) {
    case RESPONSE_OK:
        rsp_str = "OK";
        break;
    case RESPONSE_OK_DATA:
        snprintf(rsp_buf, MAX_MESSAGE, "OK/DATA:%s", rsp_data);
        rsp_str = rsp_buf;
        break;
    case RESPONSE_FAILED:
        if (rc == IB_OK) {
            snprintf(rsp_buf, MAX_MESSAGE, "FAILED/DATA:%s", rsp_data);
        }
        else {
            snprintf(rsp_buf, MAX_MESSAGE, "FAILED/DATA:%d:%s",
                     rc, ib_status_to_string(rc));
        }
        rsp_str = rsp_buf;
        break;
    case RESPONSE_INVALID:
        rsp_str = "INVALID";
        break;
    }

    /* Send it to the client */
    if (rsp_str != NULL) {
        size_t bytes = write(client_sock, rsp_str, strlen(rsp_str));
        if (bytes != strlen(rsp_str)) {
            ib_log_error(mod_data->ib,
                         "Failed to send response \"%s\": %d (%s)",
                         rsp_str, errno, strerror(errno));
        }
    }
    return rsp_str;
}

/**
 * Perform message I/O
 *
 * Attempts to read the command socket, and, if it exists, parse
 * the command in the file and execute it.  See documentation at the
 * top of this file for a list of the commands.
 *
 * @param[in] mod_data Module data object
 * @param[in] server_sock Server socket file handle
 * @param[in] client_sock Client socket file handle
 *
 * @returns Status code
 */
static ib_status_t sock_io(
    module_data_t *mod_data,
    int            server_sock,
    int            client_sock
)
{
    assert(mod_data != NULL);
    assert(server_sock > 0);
    assert(client_sock > 0);

    const ib_engine_t *ib = mod_data->ib;
    ib_status_t        rc = IB_OK;
    char               message_buf[MAX_MESSAGE+1];
    response_code_t    rsp_code = RESPONSE_OK;
    char               rsp_data[MAX_RESPONSE_DATA+1];
    const char        *rsp = NULL;
    size_t             bytes;

    /* Read the message */
    bytes = read(server_sock, message_buf, MAX_MESSAGE);
    if (bytes == 0) {
        ib_log_error(ib, "No data from client");
        return IB_EUNKNOWN;
    }

    message_buf[bytes] = '\0';
    rsp_data[0] = '\0';
    ib_log_trace(ib, "command \"%s\"", message_buf);

    /* Disable command? */
    if (strncasecmp(message_buf, "DISABLE", 8) == 0) {
        rc = ib_ctrl_mod_disable_engine(mod_data->handle);
    }

    /* Cleanup command? */
    else if (strncasecmp(message_buf, "CLEANUP", 7) == 0) {
        rc = ib_ctrl_mod_cleanup_engines(mod_data->handle);
    }

    /* "Engine Count" command? */
    else if (strncasecmp(message_buf, "COUNT", 5) == 0) {
        size_t count;
        rc = ib_ctrl_mod_engine_count(mod_data->handle, &count);
        if (rc == IB_OK) {
            snprintf(rsp_data, MAX_RESPONSE_DATA, "%zd", count);
            rsp_code = RESPONSE_OK_DATA;
        }
    }

    /* "Current Engine" command? */
    else if (strncasecmp(message_buf, "CURRENT", 7) == 0) {
        ib_engine_t *engine;
        rc = ib_ctrl_mod_current_engine(mod_data->handle, &engine);
        if (rc == IB_OK) {
            if (engine == NULL) {
                snprintf(rsp_data, MAX_RESPONSE_DATA, "%p", engine);
            }
            else {
                snprintf(rsp_data, MAX_RESPONSE_DATA, "%p [%s]",
                         engine, ib_engine_instance_id(engine));
            }
            rsp_code = RESPONSE_OK_DATA;
        }
    }

    /* "Destroy manager" command? */
    else if (strncasecmp(message_buf, "DESTROY", 7) == 0) {
        rc = ib_ctrl_mod_shutdown(mod_data->handle, false);
    }

    /* "Soft shutdown" command? */
    else if (strncasecmp(message_buf, "SHUTDOWN", 8) == 0) {
        bool do_count;
        bool do_shutdown;
        bool do_exit;

        if (strncasecmp(message_buf+7, "/SOFT", 5) == 0) {
            do_count = true;
            do_shutdown = true;
            do_exit = true;
        }
        else if (strncasecmp(message_buf+7, "/HARD", 5) == 0) {
            do_count = false;
            do_shutdown = true;
            do_exit = true;
        }
        else if (strncasecmp(message_buf+7, "/FAST", 5) == 0) {
            do_count = false;
            do_shutdown = false;
            do_exit = true;
        }
        else {
            rsp_code = RESPONSE_INVALID;
            goto send_response;
        }

        /* Do the counting, cancel the shutdown if count is non-zero */
        if (do_count) {
            size_t count = 0;
            rc = ib_ctrl_mod_engine_count(mod_data->handle, &count);
            if ( (rc != IB_OK) || (count != 0) ) {
                rc = IB_DECLINED;
                goto send_response;
            }
        }
            
        /* Send the response first */
        rsp = sock_send(mod_data, client_sock, IB_OK, RESPONSE_OK, NULL);
        if (rsp == NULL) {
            rc = IB_EOTHER;
            goto done;
        }

        /* Try to shut down */
        if (do_shutdown) {
            rc = ib_ctrl_mod_shutdown(mod_data->handle, true);
        }

        /* Exit the server */
        if ( (rc == IB_OK) && (do_exit) ) {
            rc = ib_ctrl_mod_exit(mod_data->handle);
        }
    }

    /* "Flush log" command? */
    else if (strncasecmp(message_buf, "FLUSH-LOGS", 9) == 0) {
        rc = ib_ctrl_mod_flush_logs(mod_data->handle);
    }

    /* NOP command? */
    else if (strncasecmp(message_buf, "NOP", 3) == 0) {
        rsp_code = RESPONSE_OK;
    }

    else {
        rsp_code = RESPONSE_INVALID;
        ib_log_error(mod_data->ib, "Unknown command \"%s\"", message_buf);
    }

    /* Send the response */
send_response:
    rsp = sock_send(mod_data, client_sock, rc, rsp_code, rsp_data);

done:
    ib_log_debug2(ib, "COMMAND SOCK: IN=\"%s\" OUT=\"%s\"",
                  message_buf, (rsp != NULL) ? rsp : "");

    return rc;
}

/**
 * Handle sigio
 *
 * Attempts to read the command socket, and, if it exists, parse
 * the command in the file and execute it.  See documentation at the
 * top of this file for a list of the commands.
 *
 * @param[in] mod_data Module data object
 */
static void sigio_handler(int sig)
{
    assert(sig == SIGIO);
    ib_status_t         rc = IB_OK;
    module_data_t      *mod_data = sigio_data.mod_data;
    int                 client_sock = -1;
    int                 conn_rc;
    struct sockaddr_un  addr;

    assert(mod_data != NULL);
    assert(mod_data->ib != NULL);

    /* Do nothing if our main socket doesn't exist */
    if (mod_data->server_sock <= 0) {
        ib_log_error(mod_data->ib, "No server socket");
        goto cleanup;
    }

    /* Create the client socket */
    client_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (client_sock < 0) {
        ib_log_error(mod_data->ib,
                     "Error creating client socket: %d", errno);
        goto cleanup;
    }

    /* Connect the socket to the UNIX path */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mod_data->client_path, sizeof(addr.sun_path)-1);
    conn_rc = connect(client_sock, (struct sockaddr *) &addr, sizeof(addr));
    if (conn_rc < 0) {
        ib_log_error(mod_data->ib,
                     "Error connecting client socket to \"%s\": %d %s",
                     mod_data->client_path, errno, strerror(errno));
        goto cleanup;
    }

    /* Read the command, parse it, send response. */
    rc = sock_io(mod_data, mod_data->server_sock, client_sock);
    if (rc != IB_OK) {
        ib_log_error(mod_data->ib,
                     "I/O or processing error: %s", ib_status_to_string(rc));
    }

    /* Clean up */
cleanup:
    if (client_sock >= 0) {
        close(client_sock);
        client_sock = -1;
    }
}

/**
 * Handle Command sock base path directive.
 *
 * @param[in] cp Config parser
 * @param[in] directive Directive name
 * @param[in] value Path value
 * @param[in] cbdata User data (ib_module_t)
 *
 * @returns Status code
 */
static ib_status_t param_path_handler(
    ib_cfgparser_t  *cp,
    const char      *directive,
    const char      *value,
    void            *cbdata)
{
    assert(cp != NULL);
    assert(directive != NULL);
    assert(strcasecmp(directive, base_path_directive) == 0);
    assert(cbdata != NULL);

    const ib_module_t *module = cbdata;
    module_data_t     *mod_data = module->data;
    const char        *base_path = value;
    size_t             len;
    size_t             bufsize;
    char              *spath = NULL;
    char              *cpath = NULL;

    assert(mod_data != NULL);

    /* NULL or zero-length path? Do nothing */
    if (base_path == NULL) {
        goto cleanup;
    }
    len = strlen(base_path);
    if (len == 0) {
        goto cleanup;
    }
    bufsize = len + 8;

    /* Build client path */
    cpath = malloc(bufsize);
    if (cpath == NULL) {
        goto cleanup;
    }
    strcpy(cpath, base_path);
    strcat(cpath, ".client");

    /* Build server path */
    spath = malloc(bufsize);
    if (spath == NULL) {
        goto cleanup;
    }
    strcpy(spath, base_path);
    strcat(spath, ".server");

    /* Set it */
    ib_cfg_log_debug(cp, "Command socket paths: c=\"%s\" s=\"%s\"", cpath, spath);
    if (mod_data->client_path != NULL) {
        free((char *)mod_data->client_path);
    }
    if (mod_data->server_path != NULL) {
        free((char *)mod_data->server_path);
    }
    mod_data->client_path = cpath;
    mod_data->server_path = spath;
    return IB_OK;

cleanup:
    if (spath != NULL) {
        free(spath);
    }
    if (cpath != NULL) {
        free(cpath);
    }
    return IB_OK;
}

/**
 * Called at context close.
 *
 * @param[in] ib Engine
 * @param[in] ctx Context
 * @param[in] event Event triggering the callback
 * @param[in] cbdata Callback data (module).
 *
 * @returns Status code
 */
static ib_status_t context_close_handler(
    ib_engine_t           *ib,
    ib_context_t          *ctx,
    ib_state_event_type_t  event,
    void                  *cbdata
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(event == context_close_event);
    assert(cbdata != NULL);

    ib_module_t   *module = (ib_module_t *)cbdata;
    module_data_t *mod_data;

    /* Module data pointer points at our data */
    mod_data = module->data;

    /* All we care about is the main context */
    if ( (ib_context_type(ctx) == IB_CTYPE_MAIN) && (mod_data != NULL) ) {
        ib_status_t rc = command_sock_start(mod_data);
        if ( (rc != IB_OK) && (rc != IB_DECLINED) ) {
            return rc;
        }
    }
    
    return IB_OK;
}

/**
 * Finish for the command socket module.
 *
 * @param[in] ib IronBee Engine (unused)
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused)
 *
 * @returns Status code
 */
static ib_status_t module_finish(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata
)
{
    assert(module != NULL);
    module_data_t *mod_data = module->data;

    if (mod_data != NULL) {
        command_sock_cleanup(mod_data);
    }

    return IB_OK;
}

/**
 * Initialize for the command socket module.
 *
 * @param[in] ib IronBee Engine.
 * @param[in] module Module data.
 * @param[in] cbdata Callback data (unused).
 *
 * @returns Status code
 */
static ib_status_t module_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);

    ib_status_t           rc;
    ib_mm_t               mm;
    module_data_t        *mod_data;
    ib_ctrl_mod_handle_t  handle;

    ib_log_debug(ib, "Command socket module initializing");

    /* Get the engine's main memory manager */
    mm = ib_engine_mm_main_get(ib);

    /* Create the module data */
    mod_data = ib_mm_calloc(mm, 1, sizeof(*mod_data));
    if (mod_data == NULL) {
        ib_log_error(ib,
                     "Command socket: Failed to allocate module data structure");
        return IB_EALLOC;
    }

    /* Open the controller handle */
    rc = ib_ctrl_mod_open(&handle);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get module API: %s", ib_status_to_string(rc));
        return rc;
    }

    /* Populate the module data structure */
    mod_data->ib = ib;
    mod_data->handle = handle;
    mod_data->server_path = NULL;
    mod_data->client_path = NULL;
    mod_data->server_sock = -1;

    /* Save off pointer into the module object's data pointer. */
    module->data = mod_data;

    /* Fill in the data required for the sigio handler */
    sigio_data.mod_data = mod_data;

    /* Register the base path directive. */
    rc = ib_config_register_directive(ib,
                                      base_path_directive,
                                      IB_DIRTYPE_PARAM1,
                                      (ib_void_fn_t)param_path_handler,
                                      NULL,
                                      module,
                                      NULL,
                                      NULL);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to register %s directive: %s",
                     base_path_directive, ib_status_to_string(rc));
        return rc;
    }

    /* Register the context close callback */
    rc = ib_hook_context_register(ib,
                                  context_close_event,
                                  context_close_handler,
                                  module);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error registering context close hook: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    ib_log_debug(ib, "Command socket initialization complete");

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
    module_init,                             /* Initialize function */
    NULL,                                    /* Callback data */
    module_finish,                           /* Finish function */
    NULL,                                    /* Callback data */
);
