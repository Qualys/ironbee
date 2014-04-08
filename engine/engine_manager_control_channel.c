
#include <ironbee/engine_manager_control_channel.h>
#include <ironbee/engine_manager.h>

#include <ironbee/hash.h>
#include <ironbee/mpool_lite.h>
#include <ironbee/mm.h>
#include <ironbee/mm_mpool_lite.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

//! The directory that the @ref DEFAULT_SOCKET_BASENAME will be created in.
static const char *DEFAULT_SOCKET_PATH = "/var/run/";

//! Basename of the socket file. The pid and ".sock" will be appended to this.
static const char *DEFAULT_SOCKET_BASENAME = "ironbee_manager_controller.sock";

/**
 * Structure to hold and manipulate pointers to command implementations.
 */
struct cmd_t {
    ib_engine_manager_control_channel_cmd_fn_t fn; /**< The command. */
    void       *cbdata; /**< Callback data. */
    const char *name;   /**< The command name this is registered under. */
};
typedef struct cmd_t cmd_t;

struct ib_engine_manager_control_channel_t {
    ib_mm_t       mm;              /**< Manager with the lifetime of this. */
    ib_manager_t *manager;         /**< The manager we will be controlling. */
    const char   *sock_path;       /**< The path to the socket file. */
    int           sock;            /**< Socket. -1 If not open. */
    /**
     * Message buffer.
     */
    uint8_t       msg[IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ+1];
    size_t        msgsz;           /**< How much data is in msg. */
    ib_hash_t    *cmds;            /**< Collection of cmd_t structures. */
};

/**
 * Cleanup the manager controller.
 *
 * @param[in] cbdata The @ref ib_engine_manager_control_channel_t created by
 *            ib_engine_manager_control_channel__create().
 */
static void channel_cleanup(void *cbdata) {
    assert(cbdata != NULL);

    ib_engine_manager_control_channel_t *manager_controller =
        (ib_engine_manager_control_channel_t *)cbdata;

    ib_engine_manager_control_channel_stop(manager_controller);
}

static ib_status_t echo_cmd(
    ib_mm_t      mm,
    const char  *name,
    const char  *args,
    const char **result,
    void        *cbdata
)
{
    size_t len = strlen(name) + strlen(args) + 2;
    char   *r;

    r = ib_mm_alloc(mm, len);
    if (r == NULL) {
        return IB_EALLOC;
    }

    snprintf(r, len, "%s %s", name, args);

    *result = r;

    return IB_OK;
}

/**
 * Log an error message through the current IronBee engine.
 *
 * If there is no active engine available, stderr is used.
 *
 * This function exists to make consistent the slightly complex task
 * of fetching an @ref ib_engine_t and logging to it and returning it to
 * the engine manager.
 *
 * @param[in] channel The channel that we are logging about.
 * @param[in] action The action that failed to be performed.
 * @param[in] msg The message that describes the failure of the @a action.
 */
static void log_socket_error(
    ib_engine_manager_control_channel_t *channel,
    const char *action,
    const char *msg
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);

    ib_engine_t *ib;
    ib_status_t rc;

    rc = ib_manager_engine_acquire(channel->manager, &ib);
    if (rc == IB_OK) {
        ib_log_error(
            ib,
            "Failed to %s socket %s: %s",
            action,
            channel->sock_path,
            msg
        );
        ib_manager_engine_release(channel->manager, ib);
    }
}

/**
 * Allocate and construct a socket path.
 *
 * @param[in] mm Allocate @a path from this.
 * @param[out] path Assign the path here.
 *
 * @returns
 * - IB_OK On success.
 * - IB_EALLOC on allocation errors.
 */
