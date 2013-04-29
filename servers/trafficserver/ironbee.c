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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>
#include <ts/ts.h>

#include <sys/socket.h>
#include <netdb.h>

// This gets the PRI*64 types
# define __STDC_FORMAT_MACROS 1
# include <inttypes.h>

#include <ironbee/engine.h>
#include <ironbee/engine_manager.h>
#include <ironbee/config.h>
#include <ironbee/module.h> /* Only needed while config is in here. */
#include <ironbee/provider.h>
#include <ironbee/server.h>
#include <ironbee/core.h>
#include <ironbee/site.h>
#include <ironbee/state_notify.h>
#include <ironbee/util.h>
#include <ironbee/regex.h>
#include <ironbee/string.h>
static void addr2str(const struct sockaddr *addr, char *str, int *port);

#define ADDRSIZE 48 /* what's the longest IPV6 addr ? */

typedef struct {
    TSTextLogObject  logger;
    ib_manager_t    *manager;
} module_data_t;
static module_data_t module_data = { NULL, NULL };
#define DEFAULT_LOG "ts-ironbee"

typedef enum {
    HDR_OK,
    HDR_ERROR,
    HDR_HTTP_100,
    HDR_HTTP_STATUS
} ib_hdr_outcome;
#define IB_HDR_OUTCOME_IS_HTTP(outcome, data) \
    ((outcome) == HDR_HTTP_STATUS && (data)->status >= 200 && (data)->status < 600)
#define IB_HTTP_CODE(num) ((num) >= 200 && (num) < 600)

typedef struct tx_list {
    ib_tx_t *tx;
    struct tx_list *next;
} tx_list;

static tx_list *tx_list_add(tx_list *list, ib_tx_t *tx)
{
    tx_list *ret = TSmalloc(sizeof(tx_list));
    ret->tx = tx;
    ret->next = list;
    return ret;
}
static void tx_list_destroy(tx_list *list)
{
    if (list != NULL) {
        tx_list_destroy(list->next);
        ib_tx_destroy(list->tx);
        TSfree(list);
    }
}

typedef struct {
    ib_conn_t *iconn;
    /* store the IPs here so we can clean them up and not leak memory */
    char remote_ip[ADDRSIZE];
    char local_ip[ADDRSIZE];
    TSHttpTxn txnp; /* hack: conn data requires txnp to access */
    /* Keep track of whether this is open and has active transactions */
    int txn_count;
    int closing;
    TSMutex mutex;
    /* include the contp, so we can delay destroying it from the event */
    TSCont contp;
    /* and save ib tx structs here, to delay destroying them until the
     * session closes
     */
    tx_list *txns;
} ib_ssn_ctx;

typedef struct {
    /* data filtering stuff */
    TSVIO output_vio;
    TSIOBuffer output_buffer;
    TSIOBufferReader output_reader;
    char *buf;
    unsigned int buflen;
    /* Nobuf - no buffering
     * Discard - transmission aborted, discard remaining data
     * buffer - buffer everything until EOS or abortedby error
     */
    enum { IOBUF_NOBUF, IOBUF_DISCARD, IOBUF_BUFFER } buffering;
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
    ib_rx_t *rx;
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
    ib_engine_t *engine;
    ib_ssn_ctx *ssn;
    ib_tx_t *tx;
    TSHttpTxn txnp;
    ib_filter_ctx in;
    ib_filter_ctx out;
    int state;
    int status;
    hdr_action_t *hdr_actions;
    hdr_list *err_hdrs;
    char *err_body;    /* this one can't be const */
} ib_txn_ctx;

typedef struct {
    ib_server_direction_t dir;

    const char *type_label;
    const char *dir_label;
    TSReturnCode (*hdr_get)(TSHttpTxn, TSMBuffer *, TSMLoc *);

    ib_status_t (*ib_notify_header)(ib_engine_t*, ib_tx_t*,
                 ib_parsed_header_wrapper_t*);
    ib_status_t (*ib_notify_header_finished)(ib_engine_t*, ib_tx_t*);
    ib_status_t (*ib_notify_body)(ib_engine_t*, ib_tx_t*, ib_txdata_t*);
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
    ib_direction_data_t *ibd;
    ib_filter_ctx *data;
} ibd_ctx;


static bool is_error_status(int status)
{
    return ( (status >= 200) && (status < 600) );
}

/**
 * Callback functions for Ironbee to signal to us
 */
static ib_status_t ib_header_callback(
    ib_tx_t *tx,
    ib_server_direction_t dir,
    ib_server_header_action_t action,
    const char *hdr,
    const char *value,
    ib_rx_t *rx,
    void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_action_t *header;
    /* Logic for whether we're in time for the requested action */
    /* Output headers can change any time before they're sent */
    /* Input headers can only be touched during their read */

    if (ctx->state & HDRS_OUT ||
        (ctx->state & HDRS_IN && dir == IB_SERVER_REQUEST))
        return IB_ENOTIMPL;  /* too late for requested op */

    if (action == IB_HDR_EDIT) {
        if (rx == NULL) {
            rx = ib_rx_compile(tx->mp, value);
            if (rx == NULL) {
                TSError("Failed to parse '%s' as a regexp", value);
                return IB_EINVAL;
            }
        }
    }

    header = TSmalloc(sizeof(*header));
    header->next = ctx->hdr_actions;
    ctx->hdr_actions = header;
    header->dir = dir;
    /* FIXME: deferring merge support - implementing append instead */
    header->action = action = action == IB_HDR_MERGE ? IB_HDR_APPEND : action;
    header->hdr = TSstrdup(hdr);
    header->value = TSstrdup(value);
    header->rx = rx;

    return IB_OK;
}

