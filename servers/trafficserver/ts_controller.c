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
 * @brief IronBee --- Apache Traffic Server Controller Plugin
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

/**
 * @defgroup IronBeeTrafficServerControllerPlugin
 * IronBee Traffic Server controller plugin
 * @ingroup IronBeeTrafficServer
 *
 * The recognized command line options are:
 *  /path/to/ts_ironbee.so
*/

#include "ironbee_config_auto.h"
#include "ts_private.h"

#include <ironbee/ctrl_srv.h>
#include <ironbee/engine_state.h>
#include <ironbee/log.h>
#include <ironbee/mm_mpool.h>
#include <ironbee/module.h>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>

#include <ts/ts.h>

/**
 * Module data
 */
struct module_data_t {
    ib_mpool_t           *mpool;            /**< Memory pool */
    ib_mm_t               mm;               /**< Memory manager */
    ibts_module_data_t   *ts_module_data;  /**< Main module's data */
    const char           *ts_module_path;  /**< Path to main module */
    ib_ctrl_srv_handle_t  handle;           /**< Controller handle */
    ib_ctrl_srv_hooks_t   hooks;            /**< Controller plugin hooks */
};
typedef struct module_data_t module_data_t;

/**
 * Exit the server process
 *
 * @param[in] cbdata Callback data (module data)
 */
static void kill_server(
    void *cbdata
);

/**
 * Global module data
 */
static module_data_t module_data = {
    .mpool = NULL,
    .ts_module_data = NULL,
    .ts_module_path = NULL,
    .hooks = {
        .kill_fn = kill_server,
        .kill_data = &module_data,
    },
};

static void kill_server(
    void *cbdata
)
{
    module_data_t *mod_data = cbdata;

    /* Free up memory */
    if ( (mod_data != NULL) && (mod_data->mpool != NULL) ) {
        ib_mpool_destroy(mod_data->mpool);
        mod_data->mpool = NULL;
    }

    TSDebug("ironbee", "Shutting down server");
    exit(0);
}

/**
 * Check the traffic server version, verify that it's 3.0 or greater
 *
 * @param[in] mod_data Module data (unused)
 */
static int check_ts_version(
    module_data_t *mod_data
)
{

    const char *ts_version = TSTrafficServerVersionGet();
    int result = 0;

    if (ts_version) {
        int major = 0;
        int minor = 0;
        int patch = 0;

        if (sscanf(ts_version, "%d.%d.%d", &major, &minor, &patch) != 3) {
            return 0;
        }

        /* Need at least TS 3.0 */
        if (major >= 3) {
            result = 1;
        }

    }

    return result;
}

/**
 * Generate default module path
 *
 * @param[in] mod_data Module data
 * @param[in] argv0 argv[0] from command line
 *
 * @returns Success/Failure parsing the config line
 */
static const char *default_module_path(
    const module_data_t *mod_data,
    const char          *argv0
)
{
    assert(mod_data != NULL);
    assert(argv0 != NULL);

    static const char *default_name = "ts_ironbee.so";

    char              *full;
    char              *path;
    const char        *dir;
    size_t             size;

    /* Extract the directory from argv0 */
    path = ib_mm_strdup(mod_data->mm, argv0);
    if (path == NULL) {
        return NULL;
    }
    dir = dirname(path);
    if (dir == NULL) {
        return NULL;
    }

    /* Build a full path based on the directory */
    size = strlen(dir) + strlen(default_name) + 2;
    full = ib_mm_alloc(mod_data->mm, size);
    if (full == NULL) {
        return NULL;
    }
    strcpy(full, dir);
    strcat(full, "/");
    strcat(full, default_name);

    /* Done */
    return full;
}

/**
 * Function to process TS-style argc/argv command line arguments
 *
 * @param[in] mod_data Module data
 * @param[in] argc Command-line argument count
 * @param[in] argv Command-line argument list
 *
 * @returns Success/Failure parsing the config line
 */