static ib_status_t build_sock_path(ib_mm_t mm, const char **path)
{
    char   *sock_path;
    size_t  sock_path_len;

    /* Compute the string length (minus the end-of-line character). */
    sock_path_len =
        strlen(DEFAULT_SOCKET_PATH)     + /* Path. */
        strlen(DEFAULT_SOCKET_BASENAME)   /* Basename length. */
    ;

    sock_path = ib_mm_alloc(mm, sock_path_len + 1);
    if (sock_path == NULL) {
        return IB_EALLOC;
    }

    /* Initialize sock_path to length zero. */
    sock_path[0] = '\0';
    strcat(sock_path, DEFAULT_SOCKET_PATH);
    strcat(sock_path, DEFAULT_SOCKET_BASENAME);
    sock_path[sock_path_len] = '\0';

    *path = sock_path;
    return IB_OK;
}

ib_status_t ib_engine_manager_control_channel_create(
    ib_engine_manager_control_channel_t **channel,
    ib_mm_t                               mm,
    ib_manager_t                         *manager
)
{
    ib_status_t              rc;
    ib_engine_manager_control_channel_t *mc;

    mc = (ib_engine_manager_control_channel_t *)ib_mm_alloc(mm, sizeof(*mc));
    if (mc == NULL) {
        return IB_EALLOC;
    }

    rc = ib_hash_create(&(mc->cmds), mm);
    if (rc != IB_OK) {
        return rc;
    }

    mc->sock      = -1;
    mc->manager   = manager;
    mc->mm        = mm;

    /* Ensure that the last character is always terminated. */
    mc->msg[IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ] = '\0';

    rc = build_sock_path(mm, &(mc->sock_path));
    if (rc != IB_OK) {
        return rc;
    }

    ib_mm_register_cleanup(mm, channel_cleanup, mc);

    *channel = mc;
    return IB_OK;
}


ib_status_t ib_engine_manager_control_channel_stop(
    ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);
    assert(channel->msg != NULL);

    if (channel->sock >= 0) {
        int sysrc;

        close(channel->sock);
        channel->sock = -1;

        /* Remove the socket file so external programs know it's closed. */
        sysrc = unlink(channel->sock_path);
        if (sysrc == -1 && errno != ENOENT) {
            log_socket_error(channel, "unlink socket", strerror(errno));
            return IB_EOTHER;
        }
    }

    return IB_OK;
}

ib_status_t ib_engine_manager_control_channel_start(
    ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);
    assert(channel->msg != NULL);

    int sysrc;
    int sock;
    struct sockaddr_un addr;

    /* Socket path is too long for the path. */
    if (strlen(channel->sock_path) + 1 >= sizeof(addr.sun_path)) {
        return IB_EINVAL;
    }

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_socket_error(channel, "create socket", strerror(errno));
        return IB_EOTHER;
    }

    addr.sun_family = AF_UNIX;

    strcpy(addr.sun_path, channel->sock_path);

    sysrc = unlink(addr.sun_path);
    if (sysrc == -1 && errno != ENOENT) {
        log_socket_error(channel, "unlink old socket", strerror(errno));
        return IB_EOTHER;
    }

    sysrc = bind(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (sysrc == -1) {
        log_socket_error(channel, "bind", strerror(errno));
        return IB_EOTHER;
    }

    channel->sock = sock;

    return IB_OK;
}

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
ib_status_t ib_engine_manager_control_ready(
    ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);
    assert(channel->msg != NULL);

    int sysrc;
    const int nfds = channel->sock + 1;
    struct timeval timeout;

    /* Do not block. */
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    fd_set readfds;
    fd_set exceptfds;

    FD_ZERO(&readfds);
    FD_ZERO(&exceptfds);

    sysrc = select(nfds, &readfds, NULL, &exceptfds, &timeout);
    if (sysrc < 0) {
        log_socket_error(channel, "select from", strerror(errno));
        return IB_EOTHER;
    }

    if (sysrc > 0) {
        /* sysrc > 0. */
        if (FD_ISSET(channel->sock, &exceptfds)) {
            log_socket_error(channel, "error on", strerror(errno));
            return IB_EOTHER;
        }

        if (FD_ISSET(channel->sock, &readfds)) {
            return IB_OK;
        }
    }

    /* We get here by channel->sock not being in the read set or no sockets
     * appearing in any set (read or exception). */
    return IB_EAGAIN;
}