/**
 * Handler function to generate an error response
 */
static void error_response(
    TSHttpTxn txnp,
    ib_txn_ctx *txndata)
{
    const char *reason = TSHttpHdrReasonLookup(txndata->status);
    TSMBuffer bufp;
    TSMLoc hdr_loc;
    TSMLoc field_loc;
    hdr_list *hdrs;
    TSReturnCode rv;

    if (TSHttpTxnClientRespGet(txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
        TSError("Errordoc: couldn't retrieve client response header");
        TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
        return;
    }
    rv = TSHttpHdrStatusSet(bufp, hdr_loc, txndata->status);
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHttpHdrStatusSet");
    }
    if (reason == NULL) {
        reason = "Other";
    }
    rv = TSHttpHdrReasonSet(bufp, hdr_loc, reason, strlen(reason));
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHttpHdrReasonSet");
    }

    while (hdrs = txndata->err_hdrs, hdrs != 0) {
        txndata->err_hdrs = hdrs->next;
        rv = TSMimeHdrFieldCreate(bufp, hdr_loc, &field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldCreate");
            goto errordoc_free;
        }
        rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                   hdrs->hdr, strlen(hdrs->hdr));
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldNameSet");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, -1,
                                        hdrs->value, strlen(hdrs->value));
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldValueStringInsert");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldAppend");
            goto errordoc_free1;
        }

errordoc_free1:
        rv = TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSHandleMLocRelease 1");
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
                              strlen(txndata->err_body), NULL);
    }
    rv = TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHandleMLocRelease 2");
    }

    TSDebug("ironbee", "Sent error %d \"%s\"", txndata->status, reason);
}

