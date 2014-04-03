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
 * @brief IronBee --- Apache Traffic Server Plugin
 *
 * @author Nick Kew <nkew@qualys.com>
 */

#include "ironbee_config_auto.h"
#include "ts_private.h"

#include <ironbee/flags.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>
#include <ts/ts.h>

/**
 * @defgroup IronBeeTrafficServer Apache Traffic Server module.
 *
 * This module takes advantage of Traffic Server's ability to passing command
 * line arguments to modules.  To do this, add arguments to the ts_ironbee
 * module load in 'plugin.config'.  These arguments are parsed with getopt(3)
 * according to Unix/shell command line rules.
 *
 * The recognized arguments are:
 * - <tt>-L</tt>
 *   Disable logging
 * - <tt>-l @<log file@></tt>
 *   Specify the file to use for IronBee logging
 * - <tt>-v @<log level@></tt>
 *   Specify the IronBee log level to use for IronBee logging (can be numeric
 *   or symbolic)
 * - <tt>-m @<max engines@></tt>
 *   Specify the maximum number of simultaneous IronBee engines
 *
 * Example plugin.config
 *   /local/ib/lib64/libloader.so /local/ib/lib64/libironbee.so
 *   /local/ib/libexec/ts_ironbee.so -v trace -l ts-ironbee.log -m 10 /local/ib/ts.conf
 */

#include <sys/socket.h>
#include <netdb.h>

/* This gets the PRI*64 types */
# define __STDC_FORMAT_MACROS 1
# include <inttypes.h>

#include <ironbee/engine.h>
#include <ironbee/engine_manager.h>
#include <ironbee/config.h>
#include <ironbee/server.h>
#include <ironbee/context.h>
#include <ironbee/core.h>
#include <ironbee/logger.h>
#include <ironbee/site.h>
#include <ironbee/state_notify.h>
#include <ironbee/util.h>
#include <ironbee/string.h>

/* TxLog private "API" */
#include "../../modules/txlog.h"

static void addr2str(const struct sockaddr *addr, char *str, int *port);

#define ADDRSIZE 48 /* what's the longest IPV6 addr ? */
#define DEFAULT_LOG "ts-ironbee"
#define DEFAULT_TXLOG "txlogs/tx-ironbee"

typedef enum {LE_N, LE_RN, LE_ANY} http_lineend_t;

/* Global module data */
static ibts_module_data_t module_data =
{
    .ib_initialized = false,
    .logger = NULL,
    .manager = NULL,
    .max_engines = IB_MANAGER_DEFAULT_MAX_ENGINES,
    .config_file = NULL,
    .log_file = NULL,
    .log_level = IB_LOG_WARNING,
    .log_disable = false,
    .txlogfile = DEFAULT_TXLOG,
    .txlogger = NULL,
};

typedef enum {
    HDR_OK,
    HDR_ERROR,
    HDR_HTTP_100,
    HDR_HTTP_STATUS
} ib_hdr_outcome;
#define IB_HDR_OUTCOME_IS_HTTP_OR_ERROR(outcome, data) \
    (((outcome) == HDR_HTTP_STATUS  || (outcome) == HDR_ERROR) && (data)->status >= 200 && (data)->status < 600)
#define IB_HTTP_CODE(num) ((num) >= 200 && (num) < 600)

typedef struct {
    ib_conn_t *iconn;
    /* store the IPs here so we can clean them up and not leak memory */
    char remote_ip[ADDRSIZE];
    char local_ip[ADDRSIZE];
    TSHttpTxn txnp; /* hack: conn data requires txnp to access */
    /* Keep track of whether this is open and has active transactions */
    int txn_count;
    int closing;
    ib_lock_t mutex;
    /* include the contp, so we can delay destroying it from the event */
    TSCont contp;
} ib_ssn_ctx;

typedef struct {
    /* data filtering stuff */
    TSVIO output_vio;
    TSIOBuffer output_buffer;
    TSIOBufferReader output_reader;
    char *buf;
    unsigned int buflen;
    unsigned int buffered;
    /* Nobuf - no buffering
     * Discard - transmission aborted, discard remaining data
     * buffer - buffer everything until EOS or abortedby error
     */
    enum { IOBUF_NOBUF, IOBUF_DISCARD, IOBUF_BUFFER_ALL,
           IOBUF_BUFFER_FLUSHALL, IOBUF_BUFFER_FLUSHPART } buffering;
    /* use new field for size.  May replace buflen once this is stable */
    ssize_t buf_limit;
    ssize_t log_limit;
} ib_filter_ctx;

#define IBD_REQ IB_SERVER_REQUEST
#define IBD_RESP IB_SERVER_RESPONSE
#define HDRS_IN IB_SERVER_REQUEST
#define HDRS_OUT IB_SERVER_RESPONSE
#define START_RESPONSE 0x04
#define DATA 0

typedef struct hdr_action_t {
    ib_server_header_action_t action;
    ib_server_direction_t dir;
    const char *hdr;
    const char *value;
    struct hdr_action_t *next;
} hdr_action_t;

typedef struct hdr_list {
    char *hdr;
    char *value;
    struct hdr_list *next;
} hdr_list;

typedef struct {
    const char *ctype;
    const char *redirect;
    const char *authn;
    const char *body;
} error_resp_t;

typedef struct {
    ib_ssn_ctx *ssn;
    ib_tx_t *tx;
    TSHttpTxn txnp;
    ib_filter_ctx in;
    ib_filter_ctx out;
    int state;
    int status;
    hdr_action_t *hdr_actions;
    hdr_list *err_hdrs;
    char *err_body;      /* this one can't be const */
    size_t err_body_len; /* Length of err_body. */
} ib_txn_ctx;

typedef struct {
    ib_server_direction_t dir;

    const char *type_label;
    const char *dir_label;
    TSReturnCode (*hdr_get)(TSHttpTxn, TSMBuffer *, TSMLoc *);

    ib_status_t (*ib_notify_header)(ib_engine_t*, ib_tx_t*,
                 ib_parsed_headers_t*);
    ib_status_t (*ib_notify_header_finished)(ib_engine_t*, ib_tx_t*);
    ib_status_t (*ib_notify_body)(ib_engine_t*, ib_tx_t*, const char*, size_t);
    ib_status_t (*ib_notify_end)(ib_engine_t*, ib_tx_t*);
    ib_status_t (*ib_notify_post)(ib_engine_t*, ib_tx_t*);
    ib_status_t (*ib_notify_log)(ib_engine_t*, ib_tx_t*);
} ib_direction_data_t;

static ib_direction_data_t ib_direction_client_req = {
    IBD_REQ,
    "client request",
    "request",
    TSHttpTxnClientReqGet,
    ib_state_notify_request_header_data,
    ib_state_notify_request_header_finished,
    ib_state_notify_request_body_data,
    ib_state_notify_request_finished,
    NULL,
    NULL
};
static ib_direction_data_t ib_direction_server_resp = {
    IBD_RESP,
    "server response",
    "response",
    TSHttpTxnServerRespGet,
    ib_state_notify_response_header_data,
    ib_state_notify_response_header_finished,
    ib_state_notify_response_body_data,
    ib_state_notify_response_finished,
    ib_state_notify_postprocess,
    ib_state_notify_logging
};
static ib_direction_data_t ib_direction_client_resp = {
    IBD_RESP,
    "client response",
    "response",
    TSHttpTxnClientRespGet,
    ib_state_notify_response_header_data,
    ib_state_notify_response_header_finished,
    ib_state_notify_response_body_data,
    ib_state_notify_response_finished,
    ib_state_notify_postprocess,
    ib_state_notify_logging
};

typedef struct {
    const ib_direction_data_t *ibd;
    ib_filter_ctx *data;
} ibd_ctx;

/**
 * IronBee connection cleanup
 *
 * @param[in] data Callback data (IronBee engine)
 */
static void cleanup_ib_connection(void *data)
{
    assert(data != NULL);
    assert(module_data.manager != NULL);

    ib_engine_t *ib = (ib_engine_t *)data;

    /* Release the engine, but don't destroy it */
    ib_manager_engine_release(module_data.manager, ib);
}

static void tx_finish(ib_tx_t *tx)
{
    if (!ib_flags_all(tx->flags, IB_TX_FREQ_FINISHED) ) {
        ib_state_notify_request_finished(tx->ib, tx);
    }
    if (!ib_flags_all(tx->flags, IB_TX_FRES_FINISHED) ) {
        ib_state_notify_response_finished(tx->ib, tx);
    }
    if (!ib_flags_all(tx->flags, IB_TX_FPOSTPROCESS)) {
        ib_state_notify_postprocess(tx->ib, tx);
    }
    if (!ib_flags_all(tx->flags, IB_TX_FLOGGING)) {
        ib_state_notify_logging(tx->ib, tx);
    }
}

static void tx_list_destroy(ib_conn_t *conn)
{
    while (conn->tx_first != NULL) {
        tx_finish(conn->tx_first);
        ib_tx_destroy(conn->tx_first);
    }
}


static bool is_error_status(int status)
{
    return ( (status >= 200) && (status < 600) );
}

/**
 * Callback functions for IronBee to signal to us
 */
static
ib_status_t ib_header_callback(
    ib_tx_t                   *tx,
    ib_server_direction_t      dir,
    ib_server_header_action_t  action,
    const char                *name,
    size_t                     name_length,
    const char                *value,
    size_t                     value_length,
    void                      *cbdata
)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_action_t *header;
    /* Logic for whether we're in time for the requested action */
    /* Output headers can change any time before they're sent */
    /* Input headers can only be touched during their read */

    if (ctx->state & HDRS_OUT ||
        (ctx->state & HDRS_IN && dir == IB_SERVER_REQUEST))
    {
        ib_log_debug_tx(tx, "Too late to change headers.");
        return IB_DECLINED;  /* too late for requested op */
    }

    header = TSmalloc(sizeof(*header));
    header->next = ctx->hdr_actions;
    ctx->hdr_actions = header;
    header->dir = dir;
    /* FIXME: deferring merge support - implementing append instead */
    header->action = action = action == IB_HDR_MERGE ? IB_HDR_APPEND : action;
    header->hdr = TSstrndup(name, name_length);
    header->value = TSstrndup(value, value_length);

    return IB_OK;
}

/**
 * Handler function to generate an error response
 */
static void error_response(TSHttpTxn txnp, ib_txn_ctx *txndata)
{
    const char *reason = TSHttpHdrReasonLookup(txndata->status);
    TSMBuffer bufp;
    TSMLoc hdr_loc;
    TSMLoc field_loc;
    hdr_list *hdrs;
    TSReturnCode rv;

    if (TSHttpTxnClientRespGet(txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
        TSError("[ironbee] ErrorDoc: couldn't retrieve client response header.");
        TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
        return;
    }
    rv = TSHttpHdrStatusSet(bufp, hdr_loc, txndata->status);
    if (rv != TS_SUCCESS) {
        TSError("[ironbee] ErrorDoc: TSHttpHdrStatusSet");
    }
    if (reason == NULL) {
        reason = "Other";
    }
    rv = TSHttpHdrReasonSet(bufp, hdr_loc, reason, strlen(reason));
    if (rv != TS_SUCCESS) {
        TSError("[ironbee] ErrorDoc: TSHttpHdrReasonSet");
    }

    while (hdrs = txndata->err_hdrs, hdrs != 0) {
        txndata->err_hdrs = hdrs->next;
        rv = TSMimeHdrFieldCreate(bufp, hdr_loc, &field_loc);
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] ErrorDoc: TSMimeHdrFieldCreate");
            goto errordoc_free;
        }
        rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                   hdrs->hdr, strlen(hdrs->hdr));
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] ErrorDoc: TSMimeHdrFieldNameSet");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, -1,
                                        hdrs->value, strlen(hdrs->value));
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] ErrorDoc: TSMimeHdrFieldValueStringInsert");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] ErrorDoc: TSMimeHdrFieldAppend");
            goto errordoc_free1;
        }

errordoc_free1:
        rv = TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] ErrorDoc: TSHandleMLocRelease 1");
            goto errordoc_free;
        }