static ib_status_t handle_command(
    ib_engine_manager_control_channel_t *channel,
    uint8_t           *cmdline,
    size_t             cmdlinesz,
    const struct sockaddr_un *src_addr,
    socklen_t          addrlen
)
{
    assert(channel != NULL);
    assert(cmdline != NULL);

    /* What we consider whitespace. */
    static const char *ws = "\r\n\t ";

    ib_status_t      rc;
    cmd_t           *cmd;
    ib_mpool_lite_t *mp;
    ib_mm_t          mm;
    const char      *result = NULL;
    char            *name;
    char            *args;
    size_t           name_len;
    ssize_t          written;

    rc = ib_mpool_lite_create(&mp);
    if (rc != IB_OK) {
        return rc;
    }

    mm = ib_mm_mpool_lite(mp);

    /* Copy the cmdline so we can modify it freely. */
    name = ib_mm_strdup(mm, (const char *)cmdline);
    if (name == NULL) {
        return IB_EALLOC;
    }

    /* Skip over ws character. */
    name    += strspn(name, ws);

    /* A command of all white space? Error. */
    if (*name == '\0') {
        log_socket_error(
            channel,
            "with invalid command",
            "Command name is entirely whitespace.");
        return IB_EINVAL;
    }

    /* Find the next whitespace character. That's the length of the name. */
    name_len = strcspn(name, ws);

    /* If name[len] is not the end of the command line, parse the args. */
    if (name[name_len] != '\0') {

        /* Terminate the name. */
        name[name_len] = '\0';

        /* Point args past the name. It may point at more leading ws.*/
        args = name+name_len+1;

        /* Skip whitespace, as above with name. */
        args += strspn(args, ws);
    }
    else {
        args = "";
    }

    rc = ib_hash_get(channel->cmds, &cmd, name);
    if (rc == IB_ENOENT) {
        log_socket_error(channel, "find command", name);
        result = "ENOENT: Command not found.";
    }
    else if (rc != IB_OK) {
        log_socket_error(channel, "retrieve command", name);
        result = NULL;
    }
    else {
        rc = cmd->fn(
            mm,
            name,
            args,
            &result,
            cmd->cbdata);
    }

    if (result == NULL) {
        result = ib_status_to_string(rc);
    }

    written = sendto(
        channel->sock,
        (void *)result,
        strlen(result),
        0,
        (const struct sockaddr *)src_addr,
        addrlen);
    if (written == -1) {
        log_socket_error(channel, "write result response", strerror(errno));
        return IB_EOTHER;
    }

    ib_mpool_lite_destroy(mp);

    return IB_OK;
}