static ib_status_t process_argv(
    module_data_t *mod_data,
    int            argc,
    const char    *argv[]
)
{
    assert(mod_data != NULL);
    assert(argv != NULL);

    int c;

    /* const-ness mismatch looks like an oversight, so casting should be fine */
    optind = 1;    /* Reset */
    while (c = getopt(argc, (char**)argv, "m:"), c != -1) {
        switch(c) {
        case 'm':
            mod_data->ts_module_path = ib_mm_strdup(mod_data->mm, optarg);
            break;
        default:
            TSError("[ironbee] Unrecognised option -%c ignored.", optopt);
            break;
        }
    }

    /* If not specified on command line, create a default path */
    if (mod_data->ts_module_path == NULL) {
        const char *path = default_module_path(mod_data, argv[0]);
        if (path == NULL) {
            return IB_EALLOC;
        }
        mod_data->ts_module_path = path;
    }

    return IB_OK;
}

/**
 * Initialize the IronBee ATS plugin.
 *
 * Performs initializations required by ATS.
 *
 * @param[in] argc Command-line argument count
 * @param[in] argv Command-line argument list
 */
void TSPluginInit(int argc, const char *argv[])
{
    ib_status_t               rc;
    TSPluginRegistrationInfo  registration_info;
    void                     *dlhandle = NULL;
    module_data_t            *mod_data = &module_data;
    ibts_shared_data_t     *shared_data = NULL;
    const char               *shared_data_name;
    ib_mpool_t               *mpool;
    ib_mm_t                   mm;

    /* FIXME - check why these are char*, not const char* */
    registration_info.plugin_name = (char *)"ironbee command socket";
    registration_info.vendor_name = (char *)"Qualys, Inc";
    registration_info.support_email = (char *)"ironbee-users@lists.sourceforge.com";

    /* Register and check version */
    if (TSPluginRegister(TS_SDK_VERSION_3_0, &registration_info) != TS_SUCCESS) {
        TSError("[ironbee] Plugin registration failed.");
        goto done;
    }
    if (!check_ts_version(mod_data)) {
        TSError("[ironbee] Plugin requires Traffic Server 3.0 or later");
        goto done;
    }

    /* Create the memory pool */
    rc = ib_mpool_create(&mpool, "ATS Controller Plugin", NULL);
    if (rc != IB_OK) {
        TSError("[ironbee] Failed to create memory pool");
    }
    mod_data->mpool = mpool;
    mm = ib_mm_mpool(mpool);
    mod_data->mm = mm;

    /* Process argv */
    rc = process_argv(mod_data, argc, argv);
    if (rc != IB_OK) {
        TSError("[ironbee] Failed to process argv: %s", ib_status_to_string(rc));
        goto done;
    }

    /* Get handle to the "main" IronBee ATS plugin */
    dlhandle = dlopen(mod_data->ts_module_path, RTLD_NOW);
    if (dlhandle == NULL) {
        TSError("[ironbee] Failed to access IronBee plugin \"%s\"",
                mod_data->ts_module_path);
        goto done;
    }

    /* Find the command socket data */
    shared_data_name = IBTS_SHARED_STR;
    shared_data = dlsym(dlhandle, shared_data_name);
    if (shared_data == NULL) {
        TSError("[ironbee] Failed to find symbol \"%s\": %s",
                shared_data_name, dlerror());
        goto done;
    }
    dlclose(dlhandle);
    dlhandle = NULL;
    assert (shared_data->module_data != NULL);
    mod_data->ts_module_data = shared_data->module_data;

    /* Copy out the hooks provided by the module */
    mod_data->hooks.flush_fn     = shared_data->hooks.flush_fn;
    mod_data->hooks.flush_data   = shared_data->hooks.flush_data;
    mod_data->hooks.destroy_fn   = shared_data->hooks.destroy_fn;
    mod_data->hooks.destroy_data = shared_data->hooks.destroy_data;

    /* Create the command socket */
    rc = ib_ctrl_srv_open(shared_data->module_data->manager,
                          &mod_data->hooks,
                          &mod_data->handle);
    if (rc != IB_OK) {
        TSError("[ironbee] Failed to open controller: %s", ib_status_to_string(rc));
        goto done;
    }

    TSDebug("ironbee", "Controller plugin initialized");

done:
    if (dlhandle != NULL) {
        dlclose(dlhandle);
    }
}