errordoc_free:
        TSfree(hdrs->hdr);
        TSfree(hdrs->value);
        TSfree(hdrs);
    }

    if (txndata->err_body) {
        /* this will free the body, so copy it first! */
        TSHttpTxnErrorBodySet(txnp, txndata->err_body,
                              txndata->err_body_len, NULL);
    }
    rv = TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    if (rv != TS_SUCCESS) {
        TSError("[ironbee] ErrorDoc: TSHandleMLocRelease 2");
    }

    TSDebug("ironbee", "Sent error page %d \"%s\".", txndata->status, reason);
}

static ib_status_t ib_error_callback(ib_tx_t *tx, int status, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    TSDebug("ironbee", "ib_error_callback with status=%d", status);
    if ( is_error_status(status) ) {
        if (is_error_status(ctx->status) ) {
            ib_log_debug_tx(tx, "Ignoring: status already set to %d", ctx->status);
            return IB_OK;
        }
        /* We can't return an error after the response has started */
        if (ctx->state & START_RESPONSE) {
            ib_log_debug_tx(tx, "Too late to change status=%d", status);
            return IB_DECLINED;
        }
        /* ironbee wants to return an HTTP status.  We'll oblige */
        /* FIXME: would the semantics work for 1xx?  Do we care? */
        /* No, we don't care unless a use case arises for the proxy
         * to initiate a 1xx response independently of the backend.
         */
        ctx->status = status;
        return IB_OK;
    }
    return IB_ENOTIMPL;
}

static
ib_status_t ib_errhdr_callback(
    ib_tx_t    *tx,
    const char *name,
    size_t      name_length,
    const char *value,
    size_t      value_length,
    void       *cbdata
)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_list *hdrs;
    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE)
        return IB_DECLINED;
    if (!name || !value)
        return IB_EINVAL;
    hdrs = TSmalloc(sizeof(*hdrs));
    hdrs->hdr = TSstrndup(name, name_length);
    hdrs->value = TSstrndup(value, value_length);
    hdrs->next = ctx->err_hdrs;
    ctx->err_hdrs = hdrs;
    return IB_OK;
}

static ib_status_t ib_errbody_callback(
    ib_tx_t *tx,
    const char *data,
    size_t dlen,
    void *cbdata)
{
    uint8_t *err_body;
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;

    /* Handle No Data as zero length data. */
    if (data == NULL || dlen == 0) {
        return IB_OK;
    }

    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE) {
        return IB_DECLINED;
    }

    err_body = TSmalloc(dlen);
    if (err_body == NULL) {
        return IB_EALLOC;
    }

    ctx->err_body = memcpy(err_body, data, dlen);
    ctx->err_body_len = dlen;
    return IB_OK;
}

/**
 * Called by IronBee when a connection should be blocked by closing the conn.
 *
 * If this returns not-IB_OK a block by status code will be attempted.
 *
 * @param[in] conn The connection to close.
 * @param[in] tx The transaction, if available. If a transaction is
 *            not available, this will be NULL.
 * @param[in] cbdata Callback data.
 *
 * @returns
 *   - IB_OK on success.
 */
static ib_status_t ib_errclose_callback(
    ib_conn_t *conn,
    ib_tx_t *tx,
    void *cbdata)
{
    ib_log_error(conn->ib, "Block by close not implemented.");
    return IB_ENOTIMPL;
}

/* Plugin Structure */
ib_server_t DLL_LOCAL ibplugin = {
    IB_SERVER_HEADER_DEFAULTS,
    "ts-ironbee",
    ib_header_callback,
    NULL,
    ib_error_callback,
    NULL,
    ib_errhdr_callback,
    NULL,
    ib_errbody_callback,
    NULL,
    ib_errclose_callback,
    NULL
};

/**
 * Handle transaction context destroy.
 *
 * Handles TS_EVENT_HTTP_TXN_CLOSE (transaction close) close event from the
 * ATS.
 *
 * @param[in,out] ctx Transaction context
 */
static void ib_txn_ctx_destroy(ib_txn_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    hdr_action_t *x;
    ib_tx_t *tx = ctx->tx;
    ib_ssn_ctx *ssn = ctx->ssn;

    assert(tx != NULL);
    assert(ssn != NULL);

    ib_lock_lock(&ssn->mutex);

    ctx->tx = NULL;
    TSDebug("ironbee",
            "TX DESTROY: conn=>%p tx_count=%zd tx=%p id=%s txn_count=%d",
            tx->conn, tx->conn->tx_count, tx, tx->id, ssn->txn_count);
    tx_finish(tx);
    ib_tx_destroy(tx);

#if NO_TSIOBUFFER_POOL_CLEANUP
    if (ctx->out.output_buffer) {
        TSIOBufferDestroy(ctx->out.output_buffer);
        ctx->out.output_buffer = NULL;
    }
    if (ctx->in.output_buffer) {
        TSIOBufferDestroy(ctx->in.output_buffer);
        ctx->in.output_buffer = NULL;
    }
#endif
    while (x=ctx->hdr_actions, x != NULL) {
        ctx->hdr_actions = x->next;
        TSfree( (char *)x->hdr);
        TSfree( (char *)x->value);
        TSfree(x);
    }

    /* Decrement the txn count on the ssn, and destroy ssn if it's closing */
    ctx->ssn = NULL;

    /* If it's closing, the contp and with it the mutex are already gone.
     * Trust TS not to create more TXNs after signalling SSN close!
     */
    if (ssn->closing) {
        if (ssn->iconn) {
            tx_list_destroy(ssn->iconn);
            ib_conn_t *conn = ssn->iconn;
            ib_engine_t *ib = conn->ib;

            ssn->iconn = NULL;
            TSDebug("ironbee",
                    "ib_txn_ctx_destroy: calling ib_state_notify_conn_closed()");
            ib_state_notify_conn_closed(ib, conn);
            TSDebug("ironbee", "CONN DESTROY: conn=%p", conn);
            ib_conn_destroy(conn);
        }
        TSContDataSet(ssn->contp, NULL);
        TSContDestroy(ssn->contp);
        ib_lock_unlock(&ssn->mutex);
        ib_lock_destroy(&ssn->mutex);
        TSfree(ssn);
    }
    else {
        --(ssn->txn_count);
        ib_lock_unlock(&ssn->mutex);
    }
    TSfree(ctx);
}

/**
 * Handle session context destroy.
 *
 * Handles TS_EVENT_HTTP_SSN_CLOSE (session close) close event from the
 * ATS.
 *
 * @param[in,out] ctx session context
 */
static void ib_ssn_ctx_destroy(ib_ssn_ctx * ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* To avoid the risk of sequencing issues with this coming before TXN_CLOSE,
     * we just mark the session as closing, but leave actually closing it
     * for the TXN_CLOSE if there's a TXN
     */
    ib_lock_lock(&ctx->mutex);
    if (ctx->txn_count == 0) { /* TXN_CLOSE happened already */
        if (ctx->iconn) {
            ib_conn_t *conn = ctx->iconn;
            ctx->iconn = NULL;

            tx_list_destroy(conn);
            TSDebug("ironbee",
                    "ib_ssn_ctx_destroy: calling ib_state_notify_conn_closed()");
            ib_state_notify_conn_closed(conn->ib, conn);
            TSDebug("ironbee", "CONN DESTROY: conn=%p", conn);
            ib_conn_destroy(conn);
        }

        /* Store off the continuation pointer */
        TSCont contp = ctx->contp;
        TSContDataSet(contp, NULL);
        ctx->contp = NULL;

        /* Unlock has to come first 'cos ContDestroy destroys the mutex */
        TSContDestroy(contp);
        ib_lock_unlock(&ctx->mutex);
        ib_lock_destroy(&ctx->mutex);
        TSfree(ctx);
    }
    else {
        ctx->closing = 1;
        ib_lock_unlock(&ctx->mutex);
    }
}

/**
 * Flush buffered data in ATS.
 *
 * This doesn't actively flush data, but indicates the amount of data
 * available NOW to be consumed on the output buf.  It is up to TS
 * (or to another filter continuation module if present) whether
 * actually to consume the data at this point.
 *
 * Note: http://mail-archives.apache.org/mod_mbox/trafficserver-dev/201110.mbox/%3CCAFKFyq5gWqzi=LuxdVMTJzSbJcuZDR8hYV89khUssPxnnxbk_Q@mail.gmail.com%3E
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] ibd filter continuation ctx
 */
static void flush_data(TSCont contp, ibd_ctx *ibd)
{
    TSVConn output_conn;
    if (ibd->data->buffering != IOBUF_DISCARD
        && ibd->data->buffering != IOBUF_NOBUF) {
        if (ibd->data->output_vio == NULL) {
            /* Get the output (downstream) vconnection where we'll write data to. */
            output_conn = TSTransformOutputVConnGet(contp);
            ibd->data->output_vio = TSVConnWrite(output_conn, contp, ibd->data->output_reader, TSIOBufferReaderAvail(ibd->data->output_reader));
        }
        TSVIONBytesSet(ibd->data->output_vio, ibd->data->buffered);
        TSVIOReenable(ibd->data->output_vio);
    }
}
/**
 * Process data from ATS.
 *
 * Process data from one of the ATS events.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] ibd unknown
 */