ib_status_t ib_engine_manager_control_recv(
    ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);
    assert(channel->msg != NULL);

    /* Ensure that the last byte is always null. */
    assert(channel->msg[IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ] == '\0');

    ib_status_t        rc;
    ssize_t            recvsz = 0;
    struct sockaddr_un src_addr;
    socklen_t          addrlen = sizeof(src_addr);
    recvsz =
        recvfrom(
            channel->sock,
            &(channel->msg),
            IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ,
            0,
            (struct sockaddr *)&src_addr,
            &addrlen);
    if (recvsz == -1) {
        /* On recv error the message in the channel buffer is not valid.
         * Set the length to 0. */
        channel->msgsz = 0;

        log_socket_error(channel, "receive message on", strerror(errno));

        return IB_EOTHER;
    }

    /* Null terminate the message. NOTE: The message buffer is 1 byte
     * larger than the max message we will receive, so an out-of-bounds
     * check has already been implicitly done by recvfrom(). */
    (channel->msg)[recvsz] = '\0';

    channel->msgsz = (size_t)recvsz;
    rc = handle_command(
        channel,
        channel->msg,
        channel->msgsz,
        &src_addr,
        addrlen);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_engine_manager_control_send(
    const char  *sock_path,
    const char  *message,
    ib_mm_t      mm,
    const char **response
)
{
    ib_status_t        rc = IB_OK;
    size_t             message_len  = strlen(message);
    int                sock;
    struct sockaddr_un dst_addr;
    struct sockaddr_un src_addr;
    int                sysrc;
    ssize_t            ssz;
    char              *resp; /* Our copy of response. */

    /* The message is too long. */
    if (message_len > IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ) {
        return IB_EINVAL;
    }

    /* Path to socket is too long. */
    if (strlen(sock_path) + 1 >= sizeof(dst_addr.sun_path)) {
        return IB_EINVAL;
    }

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        return IB_EOTHER;
    }

    dst_addr.sun_family = AF_UNIX;
    src_addr.sun_family = AF_UNIX;

    strcpy(dst_addr.sun_path, sock_path);
    sysrc = snprintf(
        src_addr.sun_path,
        sizeof(src_addr.sun_path),
        "/tmp/ibctrl.%d.S",
        getpid());
    if (sysrc < 0) {
        rc = IB_EINVAL;
        goto cleanup_sock;
    }

    unlink(src_addr.sun_path);
    sysrc = bind(sock, (const struct sockaddr *)&src_addr, sizeof(src_addr));
    if (sysrc == -1) {
        rc = IB_EOTHER;
        goto cleanup_sock;
    }

    ssz = sendto(
        sock,
        message,
        message_len,
        0,
        (struct sockaddr *)&dst_addr,
        sizeof(dst_addr));
    if (ssz == EMSGSIZE) {
        /* By API contract, we return IB_EINVAL here.
         * In practice, this should never happen. */
        rc = IB_EINVAL;
        goto cleanup;
    }
    if (ssz < 0) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    /* Since this is a datagram protocol, we should have sent everything. */
    assert(ssz == (ssize_t)message_len);

    /* Allocate after sending the message to the server.
     * It is more likely that the server is down, so we defer allocating mem as
     * that should almost always succeed. */
    resp = ib_mm_alloc(mm, IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ+1);
    if (resp == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }

    ssz = recvfrom(
        sock,
        resp,
        IB_ENGINE_MANAGER_CONTROL_CHANNEL_MAX_MSG_SZ,
        0,
        NULL,
        NULL);
    if (ssz == -1) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    /* Ensure this is null-terminated. */
    resp[ssz] = '\0';
    *response = resp;

cleanup:
    unlink(src_addr.sun_path);
cleanup_sock:
    close(sock);

    return rc;
}

ib_status_t ib_engine_manager_control_cmd_register(
    ib_engine_manager_control_channel_t        *channel,
    const char                                 *name,
    ib_engine_manager_control_channel_cmd_fn_t  fn,
    void                                       *cbdata
)
{
    assert(channel != NULL);
    assert(channel->manager != NULL);
    assert(channel->cmds != NULL);
    assert(name != NULL);

    cmd_t       *cmd;
    ib_status_t  rc;

    cmd = ib_mm_alloc(channel->mm, sizeof(*cmd));
    cmd->fn = fn;
    cmd->cbdata = cbdata;
    cmd->name = ib_mm_strdup(channel->mm, name);
    if (name == NULL) {
        return IB_EALLOC;
    }

    rc = ib_hash_set(channel->cmds, name, cmd);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_engine_manager_control_echo_register(
    ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);

    return ib_engine_manager_control_cmd_register(
        channel,
        "echo",
        echo_cmd, NULL);
}

const char *ib_engine_manager_control_channel_socket_path_get(
    const ib_engine_manager_control_channel_t *channel
)
{
    assert(channel != NULL);
    assert(channel->sock_path != NULL);

    return channel->sock_path;
}

ib_status_t ib_engine_manager_control_channel_socket_path_set(
    ib_engine_manager_control_channel_t *channel,
    const char                          *path
)
{
    assert(channel != NULL);

    const char *path_cpy = ib_mm_strdup(channel->mm, path);

    if (path_cpy == NULL) {
        return IB_EALLOC;
    }

    channel->sock_path = path_cpy;

    return IB_OK;
}