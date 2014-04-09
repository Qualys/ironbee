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
 * @brief IronBee --- Engine Manager Control Channel
 *
 * @author Sam Baskinger <sbaskinger@qualys.com>
 */

#ifndef __ENGINE_MANAGER_CONTROL_CHANNEL_H_
#define __ENGINE_MANAGER_CONTROL_CHANNEL_H_

/* This gets the PRI*64 types */
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#include <ironbee/engine.h>
#include <ironbee/engine_manager.h>
#include <ironbee/mm.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The largest message that can be sent to the channel.
 */
#define IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ 1024

 /**
 * @defgroup IronBeeEngMgrCtrlChan IronBee Engine Manager Control Channel
 * @ingroup IronBee
 *
 * Manage opening a communication channel by which a client can send
 * commands to an @ref ib_manager_t.
 *
 * An @ref ib_engine_manager_control_channel_t must be started to begin
 * receving commands, and should be stopped to clean up all allocated
 * resources.
 *
 * Cleanup is automatic when the @ref ib_mm_t that a controller is allocated
 * out of is destroyed.
 *
 * @{
 */

typedef struct ib_engine_manager_control_channel_t ib_engine_manager_control_channel_t;

/**
 * Callback function type.
 *
 * @param[in] mm Memory manager. All allocations, particularly, @a result,
 *            should be made from this.
 * @param[in] name The name this function is called as.
 * @parma[in] args The arguments as a single null-terminated string.
 * @param[out] result The result to send back to the user. This is set to
 *            point to NULL and if it remains unchanged, then the
 *            return code of this function is string-i-fied by
 *            ib_status_to_string() and returned.
 * @param[in] cbdata Callback data.
 *
 * @returns The status code to return to the user if no alternate message
 *          is provided by @a result.
 */
typedef ib_status_t(*ib_engine_manager_control_channel_cmd_fn_t)(
    ib_mm_t      mm,
    const char  *name,
    const char  *args,
    const char **result,
    void        *cbdata
);

/**
 * Create a stopped @ref ib_engine_manager_control_channel_t.
 *
 * The user must call ib_engine_manager_control_channel_start() to open the server
 * and start it processing events.
 *
 * @param[out] channel The created struct.
 * @param[in] mm The memory manager to allocate out of. This is probably
 *            always the same as @a manager, but it does not need to be.
 * @param[in] manager The manager we will be controlling.
 *
 * @returns
 * - IB_OK On success.
 * - IB_EALLOC On allocation errors.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_channel_create(
    ib_engine_manager_control_channel_t **channel,
    ib_mm_t                   mm,
    ib_manager_t             *manager
);

/**
 * Open a domain socket at /var/run/ironbee_channel.pid.sock.
 *
 * @returns
 * - IB_OK On success.
 * - Other on failure.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_channel_start(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Close a domain socket at /var/run/ironbee_channel.pid.sock.
 *
 * @returns
 * - IB_OK On success.
 * - Other on failure.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_channel_stop(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Check if data is available to receive.
 *
 * @param[in] channel The channel to receive from.
 *
 * @returns
 * - IB_OK If the channel is ready to receive a message.
 * - IB_EAGAIN If the channel has no data available.
 * - IB_OTHER Another error has occured.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_ready(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Recieve a command and process it.
 *
 * @param[in] channel The channel to receive from.
 *
 * @returns
 * - IB_OK If the channel received and successfuly dispatched a message.
 * - IB_ENOENT If an unknown command was received.
 * - IB_OTHER Another error has occured.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_recv(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Register @a fn with the channel as an available command.
 *
 * @param[in] channel The channel to register the command with.
 * @param[in] mm Allocations are done out of this memory manager.
 * @param[in] name The name of the command.
 * @param[in] fn The implementation of command @a name.
 * @param[in] cbdata Callback data provided to @a fn
 *
 * @returns
 * - IB_OK On success.
 * - IB_EALLOC On allocation error.
 * - Other failures.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_cmd_register(
    ib_engine_manager_control_channel_t        *channel,
    const char                                 *name,
    ib_engine_manager_control_channel_cmd_fn_t  fn,
    void                                       *cbdata
);

/**
 * Register the @c echo command with this channel.
 *
 * This is a useful command used for debugging or pings.
 * It will echo the the arguments submitted to it.
 *
 * @param[in] channel The channel to register this command with.
 *
 * @returns
 * - IB_OK On success.
 * - Other on registration failure.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_echo_register(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Register the default manager control commands.
 *
 * The commands registered are:
 * - enable - enable IronBee in the manager.
 * - disable - disable IronBee in the manager.
 * - cleanup - cleanup old IronBee engines in the manager.
 * - engine_create <config file> - Create a new engine.
 *   IronBee must not be disabled for this to succeed.
 *
 * @param[in] channel The channel to register this command with.
 *
 * @returns
 * - IB_OK On success.
 * - Other on registration failure.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_manager_ctrl_register(
    ib_engine_manager_control_channel_t *channel
);

/**
 * Return the path to the socket being used by this channel.
 *
 * @param[in] channel The channel to access the socket path of.
 *
 * @returns The socket path. This file may or may not exist
 * if the channel has been stopped.
 */
const char DLL_PUBLIC *ib_engine_manager_control_channel_socket_path_get(
    const ib_engine_manager_control_channel_t *channel
);

/**
 * Copy @a path as the socket path for this channel to use when it is opened.
 *
 * Users of this function should not call this after
 * ib_engine_manager_control_channel_start() has been called. Rather, stop the
 * channel, set this value, then start the channel.
 *
 * @note Channels are initialized in a stopped state, and so this may be
 * called after ib_engine_manager_control_channel_create().
 *
 * @param[in] channel The channel to set the socket for.
 * @param[in] path The path to the file that the socket should be created at.
 *            This string is copied using the channel's memory manager.
 *
 * @returns
 * - IB_OK On success.
 * - IB_EALLOC On allocation failure.
 */
ib_status_t DLL_PUBLIC ib_engine_manager_control_channel_socket_path_set(
    ib_engine_manager_control_channel_t *channel,
    const char                          *path
);

/**
 * Client function to open a socket path, send a msg and recv a response.
 *
 * This code is intended to handle the client-side of the messaging
 * protocol to an @ref ib_engine_manager_control_channel_t. As such,
 * it is not expected that the client can easily construct
 * an @ref ib_engine_manager_control_channel_t and so only the path to the
 * socket is required.
 *
 * @param[in] sock_path Path to the unix domain socket the channel is at.
 * @param[in] message The simple C-string message to send to the server.
 * @param[in] mm The memory manager used to allocate @a response from.
 * @param[out] response The response from the server is stored here.
 *
 * @returns
 * - IB_OK On succesfully interacting with the server. If the server
 *   returns an error, it is encoded in the @a response, not as
 *   the return code from ib_engine_manager_control_send().
 * - IB_EALLOC If allocations could not be made out of @a mm.
 * - IB_EOTHER On an unexpected system problem. Check errno.
 * - IB_EINVAL If @A message is too long to send to the server or the path
 *   is too long for a unix domain socket (107 characters + \0).
 */
ib_status_t ib_engine_manager_control_send(
    const char  *sock_path,
    const char  *message,
    ib_mm_t      mm,
    const char **response
);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* __ENGINE_MANAGER_CONTROL_CHANNEL_H_ */