static void process_data(TSCont contp, ibd_ctx *ibd)
{
    TSVConn output_conn;
    TSIOBuffer buf_test;
    TSVIO input_vio;
    ib_txn_ctx *data;
    int64_t towrite;
    int64_t avail;
    int first_time = 0;
    char *bufp = NULL;

    TSDebug("ironbee", "Entering process_data()");

    /* Get the write VIO for the write operation that was performed on
     * ourself. This VIO contains the buffer that we are to read from
     * as well as the continuation we are to call when the buffer is
     * empty. This is the input VIO (the write VIO for the upstream
     * vconnection).
     */
    input_vio = TSVConnWriteVIOGet(contp);

    data = TSContDataGet(contp);
    if (IB_HTTP_CODE(data->status)) {  /* We're going to an error document,
                                        * so we discard all this data
                                        */
        TSDebug("ironbee", "Status is %d, discarding", data->status);
        ibd->data->buffering = IOBUF_DISCARD;
    }

    if (!ibd->data->output_buffer) {
        first_time = 1;

        ibd->data->output_buffer = TSIOBufferCreate();
        ib_mm_register_cleanup(data->tx->mm,
                               (ib_mm_cleanup_fn_t) TSIOBufferDestroy,
                               (void*) ibd->data->output_buffer);
        ibd->data->output_reader = TSIOBufferReaderAlloc(ibd->data->output_buffer);
        TSDebug("ironbee", "\tWriting %"PRId64" bytes on VConn", TSVIONBytesGet(input_vio));

        /* Is buffering configured? */
        if (!IB_HTTP_CODE(data->status)) {
            ib_core_cfg_t *corecfg = NULL;
            ib_status_t rc;

            if (data->tx == NULL) {
                ibd->data->buffering = IOBUF_NOBUF;
            }
            else {
                rc = ib_core_context_config(ib_context_main(data->tx->ib),
                                            &corecfg);
                if (rc != IB_OK) {
                    TSError ("Error determining buffering configuration.");
                }
                else {
                    if (ibd->ibd->dir == IBD_REQ) {
                        ibd->data->buffering = (corecfg->buffer_req == 0)
                            ? IOBUF_NOBUF :
                            (corecfg->limits.request_body_buffer_limit < 0)
                                ? IOBUF_BUFFER_ALL :
                                (corecfg->limits.request_body_buffer_limit_action == IB_BUFFER_LIMIT_ACTION_FLUSH_ALL)
                                    ? IOBUF_BUFFER_FLUSHALL
                                    : IOBUF_BUFFER_FLUSHPART;
                        ibd->data->buf_limit = corecfg->limits.request_body_buffer_limit;
                        ibd->data->log_limit = corecfg->limits.request_body_log_limit;
                    }
                    else {
                        ibd->data->buffering = (corecfg->buffer_res == 0)
                            ? IOBUF_NOBUF :
                            (corecfg->limits.response_body_buffer_limit < 0)
                                ? IOBUF_BUFFER_ALL :
                                (corecfg->limits.response_body_buffer_limit_action == IB_BUFFER_LIMIT_ACTION_FLUSH_ALL)
                                    ? IOBUF_BUFFER_FLUSHALL
                                    : IOBUF_BUFFER_FLUSHPART;
                        ibd->data->buf_limit = corecfg->limits.response_body_buffer_limit;
                        ibd->data->log_limit = corecfg->limits.response_body_log_limit;
                    }
                }
            }

            /* Override buffering based on flags */
            if (ibd->data->buffering != IOBUF_NOBUF) {
                if (ibd->ibd->dir == IBD_REQ) {
                    if (ib_flags_any(data->tx->flags, IB_TX_FALLOW_ALL | IB_TX_FALLOW_REQUEST) ||
                        (!ib_flags_all(data->tx->flags, IB_TX_FINSPECT_REQBODY) &&
                         !ib_flags_all(data->tx->flags, IB_TX_FINSPECT_REQHDR)) )
                    {
                        ibd->data->buffering = IOBUF_NOBUF;
                        TSDebug("ironbee", "\tDisable request buffering");
                    }
                } else if (ibd->ibd->dir == IBD_RESP) {
                    if (ib_flags_any(data->tx->flags, IB_TX_FALLOW_ALL) ||
                        (!ib_flags_all(data->tx->flags, IB_TX_FINSPECT_RESBODY) &&
                         !ib_flags_all(data->tx->flags, IB_TX_FINSPECT_RESHDR)) )
                    {
                        ibd->data->buffering = IOBUF_NOBUF;
                        TSDebug("ironbee", "\tDisable response buffering");
                    }
                }
            }
        }

        if (ibd->data->buffering == IOBUF_NOBUF) {
            TSDebug("ironbee", "\tBuffering: off");
            /* Get the output (downstream) vconnection where we'll write data to. */
            output_conn = TSTransformOutputVConnGet(contp);
            ibd->data->output_vio = TSVConnWrite(output_conn, contp, ibd->data->output_reader, INT64_MAX);
        } else {
            TSDebug("ironbee", "\tBuffering: on, flush %d", (int)ibd->data->buffering);
        }
    }
    if (ibd->data->buf) {
        /* this is the second call to us, and we have data buffered.
         * Feed buffered data to ironbee
         */
        if (ibd->data->buflen != 0) {
            const char *buf = ibd->data->buf;
            size_t buf_length = ibd->data->buflen;
            TSDebug("ironbee",
                    "process_data: calling ib_state_notify_%s_body() %s:%d",
                    ibd->ibd->dir_label, __FILE__, __LINE__);
            (*ibd->ibd->ib_notify_body)(data->tx->ib, data->tx, buf, buf_length);
        }
        //TSfree(ibd->data->buf);
        ibd->data->buf = NULL;
        ibd->data->buflen = 0;
        if (IB_HTTP_CODE(data->status)) {  /* We're going to an error document,
                                            * so we discard all this data
                                            */
            TSDebug("ironbee", "Status is %d, discarding", data->status);
            ibd->data->buffering = IOBUF_DISCARD;
        }
    }

    /* test for input data */
    buf_test = TSVIOBufferGet(input_vio);

    if (!buf_test) {
        TSDebug("ironbee", "No more data, finishing");
        if (ibd->data->buffering != IOBUF_DISCARD) {
            if (ibd->data->output_vio == NULL) {
                /* Get the output (downstream) vconnection where we'll write data to. */
                output_conn = TSTransformOutputVConnGet(contp);
                ibd->data->output_vio = TSVConnWrite(output_conn, contp, ibd->data->output_reader, TSIOBufferReaderAvail(ibd->data->output_reader));
            }
            else {
                TSVIONBytesSet(ibd->data->output_vio, TSVIONDoneGet(input_vio));
            }
            TSVIOReenable(ibd->data->output_vio);
        }
        //ibd->data->output_buffer = NULL;
        //ibd->data->output_reader = NULL;
        //ibd->data->output_vio = NULL;
        return;
    }

    /* Determine how much data we have left to read. For this null
     * transform plugin this is also the amount of data we have left
     * to write to the output connection.
     */
    towrite = TSVIONTodoGet(input_vio);
    TSDebug("ironbee", "\ttoWrite is %" PRId64 "", towrite);

    if (towrite > 0) {
        /* The amount of data left to read needs to be truncated by
         * the amount of data actually in the read buffer.
         */

        avail = TSIOBufferReaderAvail(TSVIOReaderGet(input_vio));
        TSDebug("ironbee", "\tavail is %" PRId64 "", avail);
        if (towrite > avail) {
            towrite = avail;
        }

        if (towrite > 0) {
            int btowrite = towrite;
            /* Copy the data from the read buffer to the output buffer. */
            switch (ibd->data->buffering) {
              case IOBUF_NOBUF:
                TSIOBufferCopy(TSVIOBufferGet(ibd->data->output_vio), TSVIOReaderGet(input_vio), towrite, 0);
                break;
              case IOBUF_BUFFER_ALL:
                ibd->data->buffered += towrite;
                TSIOBufferCopy(ibd->data->output_buffer, TSVIOReaderGet(input_vio), towrite, 0);
                break;
              case IOBUF_BUFFER_FLUSHALL:
                /* Append data to buffer, flush it all if over limit */
                TSIOBufferCopy(ibd->data->output_buffer, TSVIOReaderGet(input_vio), towrite, 0);
                ibd->data->buffered += towrite;
                if (ibd->data->buffered >= ibd->data->buf_limit) {
                    flush_data(contp, ibd);
                }
                break;
              case IOBUF_BUFFER_FLUSHPART:
                /* If data will bring buffer over the limit, flush existing
                 * data before appending new data, so new data still buffered
                 */
                if (ibd->data->buffered + towrite >= ibd->data->buf_limit) {
                    flush_data(contp, ibd);
                }
                TSIOBufferCopy(ibd->data->output_buffer, TSVIOReaderGet(input_vio), towrite, 0);
                ibd->data->buffered += towrite;
                break;
              case IOBUF_DISCARD:
                break;
              default: // bug
                assert(0==1);
            }

            /* first time through, we have to buffer the data until
             * after the headers have been sent.  Ugh!
             * At this point, we know the size to alloc.
             */
            if (first_time) {
                ibd->data->buflen = towrite;
                bufp = ibd->data->buf = ib_mm_alloc(data->tx->mm,
                                                    ibd->data->buflen);
            }

            /* feed the data to ironbee, and consume them */
            while (btowrite > 0) {
                //ib_conndata_t icdata;
                int64_t ilength;
                TSIOBufferReader input_reader = TSVIOReaderGet(input_vio);
                TSIOBufferBlock blkp = TSIOBufferReaderStart(input_reader);
                const char *ibuf = TSIOBufferBlockReadStart(blkp, input_reader, &ilength);

                /* feed it to ironbee or to buffer */
                if (first_time) {
                    memcpy(bufp, ibuf, ilength);
                    bufp += ilength;
                }
                else {
                    if (ibd->data->buflen > 0) {
                        const char *buf = ibd->data->buf;
                        size_t buf_length = ibd->data->buflen;
                        TSDebug("ironbee",
                                "process_data: calling ib_state_notify_%s_body() "
                                "%s:%d",
                                ((ibd->ibd->dir == IBD_REQ)?"request":"response"),
                                __FILE__, __LINE__);
                        (*ibd->ibd->ib_notify_body)(data->tx->ib, data->tx,
                                                    (ilength!=0) ? buf : NULL, buf_length);
                    }
                    if (IB_HTTP_CODE(data->status)) {  /* We're going to an error document,
                                                        * so we discard all this data
                                                        */
                        ibd->data->buffering = IOBUF_DISCARD;
                    }
                }

                /* and mark it as all consumed */
                btowrite -= ilength;
                TSIOBufferReaderConsume(input_reader, ilength);
                TSVIONDoneSet(input_vio, TSVIONDoneGet(input_vio) + ilength);
            }
        }
    }

    /* Now we check the input VIO to see if there is data left to
     * read.
     */
    if (TSVIONTodoGet(input_vio) > 0) {
        if (towrite > 0) {
            /* If there is data left to read, then we re-enable the output
             * connection by re-enabling the output VIO. This will wake up
             * the output connection and allow it to consume data from the
             * output buffer.
             */
            if (ibd->data->buffering == IOBUF_NOBUF) {
                TSVIOReenable(ibd->data->output_vio);
            }

            /* Call back the input VIO continuation to let it know that we
             * are ready for more data.
             */
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_READY, input_vio);
        }
    }
    else {
        /* If there is no data left to read, then we modify the output
         * VIO to reflect how much data the output connection should
         * expect. This allows the output connection to know when it
         * is done reading. We then re-enable the output connection so
         * that it can consume the data we just gave it.
         */
        if (ibd->data->buffering == IOBUF_NOBUF) {
            TSVIONBytesSet(ibd->data->output_vio, TSVIONDoneGet(input_vio));
            TSVIOReenable(ibd->data->output_vio);
        }
        else if (ibd->data->buffering == IOBUF_DISCARD) {
            TSVIONBytesSet(ibd->data->output_vio, 0);
            TSVIOReenable(ibd->data->output_vio);
        }
        else {
            flush_data(contp, ibd);
        }

        /* Call back the input VIO continuation to let it know that we
         * have completed the write operation.
         */
        TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_COMPLETE, input_vio);
    }
}

/**
 * Handle a data event from ATS.
 *
 * Handles all data events from ATS, uses process_data to handle the data
 * itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] ibd unknown
 *
 * @returns status
 */
static int data_event(TSCont contp, TSEvent event, ibd_ctx *ibd)
{
    /* Check to see if the transformation has been closed by a call to
     * TSVConnClose.
     */
    ib_txn_ctx *data;
    TSDebug("ironbee", "Entering out_data for %s", ibd->ibd->dir_label);

    if (TSVConnClosedGet(contp)) {
        TSDebug("ironbee", "\tVConn is closed");
        TSContDataSet(contp, NULL);
        TSContDestroy(contp);    /* from null-transform, ???? */

        return 0;
    }
    switch (event) {
        case TS_EVENT_ERROR:
        {
            TSVIO input_vio;

            TSDebug("ironbee", "\tEvent is TS_EVENT_ERROR");
            /* Get the write VIO for the write operation that was
             * performed on ourself. This VIO contains the continuation of
             * our parent transformation. This is the input VIO.
             */
            input_vio = TSVConnWriteVIOGet(contp);

            /* Call back the write VIO continuation to let it know that we
             * have completed the write operation.
             */
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_ERROR, input_vio);
        }
        break;
        case TS_EVENT_VCONN_WRITE_COMPLETE:
            TSDebug("ironbee", "\tEvent is TS_EVENT_VCONN_WRITE_COMPLETE");
            /* When our output connection says that it has finished
             * reading all the data we've written to it then we should
             * shutdown the write portion of its connection to
             * indicate that we don't want to hear about it anymore.
             */
            TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);

            data = TSContDataGet(contp);
            TSDebug("ironbee", "data_event: calling ib_state_notify_%s_finished()", ((ibd->ibd->dir == IBD_REQ)?"request":"response"));
            (*ibd->ibd->ib_notify_end)(data->tx->ib, data->tx);
            if ( (ibd->ibd->ib_notify_post != NULL) &&
                 (!ib_flags_all(data->tx->flags, IB_TX_FPOSTPROCESS)) )
            {
                (*ibd->ibd->ib_notify_post)(data->tx->ib, data->tx);
            }
            if ( (ibd->ibd->ib_notify_log != NULL) &&
                 (!ib_flags_all(data->tx->flags, IB_TX_FLOGGING)) )
            {
                (*ibd->ibd->ib_notify_log)(data->tx->ib, data->tx);
            }
            break;
        case TS_EVENT_VCONN_WRITE_READY:
            TSDebug("ironbee", "\tEvent is TS_EVENT_VCONN_WRITE_READY");
            /* fall through */
        default:
            TSDebug("ironbee", "\t(event is %d)", event);
            /* If we get a WRITE_READY event or any other type of
             * event (sent, perhaps, because we were re-enabled) then
             * we'll attempt to transform more data.
             */
            process_data(contp, ibd);
            break;
    }

    return 0;
}

/**
 * Handle a outgoing data event from ATS.
 *
 * Handles all outgoing data events from ATS, uses process_data to handle the
 * data itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int out_data_event(TSCont contp, TSEvent event, void *edata)
{
    ib_txn_ctx *data = TSContDataGet(contp);

    if ( (data == NULL) || (data->tx == NULL) ) {
        TSDebug("ironbee", "\tout_data_event: tx == NULL");
        return 0;
    }
    /* Protect against https://issues.apache.org/jira/browse/TS-922 */
    else if (data->out.buflen == (unsigned int)-1) {
        TSDebug("ironbee", "\tout_data_event: buflen == -1");
        return 0;
    }
    ibd_ctx direction;
    direction.ibd = &ib_direction_server_resp;
    direction.data = &data->out;
    return data_event(contp, event, &direction);
}