static ib_status_t ib_error_callback(
    ib_tx_t *tx,
    int status,
    void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    TSDebug("ironbee", "ib_error_callback with status=%d", status);
    if ( is_error_status(status) ) {
        if (is_error_status(ctx->status) ) {
            TSDebug("ironbee",
                    "  Ignoring: status already set to %d", ctx->status);
            return IB_OK;
        }
        /* We can't return an error after the response has started */
        if (ctx->state & START_RESPONSE) {
            TSDebug("ironbee", "Too late to change status=%d", status);
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

static ib_status_t ib_errhdr_callback(ib_tx_t *tx, const char *hdr, const char *val, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_list *hdrs;
    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE)
        return IB_DECLINED;
    if (!hdr || !val)
        return IB_EINVAL;
    hdrs = TSmalloc(sizeof(*hdrs));
    hdrs->hdr = TSstrdup(hdr);
    hdrs->value = TSstrdup(val);
    hdrs->next = ctx->err_hdrs;
    ctx->err_hdrs = hdrs;
    return IB_OK;
}

static ib_status_t ib_errdata_callback(ib_tx_t *tx, const char *data, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    /* Handle No Data as zero length data. */
    if (data == NULL) {
        return IB_OK;
    }

    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE)
        return IB_DECLINED;

    ctx->err_body = TSstrdup(data);
    return IB_OK;
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
    ib_errdata_callback,
    NULL,
};

/**
 * Handle transaction context destroy.
 *
 * Handles TS_EVENT_HTTP_TXN_CLOSE (transaction close) close event from the
 * ATS.
 *
 * @param[in,out] data Transaction context data
 */
static void ib_txn_ctx_destroy(ib_txn_ctx * data)
{
    if (data) {
        hdr_action_t *x;
//        TSDebug("ironbee", "TX DESTROY: conn=>%p tx=%p id=%s txn_count=%d", data->tx->conn, data->tx, data->tx->id, data->ssn->txn_count);
//        ib_tx_destroy(data->tx);
//        data->tx = NULL;
        /* For reasons unknown, we can't destroy the tx here.
         * Instead, save it on the ssn rec to destroy when that closes.
         */
        data->ssn->txns = tx_list_add(data->ssn->txns, data->tx);
        if (data->out.output_buffer) {
            TSIOBufferDestroy(data->out.output_buffer);
            data->out.output_buffer = NULL;
        }
        if (data->in.output_buffer) {
            TSIOBufferDestroy(data->in.output_buffer);
            data->in.output_buffer = NULL;
        }
        while (x=data->hdr_actions, x != NULL) {
            data->hdr_actions = x->next;
            TSfree( (char *)x->hdr);
            TSfree( (char *)x->value);
            TSfree(x);
        }
        /* Decrement the txn count on the ssn, and destroy ssn if it's closing */
        if (data->ssn) {
            /* If it's closing, the contp and with it the mutex are already gone.
             * Trust TS not to create more TXNs after signalling SSN close!
             */
            if (data->ssn->closing) {
                tx_list_destroy(data->ssn->txns);
                if (data->ssn->iconn) {
                    TSDebug("ironbee", "ib_txn_ctx_destroy: calling ib_state_notify_conn_closed()");
                    ib_state_notify_conn_closed(data->engine, data->ssn->iconn);
                    TSDebug("ironbee", "CONN DESTROY: conn=%p", data->ssn->iconn);
                    ib_conn_destroy(data->ssn->iconn);
                }
                TSContDestroy(data->ssn->contp);
                TSfree(data->ssn);
            }
            else {
                TSMutexLock(data->ssn->mutex);
                --data->ssn->txn_count;
                TSMutexUnlock(data->ssn->mutex);
            }
        }
        TSfree(data);
    }
}

/**
 * Handle session context destroy.
 *
 * Handles TS_EVENT_HTTP_SSN_CLOSE (session close) close event from the
 * ATS.
 *
 * @param[in,out] data session context data
 */
static void ib_ssn_ctx_destroy(ib_ssn_ctx * data)
{
    /* To avoid the risk of sequencing issues with this coming before TXN_CLOSE,
     * we just mark the session as closing, but leave actually closing it
     * for the TXN_CLOSE if there's a TXN
     */
    if (data) {
        TSMutexLock(data->mutex);
        if (data->txn_count == 0) { /* TXN_CLOSE happened already */
            tx_list_destroy(data->txns);
            if (data->iconn) {
                TSDebug("ironbee", "ib_ssn_ctx_destroy: calling ib_state_notify_conn_closed()");
                ib_state_notify_conn_closed(data->iconn->ib, data->iconn);
                TSDebug("ironbee", "CONN DESTROY: conn=%p", data->iconn);
                ib_conn_destroy(data->iconn);
            }
            /* Unlock has to come first 'cos ContDestroy destroys the mutex */
            TSMutexUnlock(data->mutex);
            TSContDestroy(data->contp);
            TSfree(data);
        }
        else {
            data->closing = 1;
            TSMutexUnlock(data->mutex);
        }
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
static void process_data(TSCont contp, ibd_ctx* ibd)
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
        ibd->data->output_reader = TSIOBufferReaderAlloc(ibd->data->output_buffer);
        TSDebug("ironbee", "\tWriting %"PRId64" bytes on VConn", TSVIONBytesGet(input_vio));

        /* Is buffering configured? */
        if (!IB_HTTP_CODE(data->status)) {
            ib_core_cfg_t *corecfg = NULL;
            ib_status_t rc;

            rc = ib_core_context_config(ib_context_main(data->engine),
                                        &corecfg);
            if (rc != IB_OK) {
                TSError ("Error determining buffering configuration");
            }
            else {
                ibd->data->buffering = (((ibd->ibd->dir == IBD_REQ)
                                         ? corecfg->buffer_req
                                         : corecfg->buffer_res) == 0)
                                           ? IOBUF_NOBUF : IOBUF_BUFFER;
            }

            /* Override buffering based on flags */
            if (ibd->data->buffering == IOBUF_BUFFER) {
                if (ibd->ibd->dir == IBD_REQ) {
                    if (!ib_tx_flags_isset(data->tx, IB_TX_FINSPECT_REQBODY) &&
                        !ib_tx_flags_isset(data->tx, IB_TX_FINSPECT_REQHDR)) {
                        ibd->data->buffering = IOBUF_NOBUF;
                        TSDebug("ironbee", "\tDisable request buffering");
                    }
                } else if (ibd->ibd->dir == IBD_RESP) {
                    if (!ib_tx_flags_isset(data->tx, IB_TX_FINSPECT_RSPBODY) &&
                        !ib_tx_flags_isset(data->tx, IB_TX_FINSPECT_RSPHDR)) {
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
            TSDebug("ironbee", "\tBuffering: on");
        }
    }
    if (ibd->data->buf) {
        /* this is the second call to us, and we have data buffered.
         * Feed buffered data to ironbee
         */
        ib_txdata_t itxdata;
        itxdata.data = (uint8_t *)ibd->data->buf;
        itxdata.dlen = ibd->data->buflen;
        TSDebug("ironbee",
                "process_data: calling ib_state_notify_%s_body() %s:%d",
                ibd->ibd->dir_label, __FILE__, __LINE__);
        (*ibd->ibd->ib_notify_body)(data->tx->ib, data->tx, &itxdata);
        TSfree(ibd->data->buf);
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
            if (ibd->data->buffering == IOBUF_NOBUF) {
                TSIOBufferCopy(TSVIOBufferGet(ibd->data->output_vio), TSVIOReaderGet(input_vio), towrite, 0);
            }
            else if (ibd->data->buffering != IOBUF_DISCARD) {
                TSIOBufferCopy(ibd->data->output_buffer, TSVIOReaderGet(input_vio), towrite, 0);
            }

            /* first time through, we have to buffer the data until
             * after the headers have been sent.  Ugh!
             * At this point, we know the size to alloc.
             */
            if (first_time) {
                bufp = ibd->data->buf = TSmalloc(towrite);
                ibd->data->buflen = towrite;
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
                    ib_txdata_t itxdata;
                    itxdata.data = (uint8_t *)ibd->data->buf;
                    itxdata.dlen = ibd->data->buflen;
                    TSDebug("ironbee", "process_data: calling ib_state_notify_%s_body() %s:%d", ((ibd->ibd->dir == IBD_REQ)?"request":"response"), __FILE__, __LINE__);
                    (*ibd->ibd->ib_notify_body)(data->tx->ib, data->tx,
                                                (ilength!=0) ? &itxdata : NULL);
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
        //TSVIONBytesSet(ibd->data->output_vio, TSVIONDoneGet(input_vio));
        //TSVIOReenable(ibd->data->output_vio);

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
    TSDebug("ironbee", "Entering out_data for %s\n", ibd->ibd->dir_label);

    if (TSVConnClosedGet(contp)) {
        TSDebug("ironbee", "\tVConn is closed");
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
                 (!ib_tx_flags_isset(data->tx, IB_TX_FPOSTPROCESS)) )
            {
                (*ibd->ibd->ib_notify_post)(data->tx->ib, data->tx);
            }
            if ( (ibd->ibd->ib_notify_log != NULL) &&
                 (!ib_tx_flags_isset(data->tx, IB_TX_FLOGGING)) )
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
    if (data->out.buflen == (unsigned int)-1) {
        TSDebug("ironbee", "\tout_data_event: buflen = -1");
        //ib_log_debug(ironbee, 9, "ironbee/out_data_event(): buflen = -1");
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
    ib_txn_ctx *data = TSContDataGet(contp);
    if (data->out.buflen == (unsigned int)-1) {
        TSDebug("ironbee", "\tin_data_event: buflen = -1");
        //ib_log_debug(ironbee, 9, "ironbee/in_data_event(): buflen = -1");
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
 * @param[in] line Buffer to parse
 * @param[out] lenp Line length (excluding line end)
 * @return 1 if a line was parsed, 2 if parsed but with error correction,
 *         0 for a blank line (no more headers), -1 for irrecoverable error
 */
static int next_line(const char **linep, size_t *lenp)
{
    int rv = 1;

    size_t len = 0;
    size_t lelen = 2;
    const char *end;
    const char *line = *linep;

    if ( (line[0] == '\r') && (line[1] == '\n') ) {
        return 0; /* blank line = no more hdr lines */
    }
    else if ( (line[0] == '\r') || (line[0] == '\n') ) {
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
        if ( (line[0] == '\r') && (line[1] == '\n') ) {
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

    *lenp = len;
    *linep = line;
    return rv;
}

static void header_action(TSMBuffer bufp, TSMLoc hdr_loc,
                          const hdr_action_t *act, ib_mpool_t *pool)
{
    TSMLoc field_loc;
    int rv;
    char *oldval;
    char *newval;
    int len;
    int nmatch;

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
            TSError("Failed to add MIME header field \"%s\"", act->hdr);
        }
        rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                   act->hdr, strlen(act->hdr));
        if (rv != TS_SUCCESS) {
            TSError("Failed to set name of MIME header field \"%s\"",
                    act->hdr);
        }
        rv = TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1,
                                          act->value, strlen(act->value));
        if (rv != TS_SUCCESS) {
            TSError("Failed to set value of MIME header field \"%s\"",
                    act->hdr);
        }
        rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("Failed to append MIME header field \"%s\"", act->hdr);
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
            TSError("Failed to insert MIME header field \"%s\"", act->hdr);
        }
        TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        break;

    case IB_HDR_EDIT: /* apply regexp */
        TSDebug("ironbee", "Apply regexp %s to %s",
                act->hdr, act->value);
        field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, act->hdr,
                                       strlen(act->hdr));
        if (field_loc == TS_NULL_MLOC) {
            TSDebug("ironbee", "No %s header found", act->hdr);
            break;
        }
        /* Get the current value of the header */
        oldval = (char*) TSMimeHdrFieldValueStringGet(bufp, hdr_loc, field_loc,
                                                      0, &len);
        /* We need a string for rx */
        oldval = TSstrndup(oldval, len);
        nmatch = ib_rx_exec(pool, act->rx, oldval, &newval, NULL);
        TSfree(oldval);

        /* nmatch is positive iff the regex substitution changed anything */
        if (nmatch > 0) {
            TSDebug("ironbee", "Transformed to '%s'", newval);
            if (TSMimeHdrFieldValuesClear(bufp, hdr_loc, field_loc)
                    != TS_SUCCESS) {
                TSError("Failed to clear header\n");
            }
            if (TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, 0,
                                                newval, strlen(newval))
                    != TS_SUCCESS) {
                TSError("Failed to update header\n");
            }
        }

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
 * @param[in]  mp IronBee memory pool to use for allocations
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
    ib_mpool_t       *mp,
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
    TSHttpHdrPrint(hdr_bufp, hdr_loc, iobuf);

    reader = TSIOBufferReaderAlloc(iobuf);
    bytes = TSIOBufferReaderAvail(reader);
    hdr_buf = ib_mpool_alloc(mp, bytes + 1);
    if (hdr_buf == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }
    hdr_len = bytes;

    block = TSIOBufferReaderStart(reader);
    while(block != NULL) {
        const char *data;
        data = TSIOBufferBlockReadStart(block, reader, &bytes);
        if (bytes == 0) {
            break;
        }
        memcpy(hdr_buf + hdr_off, data, bytes);
        hdr_off += bytes;

        /* Consume the data so that we get to the next block */
        TSIOBufferReaderConsume(reader, bytes);
        block = TSIOBufferReaderStart(reader);
    }
    *(hdr_buf + hdr_len) = '\0';
    ++hdr_len;

    /* Find the line end. */
    line_len = strcspn(hdr_buf, "\r\n");
    if ( (line_len == 0) || (line_len > hdr_len) ) {
        TSError("Invalid HTTP request line");
        rc = IB_EINVAL;
        goto cleanup;
    }
    line_len += strspn(hdr_buf, "\r\n");

    *phdr_buf  = hdr_buf;
    *phdr_len  = hdr_len;
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
 * @param[out] purl_buf Pointer to URL buffer
 * @param[out] purl_len Pointer to URL length
 *
 * @returns IronBee Status Code
 */
static ib_status_t get_request_url(
    TSMBuffer         hdr_bufp,
    TSMLoc            hdr_loc,
    ib_mpool_t       *mp,
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
        TSError("TSIOBufferBlockReadStart() returned NULL");
        rc = IB_EUNKNOWN;
        goto cleanup;
    }

    url_copy = ib_mpool_memdup(mp, url_buf, url_len);
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
 * @param[in]  mp IronBee memory pool to use for allocations
 * @param[in]  hdr_buf Header buffer
 * @param[in]  hdr_len Header buffer length
 * @param[out] pline_buf Pointer to line buffer
 * @param[out] pline_len Pointer to line length
 *
 * @returns IronBee Status Code
 */
static ib_status_t fixup_request_line(
    TSMBuffer         hdr_bufp,
    TSMLoc            hdr_loc,
    ib_mpool_t       *mp,
    const char       *line_buf,
    size_t            line_len,
    const char      **pline_buf,
    size_t           *pline_len)
{
    assert(mp != NULL);
    assert(line_buf != NULL);
    assert(pline_buf != NULL);
    assert(pline_len != NULL);

    ib_status_t          rc = IB_OK;
    static const char   *bad = "http:///";
    static const size_t  bad_len = 8;
    const char          *url_buf;
    size_t               url_len;
    const char          *bad_line_url = NULL;
    size_t               line_method_len; /* Includes trailing space(s) */
    size_t               line_proto_off;  /* Includes leading space(s) */
    size_t               line_proto_len;  /* Includes leading space(s) */
    char                *new_line_buf;
    char                *new_line_cur;
    size_t               new_line_len;

    /* Search for "http:///" in the line; if it's not present, we're done */
    if (line_len >= 8) {
        bad_line_url = ib_strstr_ex(line_buf, line_len, bad, bad_len);
    }
    if ( (line_len < 8) || (bad_line_url == NULL) ) {
        goto line_ok;
    }

    /* Next, check for "http:///" in the URL.  First, get the URL. */
    rc = get_request_url(hdr_bufp, hdr_loc, mp, &url_buf, &url_len);
    if (rc != IB_OK) {
        TSError("Failed to get request URL: %s", ib_status_to_string(rc));
        return rc;
    }

    /* If the URL doesn't start with "http:///", we're done. */
    if ( (url_len < 8) || (memcmp(url_buf, bad, bad_len) != 0) ) {
        goto line_ok;
    }

    /*
     * Calculate the offset of the offending URL,
     * the start & length of the protocol
     */
    line_method_len = (bad_line_url - line_buf);
    line_proto_off = line_method_len + url_len;
    line_proto_len = line_len - line_proto_off;

    /* Advance the pointer into the URL buffer, shorten it... */
    url_buf += (bad_len - 1);
    url_len -= (bad_len - 1);

    /* Determine the size of the new line buffer, allocate it */
    new_line_len = line_method_len + url_len + line_proto_len;
    new_line_buf = ib_mpool_alloc(mp, new_line_len);
    if (new_line_buf == NULL) {
        TSError("Failed to allocate buffer for fixed request line!!");
        *pline_buf = line_buf;
        *pline_len = line_len;
        return IB_EINVAL;
    }

    /* Copy into the new buffer */
    new_line_cur = new_line_buf;
    strncpy(new_line_cur, line_buf, line_method_len);
    new_line_cur += line_method_len;
    strncpy(new_line_cur, url_buf, url_len);
    new_line_cur += url_len;
    strncpy(new_line_cur, line_buf + line_proto_off, line_proto_len);

    /* Store new pointers */
    *pline_buf = new_line_buf;
    *pline_len = new_line_len;

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

    rc = ib_parsed_req_line_create(tx, &rline,
                                   line_buf, line_len,
                                   NULL, 0,
                                   NULL, 0,
                                   NULL, 0);

    if (rc != IB_OK) {
        TSError("Error creating IronBee request line: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error creating IronBee request line: %s",
                ib_status_to_string(rc));
        return rc;
    }

    TSDebug("ironbee", "calling ib_state_notify_request_started()");
    rc = ib_state_notify_request_started(tx->ib, tx, rline);
    if (rc != IB_OK) {
        TSError("Error notifying ironbee request start: %s",
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

    rc = ib_parsed_resp_line_create(tx, &rline,
                                    line_buf, line_len,
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0);

    if (rc != IB_OK) {
        TSError("Error creating IronBee response line: %s",
                ib_status_to_string(rc));
        ib_log_error_tx(tx, "Error creating IronBee response line: %s",
                ib_status_to_string(rc));
        return rc;
    }

    TSDebug("ironbee", "calling ib_state_notify_response_started()");
    rc = ib_state_notify_response_started(tx->ib, tx, rline);
    if (rc != IB_OK) {
        TSError("Error notifying IronBee response start: %s",
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
    bool has_body = false;
    ib_parsed_header_wrapper_t *ibhdrs;

    TSDebug("ironbee", "process %s headers\n", ibd->type_label);

    /* Use alternative simpler path to get the un-doctored request
     * if we have the fix for TS-998
     *
     * This check will want expanding/fine-tuning according to what released
     * versions incorporate the fix
     */
    /* We'll get a bogus URL from TS-998 */

    rv = (*ibd->hdr_get)(txnp, &bufp, &hdr_loc);
    if (rv != 0) {
        TSError ("couldn't retrieve %s header: %d\n", ibd->type_label, rv);
        return HDR_ERROR;
    }

    const char           *hdr_buf;
    size_t                hdr_len;
    const char           *rline_buf;
    size_t                rline_len;

    ib_rc = get_http_header(bufp, hdr_loc, data->tx->mp,
                            &hdr_buf, &hdr_len,
                            &rline_buf, &rline_len);
    if (ib_rc != IB_OK) {
        TSError ("couldn't get %s header: %s\n", ibd->type_label,
                 ib_status_to_string(ib_rc));
        return HDR_ERROR;
    }

    /* Handle the request / response line */
    switch(ibd->dir) {
    case IBD_REQ: {
        ib_rc = fixup_request_line(bufp, hdr_loc, data->tx->mp,
                                   rline_buf, rline_len,
                                   &rline_buf, &rline_len);
        if (ib_rc != IB_OK) {
            TSError("Failed to fixup request line");
        }

        ib_rc = start_ib_request(data->tx, rline_buf, rline_len);
        if (ib_rc != IB_OK) {
            TSError("Failed to start IronBee request: %s",
                    ib_status_to_string(ib_rc));
        }
        break;
    }

    case IBD_RESP: {
        TSHttpStatus  http_status;

        ib_rc = start_ib_response(data->tx, rline_buf, rline_len);
        if (ib_rc != IB_OK) {
            TSError("Failed to start IronBee request: %s",
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
        TSError("Invalid direction %d!", ibd->dir);
    }


    /*
     * Parse the header into lines and feed to IronBee as parsed data
     */

    /* The buffer contains the Request line / Status line, together with
     * the actual headers.  So we'll skip the first line, which we already
     * dealt with.
     */
    rv = ib_parsed_name_value_pair_list_wrapper_create(&ibhdrs, data->tx);
    if (rv != IB_OK) {
        TSError("Error creating ironbee header wrapper.  Disabling checks!");
        ret = HDR_ERROR;
        goto process_hdr_cleanup;
    }

    // get_line ensures CRLF (line_len + 2)?
    line = hdr_buf;
    while (next_line(&line, &line_len) > 0) {
        size_t n_len;
        size_t v_len;

        n_len = strcspn(line, ":");
        lptr = line + n_len + 1;
        while (isspace(*lptr) && lptr < line + line_len)
            ++lptr;
        v_len = line_len - (lptr - line);

        /* Ironbee presumably wants to know of anything zero-length
         * so don't reject on those grounds!
         */
        rv = ib_parsed_name_value_pair_list_add(ibhdrs,
                                                line, n_len,
                                                lptr, v_len);
        if (!has_body && (ibd->dir == IBD_REQ)) {
            /* Check for expectation of a request body */
            if (((n_len == 14) && !strncasecmp(line, "Content-Length", n_len))
               || ((n_len == 17) && (v_len == 7)
                   && !strncasecmp(line, "Transfer-Encoding", n_len)
                   && !strncasecmp(lptr, "chunked", v_len))) {
                has_body = true;
            }
        }
        if (rv != IB_OK)
            TSError("Error adding header '%.*s: %.*s' to Ironbee list",
                    (int)n_len, line, (int)v_len, lptr);
        ++nhdrs;
    }

    /* Notify headers if present */
    if (nhdrs > 0) {
        TSDebug("ironbee", "process_hdr: notifying header data");
        rv = (*ibd->ib_notify_header)(data->tx->ib, data->tx, ibhdrs);
        if (rv != IB_OK)
            TSError("Error notifying Ironbee header data event");
        TSDebug("ironbee", "process_hdr: notifying header finished");
        rv = (*ibd->ib_notify_header_finished)(data->tx->ib, data->tx);
        if (rv != IB_OK)
            TSError("Error notifying Ironbee header finished event");
    }

    /* If there are no headers, treat as a transitional response */
    else {
        TSDebug("ironbee",
                "Response has no headers!  Treating as transitional!");
        ret = HDR_HTTP_100;
        goto process_hdr_cleanup;
    }

    /* If there's no body in a Request, notify end-of-request */
    if ((ibd->dir == IBD_REQ) && !has_body) {
        rv = (*ibd->ib_notify_end)(data->tx->ib, data->tx);
        if (rv != IB_OK)
            TSError("Error notifying Ironbee end of request");
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
        setact.value = site->id_str;
        header_action(bufp, hdr_loc, &setact, data->tx->mp);
    }
    else {
        TSDebug("ironbee", "No site available for @IB-SITE-ID");
    }

    /* Add internal header for effective IP address */
    setact.hdr = "@IB-EFFECTIVE-IP";
    setact.value = data->tx->er_ipstr;
    header_action(bufp, hdr_loc, &setact, data->tx->mp);

    /* Now manipulate header as requested by ironbee */
    for (act = data->hdr_actions; act != NULL; act = act->next) {
        if (act->dir != ibd->dir)
            continue;    /* it's not for us */

        TSDebug("ironbee", "Manipulating HTTP headers");
        header_action(bufp, hdr_loc, act, data->tx->mp);
    }

    /* Add internal header if we blocked the transaction */
    setact.hdr = "@IB-BLOCK-FLAG";
    if ((data->tx->flags & (IB_TX_BLOCK_PHASE|IB_TX_BLOCK_IMMEDIATE)) != 0) {
        setact.value = "blocked";
        header_action(bufp, hdr_loc, &setact, data->tx->mp);
    }
    else if (data->tx->flags & IB_TX_BLOCK_ADVISORY) {
        setact.value = "advisory";
        header_action(bufp, hdr_loc, &setact, data->tx->mp);
    }

process_hdr_cleanup:
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

    /* If an error sent us to cleanup then it's in ret.  Else just
     * return whether or not Ironbee has signalled an HTTP status.
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
    ib_status_t            rc;
    const struct sockaddr *addr;
    int                    port;
    ib_conn_t             *iconn = ssn->iconn;
               
    /* remote ip */
    addr = TSHttpTxnClientAddrGet(ssn->txnp);
    addr2str(addr, ssn->remote_ip, &port);

    iconn->remote_ipstr = ssn->remote_ip;
    rc = ib_data_add_bytestr(iconn->data,
                             "remote_ip",
                             (uint8_t *)iconn->remote_ipstr,
                             strlen(ssn->remote_ip),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* remote port */
    iconn->remote_port = port;
    rc = ib_data_add_num(iconn->data, "remote_port", port, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* local end */
    addr = TSHttpTxnIncomingAddrGet(ssn->txnp);

    addr2str(addr, ssn->local_ip, &port);

    iconn->local_ipstr = ssn->local_ip;
    rc = ib_data_add_bytestr(iconn->data,
                             "local_ip",
                             (uint8_t *)iconn->local_ipstr,
                             strlen(ssn->local_ip),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* local_port */
    iconn->local_port = port;
    rc = ib_data_add_num(iconn->data, "local_port", port, NULL);
    if (rc != IB_OK) {
        return rc;
    }
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
    TSVConn connp;
    TSCont mycont;
    TSMutex conn_mutex;
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
            conn_mutex = TSMutexCreate();
            mycont = TSContCreate(ironbee_plugin, conn_mutex);
            TSHttpSsnHookAdd (ssnp, TS_HTTP_TXN_START_HOOK, mycont);
            ssndata = TSmalloc(sizeof(*ssndata));
            memset(ssndata, 0, sizeof(*ssndata));
            ssndata->mutex = conn_mutex;
            ssndata->contp = mycont;
            TSContDataSet(mycont, ssndata);

            TSHttpSsnHookAdd (ssnp, TS_HTTP_SSN_CLOSE_HOOK, mycont);

            TSHttpSsnReenable (ssnp, TS_EVENT_HTTP_CONTINUE);
            break;
        case TS_EVENT_HTTP_TXN_START:
            /* start of Request */
            /* First req on a connection, we set up conn stuff */

            ssndata = TSContDataGet(contp);
            TSMutexLock(ssndata->mutex);
            if (ssndata->iconn == NULL) {
                ib_status_t  rc;
                ib_engine_t *ib;

                ib = ib_manager_engine_current(module_data.manager);
                rc = ib_conn_create(ib, &ssndata->iconn, contp);
                if (rc != IB_OK) {
                    TSError("ironbee: ib_conn_create: %d\n", rc);
                    return rc; // FIXME - figure out what to do
                }
                rc = ironbee_conn_init(ssndata);
                if (rc != IB_OK) {
                    TSError("ironbee: ironbee_conn_init: %d\n", rc);
                    return rc; // FIXME - figure out what to do
                }

                TSDebug("ironbee", "CONN CREATE: conn=%p", ssndata->iconn);
                ssndata->txnp = txnp;
                ssndata->txn_count = ssndata->closing = 0;
                TSContDataSet(contp, ssndata);
                TSDebug("ironbee", "ironbee_plugin: calling ib_state_notify_conn_opened()");
                ib_state_notify_conn_opened(ib, ssndata->iconn);
            }
            ++ssndata->txn_count;
            TSMutexUnlock(ssndata->mutex);

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

            ib_tx_create(&txndata->tx, ssndata->iconn, txndata);
            TSDebug("ironbee", "TX CREATE: conn=%p tx=%p id=%s txn_count=%d", ssndata->iconn, txndata->tx, txndata->tx->id, txndata->ssn->txn_count);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

        /* HTTP RESPONSE */
        case TS_EVENT_HTTP_READ_RESPONSE_HDR:
            txndata = TSContDataGet(contp);

            /* Feed ironbee the headers if not done alread. */
            if (!ib_tx_flags_isset(txndata->tx, IB_TX_FRES_STARTED)) {
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
                if (ib_tx_flags_isset(txndata->tx, IB_TX_FRES_SEENHEADER)) {
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
            if (!ib_tx_flags_isset(txndata->tx, IB_TX_FRES_STARTED)) {
                process_hdr(txndata, txnp, &ib_direction_client_resp);
            }

            /* If there is an error with a body, then notify ironbee.
             *
             * NOTE: I do not see anywhere else to put this as the error body is
             *       just a buffer and not delivered via normal IO channels, so
             *       the error body will never get caught by an event.
             */
            if ((txndata->status != 0) && (txndata->err_body != NULL)) {
                ib_txdata_t itxdata;
                itxdata.data = (uint8_t *)txndata->err_body;
                itxdata.dlen = strlen(txndata->err_body);
                TSDebug("ironbee",
                        "error_response: calling ib_state_notify_response_body_data() %s:%d",
                        __FILE__, __LINE__);
                ib_state_notify_response_body_data(txndata->tx->ib,
                                                   txndata->tx,
                                                   &itxdata);
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
            if (IB_HDR_OUTCOME_IS_HTTP(status, txndata)) {
                TSDebug("ironbee", "HTTP code %d contp=%p", txndata->status, contp);
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
                    TSError("Internal error: ts-ironbee requested error but no error response set");
                    break;
                  case HDR_HTTP_100:
                    /* This can't actually happen with current Trafficserver
                     * versions, as TS will generate a 400 error without
                     * reference to us.  But in case that changes in future ...
                     */
                    TSError("No request headers found!");
                    break;
                  default:
                    TSError("Unhandled state arose in handling request headers");
                    break;
                }
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            }
            break;


            /* CLEANUP EVENTS */
        case TS_EVENT_HTTP_TXN_CLOSE:
        {
            ib_txn_ctx *ctx = TSContDataGet(contp);
            TSDebug("ironbee", "TXN Close: %p\n", (void *)contp);
            if (!ib_tx_flags_isset(ctx->tx, IB_TX_FPOSTPROCESS)) {
                ib_state_notify_postprocess(ctx->tx->ib, ctx->tx);
            }
            if (!ib_tx_flags_isset(ctx->tx, IB_TX_FLOGGING)) {
                ib_state_notify_logging(ctx->tx->ib, ctx->tx);
            }
            ib_txn_ctx_destroy(ctx);
            TSContDataSet(contp, NULL);
            TSContDestroy(contp);
            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;
        }

        case TS_EVENT_HTTP_SSN_CLOSE:
            TSDebug("ironbee", "SSN Close: %p\n", (void *)contp);
            ib_ssn_ctx_destroy(TSContDataGet(contp));
            //TSContDestroy(contp);
            TSHttpSsnReenable(ssnp, TS_EVENT_HTTP_CONTINUE);
            break;

        case TS_EVENT_MGMT_UPDATE:
        {
            ib_status_t  rc;

            TSDebug("ironbee", "Management update\n");
            rc = ib_manager_engine_create(module_data.manager, NULL);
            if (rc != IB_OK) {
                TSError("Failed to create new engine: %s\n",
                        ib_status_to_string(rc));
            }
            break;
        }

            /* if we get here we've got a bug */
        default:
            TSError("BUG: unhandled event %d in ironbee_plugin\n", event);
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
 * IronBee ATS logger.
 *
 * Performs IronBee logging for the ATS plugin.
 *
 * @param[in] ib IronBee engine
 * @param[in] level Debug level
 * @param[in] file File name
 * @param[in] line Line number
 * @param[in] fmt Format string
 * @param[in] ap Var args list to match the format
 * @param[in] cbdata Callback data.
 */
static void ironbee_logger(
    void              *cbdata,
    const char        *buf)
{
    TSTextLogObject log = (TSTextLogObject)cbdata;
    TSTextLogObjectWrite(log, "%s", buf);
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
        TSError("[ts-ironbee] getnameinfo: %d\n", rv);
    }
    *port = atoi(serv);
}


/* this can presumably be global since it's only setup on init */
//static ironbee_config_t ibconfig;
//#define TRACEFILE "/tmp/ironbee-trace"
#define TRACEFILE NULL

/**
 * Handle ATS shutdown for IronBee plugin.
 *
 * Registered via atexit() during initialization, destroys the IB engine,
 * etc.
 *
 */
static void ibexit(void)
{
    ib_manager_destroy(module_data.manager);
    TSTextLogObjectDestroy(module_data.logger);
}

/**
 * Initialize IronBee for ATS.
 *
 * Performs IB initializations for the ATS plugin.
 *
 * @param[in] configfile Configuration file
 * @param[in] logfile Log file
 *
 * @returns status
 */
static ib_status_t ironbee_init(const char *configfile, const char *logfile)
{
    /* grab from httpd module's post-config */
    ib_status_t      rc;
    int              rv;

    /* success is documented as TS_LOG_ERROR_NO_ERROR but that's undefined.
     * It's actually a TS_SUCCESS (proxy/InkAPI.cc line 6641).
     */
    rv = TSTextLogObjectCreate(logfile,
                               TS_LOG_MODE_ADD_TIMESTAMP,
                               &module_data.logger);
    if (rv != TS_SUCCESS) {
        return IB_EUNKNOWN;
    }

    /* Create the IronBee engine manager */
    rc = ib_manager_create(&ibplugin, configfile, &module_data.manager);
    if (rc != IB_OK) {
        return rc;
    }
    ib_manager_set_logger_fn(module_data.manager,
                             NULL, ironbee_logger,
                             module_data.logger);

    /* Create the initial engine */
    rc = ib_manager_engine_create(module_data.manager, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register our at exit function */
    rv = atexit(ibexit);
    if (rv != 0) {
        return IB_EOTHER;
    }

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
    int rv;
    TSPluginRegistrationInfo info;
    TSCont cont;

    /* FIXME - check why these are char*, not const char* */
    info.plugin_name = (char *)"ironbee";
    info.vendor_name = (char *)"Qualys, Inc";
    info.support_email = (char *)"ironbee-users@lists.sourceforge.com";

    if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
        TSError("[ironbee] Plugin registration failed.\n");
        goto Lerror;
    }

    if (!check_ts_version()) {
        TSError("[ironbee] Plugin requires Traffic Server 3.0 or later\n");
        goto Lerror;
    }

    cont = TSContCreate(ironbee_plugin, TSMutexCreate());

    /* connection initialization & cleanup */
    TSHttpHookAdd(TS_HTTP_SSN_START_HOOK, cont);


    if (argc < 2) {
        TSError("[ironbee] configuration file name required\n");
        goto Lerror;
    }
    rv = ironbee_init(argv[1], argc >= 3 ? argv[2] : DEFAULT_LOG);
    if (rv != IB_OK) {
        TSError("[ironbee] initialization failed with %d\n", rv);
    }
    return;

Lerror:
    TSError("[ironbee] Unable to initialize plugin (disabled).\n");
}