/**
 * Handle a incoming data event from ATS.
 *
 * Handles all incoming data events from ATS, uses process_data to handle the
 * data itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int in_data_event(TSCont contp, TSEvent event, void *edata)
{
    ib_txn_ctx *data;
    TSDebug("ironbee-in-data", "in_data_event: contp=%p", contp);
    data = TSContDataGet(contp);

    if ( (data == NULL) || (data->tx == NULL) ) {
        TSDebug("ironbee", "\tin_data_event: tx == NULL");
        return 0;
    }
    else if (data->out.buflen == (unsigned int)-1) {
        TSDebug("ironbee", "\tin_data_event: buflen == -1");
        return 0;
    }
    ibd_ctx direction;
    direction.ibd = &ib_direction_client_req;
    direction.data = &data->in;
    return data_event(contp, event, &direction);
}
/**
 * Parse lines in an HTTP header buffer
 *
 * Given a buffer including "\r\n" linends, this finds the next line and its
 * length.  Where a line is wrapped, continuation lines are included in
 * in the (multi-)line parsed.
 *
 * Can now also error-correct for "\r" or "\n" line ends.
 *
 * @param[in,out] linep Buffer to parse.  On output, moved on one line.
 * @param[out] lenp Line length (excluding line end)
 * @param[in] letype What to treat as line ends
 * @return 1 if a line was parsed, 2 if parsed but with error correction,
 *         0 for a blank line (no more headers), -1 for irrecoverable error
 */
static int next_line(const char **linep, size_t *lenp, http_lineend_t letype)
{
    int rv = 1;

    size_t len = 0;
    size_t lelen = 2;
    const char *end;
    const char *line = *linep;

    switch (letype) {
      case LE_RN: /* Enforces the HTTP spec */
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        /* skip to next start-of-line from where we are */
        line = strstr(line, "\r\n");
        if (!line) {
            return -1;
        }
        line += 2;
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        /* Use a loop here to catch theoretically-unlimited numbers
         * of continuation lines in a folded header.  The isspace
         * tests for a continuation line
         */
        do {
            end = strstr(line, "\r\n");
            if (!end) {
                return -1;
            }
            lelen = 2;
            len = end - line;
        } while ( (isspace(end[lelen]) != 0) &&
                  (end[lelen] != '\r') &&
                  (end[lelen] != '\n') );
        break;
      case LE_ANY: /* Original code: take either \r or \n as lineend */

        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        else if ( ( (line[0] == '\r') || (line[0] == '\n') ) ) {
            return 0; /* blank line which is also malformed HTTP */
        }

        /* skip to next start-of-line from where we are */
        line += strcspn(line, "\r\n");
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            /* valid line end.  Set pointer to start of next line */
            line += 2;
        }
        else {   /* bogus lineend!
                  * Treat a single '\r' or '\n' as a lineend
                  */
            line += 1;
            rv = 2; /* bogus linend */
        }
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        else if ( (line[0] == '\r') || (line[0] == '\n') ) {
            return 0; /* blank line which is also malformed HTTP */
        }

        /* Use a loop here to catch theoretically-unlimited numbers
         * of continuation lines in a folded header.  The isspace
         * tests for a continuation line
         */
        do {
            if (len > 0) {
                /* we have a continuation line.  Add the lineend. */
                len += lelen;
            }
            end = line + strcspn(line + len, "\r\n");
            if ( (end[0] == '\r') && (end[1] == '\n') ) {
                lelen = 2;             /* All's well, this is a good line */
            }
            else {
                /* Malformed header.  Check for a bogus single-char lineend */
                if (end > line) {
                    lelen = 1;
                    rv = 2;
                }
                else { /* nothing at all we can interpret as lineend */
                    return -1;
                }
            }
            len = end - line;
        } while ( (isspace(end[lelen]) != 0) &&
                  (end[lelen] != '\r') &&
                  (end[lelen] != '\n') );
        break;
      case LE_N: /* \n is lineend, but either \n or \r\n is blank line */

        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        else if ( line[0] == '\n' ) {
            return 0; /* blank line which is also malformed HTTP */
        }

        /* skip to next start-of-line from where we are */
        line = strchr(line, '\n');
        if (line == NULL) {
            return -1;
        }
        ++line;
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
            return 0; /* blank line = no more hdr lines */
        }
        else if ( line[0] == '\n' ) {
            return 0; /* blank line which is also malformed HTTP */
        }

        /* Use a loop here to catch theoretically-unlimited numbers
         * of continuation lines in a folded header.  The isspace
         * tests for a continuation line
         */
        do {
            end = strchr(line, '\n');
            if (end == NULL) {
                return -1; /* there's no lineend */
            }
            /* point to the last non-lineend char and set length of lineend */
            if (end[-1] == '\r') {
                end--;
                lelen = 2;
            }
            else {
                lelen = 1;
                rv = 2;    /* we're into error-correcting */
            }
            len = end - line;
        } while ( (isspace(end[lelen]) != 0) &&
                  (end[lelen] != '\r') &&
                  (end[lelen] != '\n') );
        break;
    }

    *lenp = len;
    *linep = line;
    return rv;
}

static void header_action(TSMBuffer bufp, TSMLoc hdr_loc,
                          const hdr_action_t *act, ib_mm_t mm)
{
    TSMLoc field_loc;
    int rv;

    switch (act->action) {

    case IB_HDR_SET:  /* replace any previous instance == unset + add */
    case IB_HDR_UNSET:  /* unset it */
        TSDebug("ironbee", "Remove HTTP Header \"%s\"", act->hdr);
        /* Use a while loop in case there are multiple instances */
        while (field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, act->hdr,
                                              strlen(act->hdr)),
               field_loc != TS_NULL_MLOC) {
            TSMimeHdrFieldDestroy(bufp, hdr_loc, field_loc);
            TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        }
        if (act->action == IB_HDR_UNSET)
            break;
        /* else fallthrough to ADD */

    case IB_HDR_ADD:  /* add it in, regardless of whether it exists */
add_hdr:
        TSDebug("ironbee", "Add HTTP Header \"%s\"=\"%s\"",
                act->hdr, act->value);
        rv = TSMimeHdrFieldCreate(bufp, hdr_loc, &field_loc);
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] Failed to add MIME header field \"%s\".", act->hdr);
        }
        rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                   act->hdr, strlen(act->hdr));
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] Failed to set name of MIME header field \"%s\".",
                    act->hdr);
        }
        rv = TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1,
                                          act->value, strlen(act->value));
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] Failed to set value of MIME header field \"%s\".",
                    act->hdr);
        }
        rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] Failed to append MIME header field \"%s\".", act->hdr);
        }
        TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        break;

    case IB_HDR_MERGE:  /* append UNLESS value already appears */
        /* FIXME: implement this in full */
        /* treat this as APPEND */

    case IB_HDR_APPEND: /* append it to any existing instance */
        TSDebug("ironbee", "Merge/Append HTTP Header \"%s\"=\"%s\"",
                act->hdr, act->value);
        field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, act->hdr,
                                       strlen(act->hdr));
        if (field_loc == TS_NULL_MLOC) {
            /* this is identical to IB_HDR_ADD */
            goto add_hdr;
        }
        /* This header exists, so append to it
         * (the function is called Insert but actually appends
         */
        rv = TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, -1,
                                             act->value, strlen(act->value));
        if (rv != TS_SUCCESS) {
            TSError("[ironbee] Failed to insert MIME header field \"%s\".", act->hdr);
        }
        TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        break;

    default:  /* bug !! */
        TSDebug("ironbee", "Bogus header action %d", act->action);
        break;
    }
}

/**
 * Get the HTTP request/response buffer & line from ATS
 *
 * @param[in]  hdr_bufp Header marshal buffer
 * @param[in]  hdr_loc Header location object
 * @param[in]  mm IronBee memory manager to use for allocations
 * @param[out] phdr_buf Pointer to header buffer
 * @param[out] phdr_len Pointer to header length
 * @param[out] pline_buf Pointer to line buffer
 * @param[out] pline_len Pointer to line length
 *
 * @returns IronBee Status Code
 */
static ib_status_t get_http_header(
    TSMBuffer         hdr_bufp,
    TSMLoc            hdr_loc,
    ib_mm_t           mm,
    const char      **phdr_buf,
    size_t           *phdr_len,
    const char      **pline_buf,
    size_t           *pline_len)
{
    assert(hdr_bufp != NULL);
    assert(hdr_loc != NULL);
    assert(phdr_buf != NULL);
    assert(phdr_len != NULL);
    assert(pline_buf != NULL);
    assert(pline_len != NULL);

    ib_status_t       rc = IB_OK;
    TSIOBuffer        iobuf;
    TSIOBufferReader  reader;
    TSIOBufferBlock   block;
    char             *hdr_buf;
    size_t            hdr_len;
    size_t            hdr_off = 0;
    size_t            line_len;
    int64_t           bytes;

    iobuf = TSIOBufferCreate();
    /* reader has to be allocated *before* TSHttpHdrPrint, because
     * the latter loses all reference to blocks before the last 4K
     * in iobuf.
     */
    reader = TSIOBufferReaderAlloc(iobuf);
    TSHttpHdrPrint(hdr_bufp, hdr_loc, iobuf);

    bytes = TSIOBufferReaderAvail(reader);
    hdr_buf = ib_mm_alloc(mm, bytes + 1);
    if (hdr_buf == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }
    hdr_len = bytes;

    for (block = TSIOBufferReaderStart(reader);
         block != NULL;
         block = TSIOBufferBlockNext(block)) {
        const char *data;
        data = TSIOBufferBlockReadStart(block, reader, &bytes);
        if (bytes == 0) {
            break;
        }
        memcpy(hdr_buf + hdr_off, data, bytes);
        hdr_off += bytes;
    }
    *(hdr_buf + hdr_len) = '\0';
    ++hdr_len;

    /* Find the line end. */
    /* hack fixes RNS506, but could potentially get in trouble if
     * a malformed request contains a \r or \n lineend but no \r\n
     */
    char *line_end = strstr(hdr_buf, "\r\n");
    while (line_end == NULL && hdr_len > 2) {
        char *null = memchr(hdr_buf, 0, hdr_len);
        if (null != NULL) {
            /* remove embedded NULL and retry */
            int bytes = hdr_len - (null+1 - hdr_buf);
            memmove(null, null+1, bytes);
            hdr_len--;
            line_end = strstr(null, "\r\n");
        }
        else {
            /* There are no NULLs, and we still don't have termination */
            TSError("[ironbee] Invalid HTTP request line.");
            rc = IB_EINVAL;
            goto cleanup;
        }
    }
    line_len = (line_end - hdr_buf);

    *phdr_buf  = hdr_buf;
    *phdr_len  = strlen(hdr_buf);
    *pline_buf = hdr_buf;
    *pline_len = line_len;

cleanup:
    TSIOBufferReaderFree(reader);
    TSIOBufferDestroy(iobuf);

    return rc;
}

/**
 * Get the HTTP request URL from ATS
 *
 * @param[in]  hdr_bufp Header marshal buffer
 * @param[in]  hdr_loc Header location object
 * @param[in]  mm Memory manager
 * @param[out] purl_buf Pointer to URL buffer
 * @param[out] purl_len Pointer to URL length
 *
 * @returns IronBee Status Code
 */

static ib_status_t get_request_url(
    TSMBuffer         hdr_bufp,
    TSMLoc            hdr_loc,
    ib_mm_t           mm,
    const char      **purl_buf,
    size_t           *purl_len)
{
    ib_status_t       rc = IB_OK;
    int               rv;
    TSIOBuffer        iobuf;
    TSIOBufferReader  reader;
    TSIOBufferBlock   block;
    TSMLoc            url_loc;
    const char       *url_buf;
    int64_t           url_len;
    const char       *url_copy;

    rv = TSHttpHdrUrlGet(hdr_bufp, hdr_loc, &url_loc);
    assert(rv == TS_SUCCESS);

    iobuf = TSIOBufferCreate();
    TSUrlPrint(hdr_bufp, url_loc, iobuf);

    reader = TSIOBufferReaderAlloc(iobuf);
    block = TSIOBufferReaderStart(reader);

    TSIOBufferBlockReadAvail(block, reader);
    url_buf = TSIOBufferBlockReadStart(block, reader, &url_len);
    if (url_buf == NULL) {
        TSError("[ironbee] TSIOBufferBlockReadStart() returned NULL.");
        rc = IB_EUNKNOWN;
        goto cleanup;
    }

    /* hack fixes RNS506, but could potentially get in trouble if
     * a malformed request contains a \r or \n lineend but no \r\n
     */
#if STRICT_LINEEND
    char *line_end = strstr(url_buf, "\r\n");
#else
    char *line_end = strchr(url_buf, '\n');
#endif
    while (line_end == NULL && url_len > 1) {
        char *null = memchr(url_buf, 0, url_len);
        if (null != NULL) {
            /* remove embedded NULL and retry */
            int bytes = url_len - (null+1 - url_buf);
            memmove(null, null+1, bytes);
            url_len--;
            line_end = strstr(null, "\r\n");
        }
        else {
            /* There are no NULLs, and we still don't have termination */
            TSError("[ironbee] Invalid HTTP request line.");
            rc = IB_EINVAL;
            goto cleanup;
        }
    }

    url_copy = ib_mm_memdup(mm, url_buf, url_len);
    if (url_copy == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }
    *purl_buf  = url_copy;
    *purl_len  = url_len;

cleanup:
    TSIOBufferReaderFree(reader);
    TSIOBufferDestroy(iobuf);

    return rc;
}
/**
 * Fixup the HTTP request line from ATS if required
 *
 * @param[in]  hdr_bufp Header marshal buffer
 * @param[in]  hdr_loc Header location object
 * @param[in]  tx IronBee transaction
 * @param[in]  line_buf Line buffer
 * @param[in]  line_len Length of @a line_buf
 * @param[out] pline_buf Pointer to line buffer
 * @param[out] pline_len Pointer to line length
 *
 * @returns IronBee Status Code
 */
static ib_status_t fixup_request_line(
    TSMBuffer         hdr_bufp,
    TSMLoc            hdr_loc,
    ib_tx_t          *tx,
    const char       *line_buf,
    size_t            line_len,
    const char      **pline_buf,
    size_t           *pline_len)
{
    assert(tx != NULL);
    assert(line_buf != NULL);
    assert(pline_buf != NULL);
    assert(pline_len != NULL);

    ib_status_t          rc = IB_OK;
    static const char   *bad1_str = "http:///";
    static const size_t  bad1_len = 8;
    static const char   *bad2_str = "https:///";
    static const size_t  bad2_len = 9;
    const char          *bad_url;
    size_t               bad_len;
    const char          *url_buf;
    size_t               url_len;
    const char          *bad_line_url = NULL;
    size_t               bad_line_len;
    size_t               line_method_len; /* Includes trailing space(s) */
    size_t               line_proto_off;  /* Includes leading space(s) */
    size_t               line_proto_len;  /* Includes leading space(s) */
    char                *new_line_buf;
    char                *new_line_cur;
    size_t               new_line_len;

    /* Search for "http:///" or "https:///" in the line */
    if (line_len < bad2_len + 2) {
        goto line_ok;
    }

    /* Look for http:/// first */
    bad_line_url = ib_strstr_ex(line_buf, line_len, bad1_str, bad1_len);
    if (bad_line_url != NULL) {
        bad_url = bad1_str;
        bad_len = bad1_len;
    }
    else {
        /* Look for https:/// next */
        bad_line_url = ib_strstr_ex(line_buf, line_len, bad2_str, bad2_len);
        if (bad_line_url != NULL) {
            bad_url = bad2_str;
            bad_len = bad2_len;
        }
    }
    if (bad_line_url == NULL) {
        goto line_ok;
    }

    /* Next, check for the pattern in the URL.  We need the URL to do that. */
    rc = get_request_url(hdr_bufp, hdr_loc, tx->mm, &url_buf, &url_len);
    if (rc != IB_OK) {
        TSError("[ironbee] Error getting request URL: %s", ib_status_to_string(rc));
        return rc;
    }
    /* If the URL doesn't start with the above pattern, we're done. */
    if ( (url_len < bad_len) || (memcmp(url_buf, bad_url, bad_len) != 0) ) {
        goto line_ok;
    }

    bad_line_len = url_len;

    /*
     * Calculate the offset of the offending URL,
     * the start & length of the protocol
     */
    line_method_len = (bad_line_url - line_buf);
    line_proto_off = line_method_len + url_len;
    if (line_len < line_proto_off) {
        /* line_len was computed using our parser, which forgivingly
         * treats a lone \r or \n as line end.
         * url_len and hence line_proto_off was computed by TS, which is
         * less forgiving.  Hence a malformed line may trigger this.
         */
        TSError("[ironbee] Malformed request line.");
        return IB_EOTHER;
    }
    line_proto_len = line_len - line_proto_off;

    /* Advance the pointer into the URL buffer, shorten it... */
    url_buf += (bad_len - 1);
    url_len -= (bad_len - 1);

    /* Determine the size of the new line buffer, allocate it */
    new_line_len = line_method_len + url_len + line_proto_len;
    new_line_buf = ib_mm_alloc(tx->mm, new_line_len+1);
    if (new_line_buf == NULL) {
        TSError("[ironbee] Failed to allocate buffer for fixed request line.");
        *pline_buf = line_buf;
        *pline_len = line_len;
        return IB_EINVAL;
    }

    /* Copy into the new buffer */
    new_line_cur = new_line_buf;
    memcpy(new_line_cur, line_buf, line_method_len);
    new_line_cur += line_method_len;
    memcpy(new_line_cur, url_buf, url_len);
    new_line_cur += url_len;
    memcpy(new_line_cur, line_buf + line_proto_off, line_proto_len);

    /* Store new pointers */
    *pline_buf = new_line_buf;
    *pline_len = new_line_len;

    /* Log a message */
    if (ib_logger_level_get(ib_engine_logger_get(tx->ib)) >= IB_LOG_DEBUG) {
        TSDebug("ironbee", "Rewrote request URL from \"%.*s\" to \"%.*s\"",
                (int)bad_line_len, bad_line_url,
                (int)url_len, url_buf);
    }

    /* Done */
    return IB_OK;

line_ok:
    *pline_buf = line_buf;
    *pline_len = line_len;
    return IB_OK;

}

/**
 * Start the IronBee request
 *
 * @param[in] tx The IronBee transaction
 * @param[in] line_buf Pointer to line buffer
 * @param[in] line_len Pointer to line length
 *
 * @returns IronBee Status Code
 */
static ib_status_t start_ib_request(
    ib_tx_t     *tx,
    const char  *line_buf,
    size_t       line_len)
{
    ib_status_t           rc;
    ib_parsed_req_line_t *rline;

    rc = ib_parsed_req_line_create(&rline, tx->mm,
                                   line_buf, line_len,
                                   NULL, 0,
                                   NULL, 0,
                                   NULL, 0);

    if (rc != IB_OK) {
        TSError("[ironbee] Error creating IronBee request line: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error creating IronBee request line: %s",
                ib_status_to_string(rc));
        return rc;
    }

    TSDebug("ironbee", "calling ib_state_notify_request_started()");
    rc = ib_state_notify_request_started(tx->ib, tx, rline);
    if (rc != IB_OK) {
        TSError("[ironbee] Error notifying ironbee request start: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error notifying IronBee request start: %s",
                        ib_status_to_string(rc));
    }

    return IB_OK;
}

/**
 * Start the IronBee response
 *
 * @param[in] tx The IronBee transaction
 * @param[in] line_buf Pointer to line buffer
 * @param[in] line_len Pointer to line length
 *
 * @returns IronBee Status Code
 */
static ib_status_t start_ib_response(
    ib_tx_t     *tx,
    const char  *line_buf,
    size_t       line_len)
{
    ib_status_t            rc;
    ib_parsed_resp_line_t *rline;

    rc = ib_parsed_resp_line_create(&rline, tx->mm,
                                    line_buf, line_len,
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0);

    if (rc != IB_OK) {
        TSError("[ironbee] Error creating IronBee response line: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error creating IronBee response line: %s",
                ib_status_to_string(rc));
        return rc;
    }

    TSDebug("ironbee", "calling ib_state_notify_response_started()");
    rc = ib_state_notify_response_started(tx->ib, tx, rline);
    if (rc != IB_OK) {
        TSError("[ironbee] Error notifying IronBee response start: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error notifying IronBee response start: %s",
                        ib_status_to_string(rc));
    }

    return IB_OK;
}

/**
 * Process an HTTP header from ATS.
 *
 * Handles an HTTP header, called from ironbee_plugin.
 *
 * @param[in,out] data Transaction context
 * @param[in,out] txnp ATS transaction pointer
 * @param[in,out] ibd unknown
 * @return OK (nothing to tell), Error (something bad happened),
 *         HTTP_STATUS (check data->status).
 */
static ib_hdr_outcome process_hdr(ib_txn_ctx *data,
                                  TSHttpTxn txnp,
                                  ib_direction_data_t *ibd)
{
    int rv;
    ib_hdr_outcome ret = HDR_OK;
    TSMBuffer bufp;
    TSMLoc hdr_loc;
    hdr_action_t *act;
    hdr_action_t setact;
    const char *line, *lptr;
    size_t line_len = 0;
    const ib_site_t *site;
    ib_status_t ib_rc;
    int nhdrs = 0;
    int body_len = 0;
    ib_parsed_headers_t *ibhdrs;

    if (data->tx == NULL) {
        return HDR_OK;
    }
    TSDebug("ironbee", "process %s headers", ibd->type_label);

    /* Use alternative simpler path to get the un-doctored request
     * if we have the fix for TS-998
     *
     * This check will want expanding/fine-tuning according to what released
     * versions incorporate the fix
     */
    /* We'll get a bogus URL from TS-998 */

    rv = (*ibd->hdr_get)(txnp, &bufp, &hdr_loc);
    if (rv != 0) {
        TSError ("Failed to get %s header: %d", ibd->type_label, rv);
        ib_error_callback(data->tx, 500, NULL);
        return HDR_ERROR;
    }

    const char           *hdr_buf;
    size_t                hdr_len;
    const char           *rline_buf;
    size_t                rline_len;

    ib_rc = get_http_header(bufp, hdr_loc, data->tx->mm,
                            &hdr_buf, &hdr_len,
                            &rline_buf, &rline_len);
    if (ib_rc != IB_OK) {
        TSError("[ironbee] Failed to get %s header: %s", ibd->type_label,
                ib_status_to_string(ib_rc));
        ib_error_callback(data->tx, 500, NULL);
        ret = HDR_ERROR;
        goto process_hdr_cleanup;
    }

    /* Handle the request / response line */
    switch(ibd->dir) {
    case IBD_REQ: {
        ib_rc = fixup_request_line(bufp, hdr_loc, data->tx,
                                   rline_buf, rline_len,
                                   &rline_buf, &rline_len);
        if (ib_rc != 0) {
            TSError("[ironbee] Failed to fixup request line.");
            ib_error_callback(data->tx, 400, NULL);
            ret = HDR_ERROR;
            goto process_hdr_cleanup;
        }

        ib_rc = start_ib_request(data->tx, rline_buf, rline_len);
        if (ib_rc != IB_OK) {
            TSError("[ironbee] Error starting IronBee request: %s",
                    ib_status_to_string(ib_rc));
            ib_error_callback(data->tx, 500, NULL);
            ret = HDR_ERROR;
            goto process_hdr_cleanup;
        }
        break;
    }

    case IBD_RESP: {
        TSHttpStatus  http_status;

        ib_rc = start_ib_response(data->tx, rline_buf, rline_len);
        if (ib_rc != IB_OK) {
            TSError("[ironbee] Error starting IronBee response: %s",
                    ib_status_to_string(ib_rc));
        }

        /* A transitional response doesn't have most of what a real response
         * does, so we need to wait for the real response to go further
         * Cleanup is N/A - we haven't yet allocated anything locally!
         */
        http_status = TSHttpHdrStatusGet(bufp, hdr_loc);
        if (http_status == TS_HTTP_STATUS_CONTINUE) {
            return HDR_HTTP_100;
        }

        break;
    }

    default:
        TSError("[ironbee] Invalid direction: %d", ibd->dir);
    }


    /*
     * Parse the header into lines and feed to IronBee as parsed data
     */

    /* The buffer contains the Request line / Status line, together with
     * the actual headers.  So we'll skip the first line, which we already
     * dealt with.
     */
    rv = ib_parsed_headers_create(&ibhdrs, data->tx->mm);
    if (rv != IB_OK) {
        ib_error_callback(data->tx, 500, NULL);
        TSError("[ironbee] Failed to create ironbee header wrapper.  Disabling.");
        ret = HDR_ERROR;
        goto process_hdr_cleanup;
    }

    // get_line ensures CRLF (line_len + 2)?
    line = hdr_buf;
    /* RNS506 fix: enforce strict lineend first time round (jumping over the request/response line) */
    int l_status;

    for (l_status = next_line(&line, &line_len, LE_N);
         l_status > 0;
         l_status = next_line(&line, &line_len, LE_N)) {
        size_t n_len;
        size_t v_len;

        n_len = strcspn(line, ":");
        lptr = line + n_len + 1;
        while (isspace(*lptr) && lptr < line + line_len)
            ++lptr;
        v_len = line_len - (lptr - line);

        /* IronBee presumably wants to know of anything zero-length
         * so don't reject on those grounds!
         */
        rv = ib_parsed_headers_add(ibhdrs,
                                                line, n_len,
                                                lptr, v_len);
        if (!body_len && (ibd->dir == IBD_REQ)) {
            /* Check for expectation of a request body */
            if ((n_len == 14) && !strncasecmp(line, "Content-Length", n_len)) {
                /* lptr should contain a number.
                 * If it's positive we get normal processing, including body.
                 * If it's zero, we have a special case.
                 * If it's blank or malformed, log an error.
                 *
                 * FIXME: should we be more aggressive in malformed case?
                 * Shouldn't really be the plugin's problem.
                 */
                size_t i;
                for (i=0; i < v_len; ++i) {
                    if (isdigit(lptr[i])) {
                        body_len = 10*body_len + lptr[i] - '0';
                    }
                    else if (!isspace(lptr[i])) {
                        TSError("[ironbee] Malformed Content-Length: %.*s",
                                (int)v_len, lptr);
                        break;
                    }
                }
            }
            else if (((n_len == 17) && (v_len == 7)
                   && !strncasecmp(line, "Transfer-Encoding", n_len)
                   && !strncasecmp(lptr, "chunked", v_len))) {
                body_len = -1;  /* nonzero - body and length yet to come */
            }
        }
        if (rv != IB_OK)
            TSError("[ironbee] Failed to add header '%.*s: %.*s' to IronBee list",
                    (int)n_len, line, (int)v_len, lptr);
        ++nhdrs;
    }

    /* Notify headers if present */
    if (nhdrs > 0) {
        TSDebug("ironbee", "process_hdr: notifying header data");
        rv = (*ibd->ib_notify_header)(data->tx->ib, data->tx, ibhdrs);
        if (rv != IB_OK)
            TSError("[ironbee] Failed to notify IronBee header data event.");
        TSDebug("ironbee", "process_hdr: notifying header finished");
        rv = (*ibd->ib_notify_header_finished)(data->tx->ib, data->tx);
        if (rv != IB_OK)
            TSError("[ironbee] Failed to notify IronBee header finished event.");
    }

    /* If there are no headers, treat as a transitional response */
    else {
        TSDebug("ironbee",
                "Response has no headers!  Treating as transitional!");
        ret = HDR_HTTP_100;
        goto process_hdr_cleanup;
    }

    /* If there's no or zero-length body in a Request, notify end-of-request */
    if ((ibd->dir == IBD_REQ) && !body_len) {
        rv = (*ibd->ib_notify_end)(data->tx->ib, data->tx);
        if (rv != IB_OK)
            TSError("[ironbee] Failed to notify IronBee end of request.");
    }

    /* Initialize the header action */
    setact.action = IB_HDR_SET;
    setact.dir = ibd->dir;

    /* Add the ironbee site id to an internal header. */
    ib_rc = ib_context_site_get(data->tx->ctx, &site);
    if (ib_rc != IB_OK) {
        TSDebug("ironbee", "Error getting site for context: %s",
                ib_status_to_string(ib_rc));
        site = NULL;
    }
    if (site != NULL) {
        setact.hdr = "@IB-SITE-ID";
        setact.value = site->id;
        header_action(bufp, hdr_loc, &setact, data->tx->mm);
    }
    else {
        TSDebug("ironbee", "No site available for @IB-SITE-ID");
    }

    /* Add internal header for effective IP address */
    setact.hdr = "@IB-EFFECTIVE-IP";
    setact.value = data->tx->remote_ipstr;
    header_action(bufp, hdr_loc, &setact, data->tx->mm);

    /* Now manipulate header as requested by ironbee */
    for (act = data->hdr_actions; act != NULL; act = act->next) {
        if (act->dir != ibd->dir)
            continue;    /* it's not for us */

        TSDebug("ironbee", "Manipulating HTTP headers");
        header_action(bufp, hdr_loc, act, data->tx->mm);
    }

    /* Add internal header if we blocked the transaction */
    setact.hdr = "@IB-BLOCK-FLAG";
    if ((data->tx->flags & (IB_TX_FBLOCK_PHASE|IB_TX_FBLOCK_IMMEDIATE)) != 0) {
        setact.value = "blocked";
        header_action(bufp, hdr_loc, &setact, data->tx->mm);
    }
    else if (data->tx->flags & IB_TX_FBLOCK_ADVISORY) {
        setact.value = "advisory";
        header_action(bufp, hdr_loc, &setact, data->tx->mm);
    }

process_hdr_cleanup:
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

    /* If an error sent us to cleanup then it's in ret.  Else just
     * return whether or not IronBee has signalled an HTTP status.
     */
    return ( (ret != HDR_OK) ?
             ret :
             ((data->status == 0) ? HDR_OK : HDR_HTTP_STATUS));
}

/**
 * Initialize the IB connection.
 *
 * Initializes an IronBee connection from a ATS continuation
 *
 * @param[in] iconn IB connection
 * @param[in] ssn Session context data
 *
 * @returns status
 */
static ib_status_t ironbee_conn_init(
    ib_ssn_ctx *ssn)
{
    assert(ssn != NULL);
    const struct sockaddr *addr;
    int                    port;
    ib_conn_t             *iconn = ssn->iconn;

    /* remote ip */
    addr = TSHttpTxnClientAddrGet(ssn->txnp);
    addr2str(addr, ssn->remote_ip, &port);

    iconn->remote_ipstr = ssn->remote_ip;

    /* remote port */
    iconn->remote_port = port;

    /* local end */
    addr = TSHttpTxnIncomingAddrGet(ssn->txnp);

    addr2str(addr, ssn->local_ip, &port);

    iconn->local_ipstr = ssn->local_ip;

    /* local_port */
    iconn->local_port = port;

    return IB_OK;
}

/**
 * Plugin for the IronBee ATS.
 *
 * Handles some ATS events.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int ironbee_plugin(TSCont contp, TSEvent event, void *edata)
{
    ib_status_t rc;
    TSVConn connp;
    TSCont mycont;
    TSHttpTxn txnp = (TSHttpTxn) edata;
    TSHttpSsn ssnp = (TSHttpSsn) edata;
    ib_txn_ctx *txndata;
    ib_ssn_ctx *ssndata;
    ib_hdr_outcome status;

    TSDebug("ironbee", "Entering ironbee_plugin with %d", event);
    switch (event) {

        /* CONNECTION */
        case TS_EVENT_HTTP_SSN_START:
            /* start of connection */
            /* But we can't initialize conn stuff here, because there's
             * no API to get the connection stuff required by ironbee
             * at this point.  So instead, intercept the first TXN
             *
             * what we can and must do: create a new contp whose
             * lifetime is our ssn
             */
            mycont = TSContCreate(ironbee_plugin, TSMutexCreate());
            TSHttpSsnHookAdd (ssnp, TS_HTTP_TXN_START_HOOK, mycont);
            ssndata = TSmalloc(sizeof(*ssndata));
            memset(ssndata, 0, sizeof(*ssndata));
            /* The only failure here is EALLOC, and if that happens
             * we're ****ed anyway
             */
            rc = ib_lock_init(&ssndata->mutex);
            assert(rc == IB_OK);
            ssndata->contp = mycont;
            TSContDataSet(mycont, ssndata);

            TSHttpSsnHookAdd (ssnp, TS_HTTP_SSN_CLOSE_HOOK, mycont);

            TSHttpSsnReenable (ssnp, TS_EVENT_HTTP_CONTINUE);
            break;
        case TS_EVENT_HTTP_TXN_START:
        {
            /* start of Request */
            /* First req on a connection, we set up conn stuff */
            ib_status_t  rc;
            ib_engine_t *ib = NULL;

            ssndata = TSContDataGet(contp);
            ib_lock_lock(&ssndata->mutex);

            if (ssndata->iconn == NULL) {
                if (module_data.manager != NULL) {
                    rc = ib_manager_engine_acquire(module_data.manager, &ib);
                    if (rc == IB_DECLINED) {
                        TSError("[ironbee] Decline from engine manager");
                    }
                    else if (rc != IB_OK) {
                        TSError("[ironbee] Failed to acquire engine: %s",
                                ib_status_to_string(rc));
                    }
                }
            }

            if ( (ssndata->iconn == NULL) && (ib != NULL) ) {
                rc = ib_conn_create(ib, &ssndata->iconn, contp);
                if (rc != IB_OK) {
                    TSError("[ironbee] ib_conn_create: %s",
                            ib_status_to_string(rc));
                    ib_manager_engine_release(module_data.manager, ib);
                    return rc; // FIXME - figure out what to do
                }

                /* In the normal case, release the engine when the
                 * connection's memory pool is destroyed */
                rc = ib_mm_register_cleanup(ssndata->iconn->mm,
                                            cleanup_ib_connection,
                                            ib);
                if (rc != IB_OK) {
                    TSError("[ironbee] ib_mm_register_cleanup: %s",
                            ib_status_to_string(rc));
                    ib_manager_engine_release(module_data.manager, ib);
                    return rc; // FIXME - figure out what to do
                }

                TSDebug("ironbee", "CONN CREATE: conn=%p", ssndata->iconn);
                ssndata->txnp = txnp;
                ssndata->txn_count = ssndata->closing = 0;

                rc = ironbee_conn_init(ssndata);
                if (rc != IB_OK) {
                    TSError("[ironbee] ironbee_conn_init: %s",
                            ib_status_to_string(rc));
                    return rc; // FIXME - figure out what to do
                }

                TSContDataSet(contp, ssndata);
                TSDebug("ironbee",
                        "ironbee_plugin: ib_state_notify_conn_opened()");
                rc = ib_state_notify_conn_opened(ib, ssndata->iconn);
                if (rc != IB_OK) {
                    TSError("[ironbee] Failed to notify connection opened: %s",
                            ib_status_to_string(rc));
                }
            }
            ++ssndata->txn_count;
            ib_lock_unlock(&ssndata->mutex);

            /* create a txn cont (request ctx) */
            mycont = TSContCreate(ironbee_plugin, TSMutexCreate());
            txndata = TSmalloc(sizeof(*txndata));
            memset(txndata, 0, sizeof(*txndata));
            txndata->ssn = ssndata;
            txndata->txnp = txnp;
            TSContDataSet(mycont, txndata);

            /* With both of these, SSN_CLOSE gets called first.
             * I must be misunderstanding SSN
             * So hook it all to TXN
             */
            TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, mycont);

            /* Hook to process responses */
            TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, mycont);

            /* Hook to process requests */
            TSHttpTxnHookAdd(txnp, TS_HTTP_READ_REQUEST_HDR_HOOK, mycont);

            if (ssndata->iconn == NULL) {
                txndata->tx = NULL;
            }
            else {
                ib_tx_create(&txndata->tx, ssndata->iconn, txndata);
                TSDebug("ironbee",
                        "TX CREATE: conn=%p tx=%p id=%s txn_count=%d",
                        ssndata->iconn, txndata->tx, txndata->tx->id,
                        txndata->ssn->txn_count);
            }

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;
        }

        /* HTTP RESPONSE */
        case TS_EVENT_HTTP_READ_RESPONSE_HDR:
            txndata = TSContDataGet(contp);
            if (txndata->tx == NULL) {
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
                break;
            }

            /* Feed ironbee the headers if not done alread. */
            if (!ib_flags_all(txndata->tx->flags, IB_TX_FRES_STARTED)) {
                status = process_hdr(txndata, txnp, &ib_direction_server_resp);

                /* OK, if this was an HTTP 100 response, it's not the
                 * response we're interested in.  No headers have been
                 * sent yet, and no data will be sent until we've
                 * reached here again with the final response.
                 */
                if (status == HDR_HTTP_100) {
                    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
                    break;
                }
                // FIXME: Need to know if this fails as it (I think) means
                //        that the response did not come from the server and
                //        that ironbee should ignore it.
                /* I've not seen a fail here.  AFAICT if either the origin
                 * isn't responding or we're responding from cache. we
                 * never reach here in the first place.
                 */
                if (ib_flags_all(txndata->tx->flags, IB_TX_FRES_HEADER)) {
                    txndata->state |= HDRS_OUT;
                }
            }

            /* If ironbee signalled an error while processing request body data,
             * this is the first opportunity to divert to an errordoc
             */
            if (IB_HTTP_CODE(txndata->status)) {
                TSDebug("ironbee", "HTTP code %d contp=%p", txndata->status, contp);
                TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_ERROR);
                break;
            }

            /* hook an output filter to watch data */
            connp = TSTransformCreate(out_data_event, txnp);
            TSContDataSet(connp, txndata);
            TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

        /* hook for processing response headers */
        /* If ironbee has sent us into an error response then
         * we came here in our error path, with nonzero status
         * FIXME: tests
         */
        case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
            txndata = TSContDataGet(contp);

            txndata->state |= START_RESPONSE;

            if (txndata->status != 0) {
                error_response(txnp, txndata);
            }

            txndata->state |= START_RESPONSE;

            /* Feed ironbee the headers if not done already. */
            if (!ib_flags_all(txndata->tx->flags, IB_TX_FRES_STARTED)) {
                process_hdr(txndata, txnp, &ib_direction_client_resp);
            }

            /* If there is an error with a body, then notify ironbee.
             *
             * NOTE: I do not see anywhere else to put this as the error body is
             *       just a buffer and not delivered via normal IO channels, so
             *       the error body will never get caught by an event.
             */
            if ((txndata->status != 0) && (txndata->err_body != NULL)) {
                const char *data = txndata->err_body;
                size_t data_length = txndata->err_body_len;
                TSDebug("ironbee",
                        "error_response: calling ib_state_notify_response_body_data() %s:%d",
                        __FILE__, __LINE__);
                ib_state_notify_response_body_data(txndata->tx->ib,
                                                   txndata->tx,
                                                   data, data_length);
            }

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

            /* HTTP REQUEST */
        case TS_EVENT_HTTP_READ_REQUEST_HDR:
            txndata = TSContDataGet(contp);

            /* hook to examine output headers */
            /* Not sure why we can't do it right now, but it seems headers
             * are not yet available.
             * Can we use another case switch in this function?
             */
            //TSHttpTxnHookAdd(txnp, TS_HTTP_OS_DNS_HOOK, contp);
            TSHttpTxnHookAdd(txnp, TS_HTTP_PRE_REMAP_HOOK, contp);

            /* hook an input filter to watch data */
            connp = TSTransformCreate(in_data_event, txnp);
            TSDebug("ironbee-in-data", "TSTransformCreate contp=%p", connp);
            TSContDataSet(connp, txndata);
            TSHttpTxnHookAdd(txnp, TS_HTTP_REQUEST_TRANSFORM_HOOK, connp);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

            /* hook for processing incoming request/headers */
        case TS_EVENT_HTTP_PRE_REMAP:
        case TS_EVENT_HTTP_OS_DNS:
            txndata = TSContDataGet(contp);
            status = process_hdr(txndata, txnp, &ib_direction_client_req);
            txndata->state |= HDRS_IN;
            if (IB_HDR_OUTCOME_IS_HTTP_OR_ERROR(status, txndata)) {
                if (status == HDR_HTTP_STATUS) {
                    TSDebug("ironbee", "HTTP code %d contp=%p", txndata->status, contp);
                 }
                 else {
                    TSDebug("ironbee", "Internal error %d contp=%p", txndata->status, contp);
                    /* Ugly hack: notifications to stop modhtp bombing out */
                    if (!ib_flags_all(txndata->tx->flags, IB_TX_FREQ_STARTED) ) {
                        ib_state_notify_request_started(txndata->tx->ib, txndata->tx, NULL);
                    }
                    if (!ib_flags_all(txndata->tx->flags, IB_TX_FREQ_FINISHED) ) {
                        ib_state_notify_request_finished(txndata->tx->ib, txndata->tx);
                    }
                 }
                TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_ERROR);
            }
            else {
                /* Other nonzero statuses not supported */
                switch(status) {
                  case HDR_OK:
                    break;	/* All's well */
                  case HDR_HTTP_STATUS:
                    // FIXME: should we take the initiative here and return 500?
                    TSError("[ironbee] Internal error: ts-ironbee requested error but no error response set.");
                    break;
                  case HDR_HTTP_100:
                    /* This can't actually happen with current Trafficserver
                     * versions, as TS will generate a 400 error without
                     * reference to us.  But in case that changes in future ...
                     */
                    TSError("[ironbee] No request headers found.");
                    break;
                  default:
                    TSError("[ironbee] Unhandled state arose in handling request headers.");
                    break;
                }
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            }
            break;


            /* CLEANUP EVENTS */
        case TS_EVENT_HTTP_TXN_CLOSE:
        {
            ib_txn_ctx *ctx = TSContDataGet(contp);

            TSContDataSet(contp, NULL);
            TSContDestroy(contp);
            if ( (ctx != NULL) && (ctx->tx != NULL) ) {
                TSDebug("ironbee", "TXN Close: %p", (void *)contp);
                ib_txn_ctx_destroy(ctx);
            }
            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;
        }

        case TS_EVENT_HTTP_SSN_CLOSE:
            TSDebug("ironbee", "SSN Close: %p", (void *)contp);
            ib_ssn_ctx_destroy(TSContDataGet(contp));
            if (module_data.manager != NULL) {
                ib_manager_engine_cleanup(module_data.manager);
            }
            //TSContDestroy(contp);
            TSHttpSsnReenable(ssnp, TS_EVENT_HTTP_CONTINUE);
            break;

        case TS_EVENT_MGMT_UPDATE:
        {
            TSDebug("ironbee", "Management update");
            if (module_data.manager != NULL) {
                ib_status_t  rc;
                rc = ib_manager_engine_create(module_data.manager,
                                              module_data.config_file);
                if (rc != IB_OK) {
                    TSError("[ironbee] Error creating new engine: %s",
                            ib_status_to_string(rc));
                }
            }
            break;
        }

            /* if we get here we've got a bug */
        default:
            TSError("[ironbee] BUG: unhandled event %d in ironbee_plugin.", event);
            break;
    }

    return 0;
}

static int check_ts_version(void)
{

    const char *ts_version = TSTrafficServerVersionGet();
    int result = 0;

    if (ts_version) {
        int major_ts_version = 0;
        int minor_ts_version = 0;
        int patch_ts_version = 0;

        if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version, &patch_ts_version) != 3) {
            return 0;
        }

        /* Need at least TS 3.0 */
        if (major_ts_version >= 3) {
            result = 1;
        }

    }

    return result;
}

/**
 * Log a message to the server plugin.
 *
 * @param[in] ib_logger The IronBee logger.
 * @param[in] rec The record to use in logging.
 * @param[in] log_msg The user's log message.
 * @param[in] log_msg_sz The user's log message size.
 * @param[out] writer_record Unused. We always return IB_DECLINED.
 * @param[in] cbdata The server plugin module data used for logging.
 *
 * @returns
 * - IB_DECLINED when everything goes well.
 * - IB_OK is not returned.
 * - Other on error.
 */
static ib_status_t logger_format(
    ib_logger_t           *ib_logger,
    const ib_logger_rec_t *rec,
    const uint8_t         *log_msg,
    const size_t           log_msg_sz,
    void                  *writer_record,
    void                  *cbdata
)
{
    assert(ib_logger != NULL);
    assert(rec != NULL);
    assert(log_msg != NULL);
    assert(cbdata != NULL);

    if (cbdata == NULL) {
        return IB_DECLINED;
    }

    ibts_module_data_t *mod_data = (ibts_module_data_t *)cbdata;
    TSTextLogObject       logger = mod_data->logger;

    if (logger == NULL) {
        return IB_DECLINED;
    }
    if (log_msg == NULL || log_msg_sz == 0) {
        TSTextLogObjectFlush(logger);
    }
    else {

        ib_logger_standard_msg_t *std_msg = NULL;

        ib_status_t rc = ib_logger_standard_formatter(
            ib_logger,
            rec,
            log_msg,
            ((log_msg_sz > 255) ? 255 : log_msg_sz), /* TODO NRL Hack */
            &std_msg,
            NULL);
        if (rc != IB_OK) {
            return rc;
        }

        TSTextLogObjectWrite(
            logger,
            "%s %.*s",
            std_msg->prefix,
            (int)std_msg->msg_sz,
            (const char *)std_msg->msg);

        ib_logger_standard_msg_free(ib_logger, std_msg, cbdata);
    }

    return IB_DECLINED;
}

/**
 * Perform a flush when closing the log.
 *
 * Performs flush for IronBee ATS plugin logging.
 *
 * @param[in] logger IronBee logger. Unused.
 * @param[in] cbdata Callback data.
 */
static ib_status_t logger_close(
    ib_logger_t       *ib_logger,
    void              *cbdata)
{
    if (cbdata == NULL) {
        return IB_OK;
    }
    ibts_module_data_t *mod_data = (ibts_module_data_t *)cbdata;
    TSTextLogObject       logger = mod_data->logger;

    if (logger != NULL) {
        TSTextLogObjectFlush(logger);
    }

    return IB_OK;
}

/**
 * Handle a single log record. This is a @ref ib_logger_standard_msg_t.
 *
 * @param[in] element A @ref ib_logger_standard_msg_t holding
 *            a serialized transaction log to be written to the
 *            Traffic Server transaction log.
 * @param[in] cbdata A @ref ibts_module_data_t.
 */
static void txlog_record_element(
    void *element,
    void *cbdata
)
{
    assert(element != NULL);
    assert(cbdata != NULL);

    ib_logger_standard_msg_t *msg      = (ib_logger_standard_msg_t *)element;
    ibts_module_data_t            *mod_data = (ibts_module_data_t *)cbdata;

    /* FIXME - expand msg->msg with Traffic Server variables. */
    /* I don't understand what is TBD here! */

    if (!mod_data->txlogger) {
        /* txlogging off  */
        return;
    }

    /* write log file. */
    if (msg->msg != NULL) {
        /* In practice, this is always NULL for txlogs. */
        if (msg->prefix != NULL) {
            TSTextLogObjectWrite(mod_data->txlogger, "%s %.*s", msg->prefix,
                                 (int)msg->msg_sz, (const char *)msg->msg);
        }
        else {
            TSTextLogObjectWrite(mod_data->txlogger, "%.*s",
                                 (int)msg->msg_sz, (const char *)msg->msg);
        }
        /* FIXME: once debugged, take this out for speed */
        TSTextLogObjectFlush(mod_data->txlogger);
    }
}

/**
 * Transaction Log record handler.
 *
 * @param[in] logger The logger.
 * @param[in] writer The log writer.
 * @param[in] cbdata Callback data. @ref ibts_module_data_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on failure.
 */
static ib_status_t txlog_record(
    ib_logger_t        *logger,
    ib_logger_writer_t *writer,
    void               *cbdata
)
{
    ibts_module_data_t *mod_data = (ibts_module_data_t *)cbdata;
    assert(logger != NULL);
    assert(writer != NULL);
    assert(cbdata != NULL);

    ib_status_t    rc;

    if (!mod_data->txlogger) {
        /* txlogging off  */
        return IB_OK;
    }

    rc = ib_logger_dequeue(logger, writer, txlog_record_element, cbdata);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Register loggers to the IronBee engine.
 *
 * @param[out] manager The manager.
 * @param[in] ib The unconfigured engine.
 * @param[in] cbdata @ref ibts_module_data_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on fatal errors.
 */
static ib_status_t engine_preconfig_fn(
    ib_manager_t *manager,
    ib_engine_t  *ib,
    void         *cbdata
)
{
    assert(manager != NULL);
    assert(ib != NULL);
    assert(cbdata != NULL);

    ib_status_t           rc;
    ib_logger_format_t   *iblog_format;
    ibts_module_data_t *mod_data = (ibts_module_data_t *)cbdata;

    /* Clear all existing loggers. */
    rc = ib_logger_writer_clear(ib_engine_logger_get(ib));
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_logger_format_create(
        ib_engine_logger_get(ib),
        &iblog_format,
        logger_format,
        mod_data,
        NULL,
        NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the IronBee logger. */
    ib_logger_writer_add(
        ib_engine_logger_get(ib),
        NULL,                      /* Open. */
        NULL,                      /* Callback data. */
        logger_close,              /* Close. */
        mod_data,                  /* Callback data. */
        NULL,                      /* Reopen. */
        NULL,                      /* Callback data. */
        iblog_format,              /* Format - This does all the work. */
        NULL,                      /* Record. */
        NULL                       /* Callback data. */
    );

    return IB_OK;
}

/**
 * Register loggers to the IronBee engine.
 *
 * @param[out] manager The manager.
 * @param[in] ib The configured engine.
 * @param[in] cbdata @ref ibts_module_data_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on fatal errors.
 */
static ib_status_t engine_postconfig_fn(
    ib_manager_t *manager,
    ib_engine_t  *ib,
    void         *cbdata
)
{
    assert(manager != NULL);
    assert(ib != NULL);
    assert(cbdata != NULL);

    int rv;
    ib_status_t         rc;
    ibts_module_data_t      *mod_data = (ibts_module_data_t *)cbdata;
    ib_logger_format_t *txlog_format;

    rc = ib_logger_fetch_format(
        ib_engine_logger_get(ib),
        TXLOG_FORMAT_FN_NAME,
        &txlog_format);
    if (rc == IB_OK) {
        /* Register the IronBee Transaction Log logger. */
        ib_logger_writer_add(
            ib_engine_logger_get(ib),
            NULL,                      /* Open. */
            NULL,                      /* Callback data. */
            NULL,                      /* Close. */
            NULL,                      /* Callback data. */
            NULL,                      /* Reopen. */
            NULL,                      /* Callback data. */
            txlog_format,              /* Format - This does all the work. */
            txlog_record,              /* Record. */
            mod_data                   /* Callback data. */
        );
        /* Open logfile for txlog */
        rv = TSTextLogObjectCreate(mod_data->txlogfile,
                                   0,
                                   &mod_data->txlogger);
        if (rv != TS_SUCCESS) {
            mod_data->txlogger = NULL;
            TSError("[ironbee] Failed to create transaction log \"%s\": %d",
                    mod_data->txlogfile,
                    rv);
        } else {
            /* 300 seconds */
            TSTextLogObjectRollingIntervalSecSet(mod_data->txlogger, 300);
            /* 3:00 am */
            TSTextLogObjectRollingOffsetHrSet(mod_data->txlogger, 3);
            /* 3 = time or size */
            TSTextLogObjectRollingEnabledSet(mod_data->txlogger, 3);
        }
    }
    else {
        ib_log_notice(ib, "No transaction logger available.");
    }

    return IB_OK;
}

/**
 * Convert an IP address into a string.
 *
 * @param[in,out] addr IP address structure
 * @param[in] str Buffer in which to store the address string
 * @param[in] port Pointer to port number (also filled in)
 */
static void addr2str(const struct sockaddr *addr, char *str, int *port)
{
    char serv[8]; /* port num */
    int rv = getnameinfo(addr, sizeof(*addr), str, ADDRSIZE, serv, 8,
                         NI_NUMERICHOST|NI_NUMERICSERV);
    if (rv != 0) {
        TSError("[ironbee] getnameinfo: %d", rv);
    }
    *port = atoi(serv);
}


/* this can presumably be global since it's only setup on init */
//static ironbee_config_t ibconfig;
//#define TRACEFILE "/tmp/ironbee-trace"
#define TRACEFILE NULL

/**
 * Destroy the engine manager from the command socket or ibexit()
 *
 * @param[in] cbdata Callback data (module data)
 */
static void destroy_manager(void *cbdata)
{
    assert(cbdata != NULL);
    ibts_module_data_t *mod_data = cbdata;

    TSDebug("ironbee", "destroy_manager(): l=%p m=%p tl=%p\n",
            mod_data->logger, mod_data->manager, mod_data->txlogger);
    if (mod_data->logger != NULL) {
        TSTextLogObjectFlush(mod_data->logger);
    }
    if (mod_data->manager != NULL) {
        ib_manager_destroy(mod_data->manager);
        mod_data->manager = NULL;
    }
    if (mod_data->logger != NULL) {
        TSTextLogObjectFlush(mod_data->logger);
        TSTextLogObjectDestroy(mod_data->logger);
        mod_data->logger = NULL;
    }
    if (mod_data->txlogger != NULL) {
        TSTextLogObjectFlush(mod_data->txlogger);
        TSTextLogObjectDestroy(mod_data->txlogger);
        mod_data->txlogger = NULL;
    }
    if (mod_data->log_file != NULL) {
        free((void *)mod_data->log_file);
        mod_data->log_file = NULL;
    }

    if (mod_data->ib_initialized) {
        ib_shutdown();
        mod_data->ib_initialized = false;
    }
}

/**
 * Handle ATS shutdown for IronBee plugin.
 *
 * Registered via atexit() during initialization, destroys the IB engine,
 * etc.
 *
 */
static void ibexit(void)
{
    TSDebug("ironbee", "ibexit()");
    destroy_manager(&module_data);
    TSDebug("ironbee", "ibexit() done");
}

/**
 * Function and struct to read a TS-style argc/argv commandline into
 * a config struct.  This struct is only used for ironbee_init, and
 * serves to enable new/revised options without disrupting the API or
 * load syntax.
 *
 * @param[in,out] mod_data Module data
 * @param[in] argc Command-line argument count
 * @param[in] argv Command-line argument list
 * @return  Success/Failure parsing the config line
 */
static ib_status_t read_ibconf(
    ibts_module_data_t *mod_data,
    int            argc,
    const char    *argv[]
)
{
    int c;

    /* defaults */
    mod_data->log_level = 4;

    /* const-ness mismatch looks like an oversight, so casting should be fine */
    optind = 1;    /* Reset */
    while (c = getopt(argc, (char**)argv, "l:Lv:d:m:x:"), c != -1) {
        switch(c) {
        case 'L':
            mod_data->log_disable = true;
            break;
        case 'l':
            mod_data->log_file = strdup(optarg);
            break;
        case 'v':
            mod_data->log_level =
                ib_logger_string_to_level(optarg, IB_LOG_WARNING);
            break;
        case 'm':
            mod_data->max_engines = atoi(optarg);
            break;
        case 'x':
            mod_data->txlogfile = strdup(optarg);
            break;
        default:
            TSError("[ironbee] Unrecognised option -%c ignored.", optopt);
            break;
        }
    }

    /* Default log file */
    if (mod_data->log_file == NULL) {
        mod_data->log_file = strdup(DEFAULT_LOG);
        if (mod_data->log_file == NULL) {
            return IB_EALLOC;
        }
    }

    /* keep the config file as a non-opt argument for back-compatibility */
    if (optind == argc-1) {
        mod_data->config_file = strdup(argv[optind]);
        if (mod_data->config_file == NULL) {
            return IB_EALLOC;
        }

        TSDebug("ironbee", "Configuration file: \"%s\"", mod_data->config_file);
        return IB_OK;
    }
    else {
        TSError("[ironbee] Exactly one configuration file name required.");
        return IB_EINVAL;
    }
}
/**
 * Initialize IronBee for ATS.
 *
 * Performs IB initializations for the ATS plugin.
 *
 * @param[in] mod_data Global module data
 *
 * @returns status
 */
static int ironbee_init(ibts_module_data_t *mod_data)
{
    /* grab from httpd module's post-config */
    ib_status_t rc;
    int rv;

    if (!mod_data->log_disable) {
        assert(mod_data->logger == NULL);
        /* success is documented as TS_LOG_ERROR_NO_ERROR but that's undefined.
         * It's actually a TS_SUCCESS (proxy/InkAPI.cc line 6641).
         */
        printf("Logging to \"%s\"\n", mod_data->log_file);
        rv = TSTextLogObjectCreate(mod_data->log_file,
                                   TS_LOG_MODE_ADD_TIMESTAMP,
                                   &mod_data->logger);
        if (rv != TS_SUCCESS) {
            return IB_EUNKNOWN;
        }
    }

    /* Initialize IronBee (including util) */
    rc = ib_initialize();
    if (rc != IB_OK) {
        return rc;
    }
    mod_data->ib_initialized = true;

    /* Create the IronBee engine manager */
    TSDebug("ironbee", "Creating IronBee engine manager");
    assert(mod_data->manager == NULL);
    rc = ib_manager_create(&(mod_data->manager),   /* Engine Manager */
                           &ibplugin,              /* Server object */
                           mod_data->max_engines); /* Default max */
    if (rc != IB_OK) {
        TSError("[ironbee] Error creating IronBee engine manager: %s",
                ib_status_to_string(rc));
        return rc;
    }

    rc = ib_manager_engine_preconfig_fn_add(
        mod_data->manager,
        engine_preconfig_fn,
        mod_data);
    if (rc != IB_OK) {
        TSError("[ironbee] Error registering server preconfig function: %s",
                ib_status_to_string(rc));
        return rc;
    }

    rc = ib_manager_engine_postconfig_fn_add(
        mod_data->manager,
        engine_postconfig_fn,
        mod_data);
    if (rc != IB_OK) {
        TSError("[ironbee] Error registering server postconfig function: %s",
                ib_status_to_string(rc));
        return rc;
    }

    /* Create the initial engine */
    TSDebug("ironbee", "Creating initial IronBee engine");
    rc = ib_manager_engine_create(mod_data->manager, mod_data->config_file);
    if (rc != IB_OK) {
        TSError("[ironbee] Error creating initial IronBee engine: %s",
                ib_status_to_string(rc));
        return rc;
    }

    /* Register our at exit function */
    rv = atexit(ibexit);
    if (rv != 0) {
        TSError("[ironbee] Error registering IronBee exit handler: %s", strerror(rv));
        return IB_EOTHER;
    }

    TSDebug("ironbee", "IronBee Ready");
    return rc;
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
    TSPluginRegistrationInfo info;
    TSCont cont;
    ib_status_t rc;

    /* FIXME - check why these are char*, not const char* */
    info.plugin_name = (char *)"ironbee";
    info.vendor_name = (char *)"Qualys, Inc";
    info.support_email = (char *)"ironbee-users@lists.sourceforge.com";

    if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
        TSError("[ironbee] Plugin registration failed.");
        goto Lerror;
    }

    if (!check_ts_version()) {
        TSError("[ironbee] Plugin requires Traffic Server 3.0 or later");
        goto Lerror;
    }

    rc = read_ibconf(&module_data, argc, argv);
    if (rc != IB_OK) {
        /* we already logged the error */
        goto Lerror;
    }

    rc = ironbee_init(&module_data);
    if (rc != IB_OK) {
        TSError("[ironbee] initialization failed: %s",
                ib_status_to_string(rc));
        goto Lerror;
    }

    cont = TSContCreate(ironbee_plugin, TSMutexCreate());
    if (cont == NULL) {
        TSError("[ironbee] failed to create initial continuation!");
        goto Lerror;
    }
    TSContDataSet(cont, NULL);

    /* connection initialization & cleanup */
    TSHttpHookAdd(TS_HTTP_SSN_START_HOOK, cont);

    /* Register our continuation for management update for traffic_line -x
     * Note that this requires Trafficserver 3.3.5 or later, or else
     * apply the patch from bug TS-2036
     */
    TSMgmtUpdateRegister(cont, "ironbee");

    return;

Lerror:
    TSError("[ironbee] Unable to initialize plugin (disabled).");
}
