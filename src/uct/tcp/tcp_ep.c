/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "tcp.h"

#include <ucs/async/async.h>
#include <ucs/sys/iovec.h>


/* Forward declaration */
static unsigned uct_tcp_ep_progress_data_tx(uct_tcp_ep_t *ep);


const uct_tcp_cm_state_t uct_tcp_ep_cm_state[] = {
    [UCT_TCP_EP_CONN_STATE_CLOSED]      = {
        .name        = "CLOSED",
        .tx_progress = (uct_tcp_ep_progress_t)ucs_empty_function_return_zero
    },
    [UCT_TCP_EP_CONN_STATE_CONNECTING]  = {
        .name        = "CONNECTING",
        .tx_progress = uct_tcp_cm_conn_progress
    },
    [UCT_TCP_EP_CONN_STATE_WAITING_ACK] = {
        .name        = "WAITING_ACK",
        .tx_progress = (uct_tcp_ep_progress_t)ucs_empty_function_return_zero
    },
    [UCT_TCP_EP_CONN_STATE_ACCEPTING]   = {
        .name        = "ACCEPTING",
        .tx_progress = (uct_tcp_ep_progress_t)ucs_empty_function_return_zero
    },
    [UCT_TCP_EP_CONN_STATE_WAITING_REQ] = {
        .name        = "WAITING_REQ",
        .tx_progress = (uct_tcp_ep_progress_t)ucs_empty_function_return_zero
    },
    [UCT_TCP_EP_CONN_STATE_CONNECTED]   = {
        .name        = "CONNECTED",
        .tx_progress = uct_tcp_ep_progress_data_tx
    }
};

static inline int uct_tcp_ep_ctx_buf_empty(uct_tcp_ep_ctx_t *ctx)
{
    ucs_assert((ctx->length == 0) || (ctx->buf != NULL));

    return ctx->length == 0;
}

static inline int uct_tcp_ep_ctx_buf_need_progress(uct_tcp_ep_ctx_t *ctx)
{
    ucs_assert(ctx->offset <= ctx->length);

    return ctx->offset < ctx->length;
}

static inline ucs_status_t uct_tcp_ep_check_tx_res(uct_tcp_ep_t *ep)
{
    if (ucs_unlikely(ep->conn_state != UCT_TCP_EP_CONN_STATE_CONNECTED)) {
        if (ep->conn_state == UCT_TCP_EP_CONN_STATE_CLOSED) {
            return UCS_ERR_UNREACHABLE;
        }

        ucs_assertv((ep->conn_state == UCT_TCP_EP_CONN_STATE_CONNECTING) ||
                    (ep->conn_state == UCT_TCP_EP_CONN_STATE_WAITING_ACK) ||
                    (ep->conn_state == UCT_TCP_EP_CONN_STATE_WAITING_REQ),
                    "ep=%p", ep);
        return UCS_ERR_NO_RESOURCE;
    }

    return uct_tcp_ep_ctx_buf_empty(&ep->tx) ? UCS_OK : UCS_ERR_NO_RESOURCE;
}

static inline void uct_tcp_ep_ctx_rewind(uct_tcp_ep_ctx_t *ctx)
{
    ctx->offset = 0;
    ctx->length = 0;
}

static inline void uct_tcp_ep_ctx_init(uct_tcp_ep_ctx_t *ctx)
{
    ctx->buf = NULL;
    uct_tcp_ep_ctx_rewind(ctx);
}

static inline void uct_tcp_ep_ctx_reset(uct_tcp_ep_ctx_t *ctx)
{
    ucs_mpool_put_inline(ctx->buf);
    uct_tcp_ep_ctx_init(ctx);
}

static void uct_tcp_ep_addr_cleanup(struct sockaddr_in *sock_addr)
{
    memset(sock_addr, 0, sizeof(*sock_addr));
}

static void uct_tcp_ep_addr_init(struct sockaddr_in *sock_addr,
                                 const struct sockaddr_in *peer_addr)
{
    /* TODO: handle IPv4 and IPv6 */
    if (peer_addr == NULL) {
        uct_tcp_ep_addr_cleanup(sock_addr);
    } else {
        *sock_addr = *peer_addr;
    }
}

static void uct_tcp_ep_close_fd(int *fd_p)
{
    if (*fd_p != -1) {
        close(*fd_p);
        *fd_p = -1;
    }
}

unsigned uct_tcp_ep_is_self(const uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    ucs_status_t status;
    int cmp;

    cmp = ucs_sockaddr_cmp((const struct sockaddr*)&ep->peer_addr,
                           (const struct sockaddr*)&iface->config.ifaddr,
                           &status);
    ucs_assertv(status == UCS_OK, "ep=%p", ep);
    return !cmp;
}

static void uct_tcp_ep_cleanup(uct_tcp_ep_t *ep)
{
    uct_tcp_ep_addr_cleanup(&ep->peer_addr);

    if (ep->tx.buf) {
        uct_tcp_ep_ctx_reset(&ep->tx);
    }

    if (ep->rx.buf) {
        uct_tcp_ep_ctx_reset(&ep->rx);
    }

    if (ep->events && (ep->fd != -1)) {
        uct_tcp_ep_mod_events(ep, 0, ep->events);
    }

    uct_tcp_ep_close_fd(&ep->fd);
}

static UCS_CLASS_INIT_FUNC(uct_tcp_ep_t, uct_tcp_iface_t *iface,
                           int fd, const struct sockaddr_in *dest_addr)
{
    ucs_status_t status;

    ucs_assertv(fd >= 0, "iface=%p", iface);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super)

    uct_tcp_ep_addr_init(&self->peer_addr, dest_addr);

    uct_tcp_ep_ctx_init(&self->tx);
    uct_tcp_ep_ctx_init(&self->rx);

    self->events     = 0;
    self->fd         = fd;
    self->ctx_caps   = 0;
    self->conn_state = UCT_TCP_EP_CONN_STATE_CLOSED;

    ucs_list_head_init(&self->list);
    ucs_queue_head_init(&self->pending_q);

    status = ucs_sys_fcntl_modfl(self->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        goto err_cleanup;
    }

    status = uct_tcp_iface_set_sockopt(iface, self->fd);
    if (status != UCS_OK) {
        goto err_cleanup;
    }

    uct_tcp_iface_add_ep(self);

    ucs_debug("tcp_ep %p: created on iface %p, fd %d", self, iface, self->fd);
    return UCS_OK;

err_cleanup:
    /* need to be closed by this function caller */
    self->fd = -1;
    uct_tcp_ep_cleanup(self);
    return status;
}

const char *uct_tcp_ep_ctx_caps_str(uint8_t ep_ctx_caps, char *str_buffer)
{
    ucs_snprintf_zero(str_buffer, UCT_TCP_EP_CTX_CAPS_STR_MAX, "[%s:%s]",
                      (ep_ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX)) ?
                      "Tx" : "-",
                      (ep_ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX)) ?
                      "Rx" : "-");
    return str_buffer;
}

void uct_tcp_ep_change_ctx_caps(uct_tcp_ep_t *ep, uint8_t new_caps)
{
    char str_prev_ctx_caps[UCT_TCP_EP_CTX_CAPS_STR_MAX];
    char str_cur_ctx_caps[UCT_TCP_EP_CTX_CAPS_STR_MAX];

    if (ep->ctx_caps != new_caps) {
        ucs_trace("tcp_ep %p: ctx caps changed %s -> %s", ep,
                  uct_tcp_ep_ctx_caps_str(ep->ctx_caps, str_prev_ctx_caps),
                  uct_tcp_ep_ctx_caps_str(new_caps, str_cur_ctx_caps));
        ep->ctx_caps = new_caps;
    }
}

ucs_status_t uct_tcp_ep_add_ctx_cap(uct_tcp_ep_t *ep,
                                    uct_tcp_ep_ctx_type_t cap)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    uint8_t prev_caps      = ep->ctx_caps;

    uct_tcp_ep_change_ctx_caps(ep, ep->ctx_caps | UCS_BIT(cap));
    if (!uct_tcp_ep_is_self(ep) && (prev_caps != ep->ctx_caps)) {
        if (!prev_caps) {
            return uct_tcp_cm_add_ep(iface, ep);
        } else if (ucs_test_all_flags(ep->ctx_caps,
                                      (UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX) |
                                       UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX)))) {
            uct_tcp_cm_remove_ep(iface, ep);
        }
    }

    return UCS_OK;
}

ucs_status_t uct_tcp_ep_remove_ctx_cap(uct_tcp_ep_t *ep,
                                       uct_tcp_ep_ctx_type_t cap)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    uint8_t prev_caps      = ep->ctx_caps;

    uct_tcp_ep_change_ctx_caps(ep, ep->ctx_caps & ~UCS_BIT(cap));
    if (!uct_tcp_ep_is_self(ep)) {
        if (ucs_test_all_flags(prev_caps,
                               (UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX) |
                                UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX)))) {
            return uct_tcp_cm_add_ep(iface, ep);
        } else if (!ep->ctx_caps) {
            uct_tcp_cm_remove_ep(iface, ep);
        }
    }

    return UCS_OK;
}

ucs_status_t uct_tcp_ep_move_ctx_cap(uct_tcp_ep_t *from_ep, uct_tcp_ep_t *to_ep,
                                     uct_tcp_ep_ctx_type_t ctx_cap)
{
    ucs_status_t status;

    status = uct_tcp_ep_remove_ctx_cap(from_ep, ctx_cap);
    if (status != UCS_OK) {
        return status;
    }

    return uct_tcp_ep_add_ctx_cap(to_ep, ctx_cap);
}

static UCS_CLASS_CLEANUP_FUNC(uct_tcp_ep_t)
{
    uct_tcp_iface_t UCS_V_UNUSED *iface =
        ucs_derived_of(self->super.super.iface, uct_tcp_iface_t);

    uct_tcp_ep_mod_events(self, 0, self->events);

    if (self->ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX)) {
        uct_tcp_ep_remove_ctx_cap(self, UCT_TCP_EP_CTX_TYPE_TX);
    }

    if (self->ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX)) {
        uct_tcp_ep_remove_ctx_cap(self, UCT_TCP_EP_CTX_TYPE_RX);
    }

    ucs_assertv(!self->ctx_caps, "ep=%p", self);

    uct_tcp_iface_remove_ep(self);

    if (self->conn_state != UCT_TCP_EP_CONN_STATE_CLOSED) {
        uct_tcp_cm_change_conn_state(self, UCT_TCP_EP_CONN_STATE_CLOSED);
    }

    uct_tcp_ep_cleanup(self);

    ucs_debug("tcp_ep %p: destroyed on iface %p", self, iface);
}

UCS_CLASS_DEFINE(uct_tcp_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NAMED_NEW_FUNC(uct_tcp_ep_init, uct_tcp_ep_t, uct_tcp_ep_t,
                                uct_tcp_iface_t*, int,
                                const struct sockaddr_in*)
UCS_CLASS_DEFINE_NAMED_DELETE_FUNC(uct_tcp_ep_destroy_internal,
                                   uct_tcp_ep_t, uct_ep_t)

void uct_tcp_ep_destroy(uct_ep_h tl_ep)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);

    if ((ep->conn_state == UCT_TCP_EP_CONN_STATE_CONNECTED) &&
        ucs_test_all_flags(ep->ctx_caps,
                           UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX) |
                           UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX))) {
        /* remove TX capability, but still will be able to receive data */
        uct_tcp_ep_remove_ctx_cap(ep, UCT_TCP_EP_CTX_TYPE_TX);
    } else {
        uct_tcp_ep_destroy_internal(tl_ep);
    }
}

void uct_tcp_ep_set_failed(uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);

    if (ep->conn_state != UCT_TCP_EP_CONN_STATE_CLOSED) {
        uct_tcp_cm_change_conn_state(ep, UCT_TCP_EP_CONN_STATE_CLOSED);
    }

    uct_set_ep_failed(&UCS_CLASS_NAME(uct_tcp_ep_t),
                      &ep->super.super, &iface->super.super,
                      UCS_ERR_UNREACHABLE);
}

static ucs_status_t uct_tcp_ep_create_connected(uct_tcp_iface_t *iface,
                                                const struct sockaddr_in *dest_addr,
                                                uct_tcp_ep_t **new_ep)
{
    ucs_status_t status;
    uct_tcp_ep_t *ep;
    int fd;

    status = ucs_socket_create(AF_INET, SOCK_STREAM, &fd);
    if (status != UCS_OK) {
        return status;
    }

    status = uct_tcp_ep_init(iface, fd, dest_addr, &ep);
    if (status != UCS_OK) {
        goto err_close_fd;
    }

    status = uct_tcp_cm_conn_start(ep);
    if (status != UCS_OK) {
        goto err_ep_destroy;
    }

    status = uct_tcp_ep_add_ctx_cap(ep, UCT_TCP_EP_CTX_TYPE_TX);
    if (status != UCS_OK) {
        goto err_ep_destroy;
    }

    *new_ep = ep;

    return UCS_OK;

err_ep_destroy:
    uct_tcp_ep_destroy_internal(&ep->super.super);
err_close_fd:
    close(fd);
    return status;
}

ucs_status_t uct_tcp_ep_create(const uct_ep_params_t *params,
                               uct_ep_h *ep_p)
{
    uct_tcp_iface_t *iface = ucs_derived_of(params->iface, uct_tcp_iface_t);
    uct_tcp_ep_t *ep       = NULL;
    struct sockaddr_in dest_addr;
    ucs_status_t status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    memset(&dest_addr, 0, sizeof(dest_addr));
    /* TODO: handle AF_INET6 */
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port   = *(in_port_t*)params->iface_addr;
    dest_addr.sin_addr   = *(struct in_addr*)params->dev_addr;

    do {
        ep = uct_tcp_cm_search_ep(iface, &dest_addr,
                                  UCT_TCP_EP_CTX_TYPE_RX);
        if (ep) {
            /* Found EP with RX ctx, try to send the connection request
             * to the remote peer, if it successful - assign TX to this EP
             * and return the EP to the user, otherwise - destroy this EP
             * and try to search another EP w/o TX capability or create
             * new EP */
            status = uct_tcp_cm_send_event(ep, UCT_TCP_CM_CONN_REQ);
            if (status != UCS_OK) {
                uct_tcp_ep_destroy_internal(&ep->super.super);
                ep = NULL;
            } else {
                status = uct_tcp_ep_add_ctx_cap(ep, UCT_TCP_EP_CTX_TYPE_TX);
                if (status != UCS_OK) {
                    return status;
                }
            }
        } else {
            status = uct_tcp_ep_create_connected(iface, &dest_addr, &ep);
            break;
        }
    } while (ep == NULL);

    if (status == UCS_OK) {
        /* cppcheck-suppress autoVariables */
        *ep_p = &ep->super.super;
    }
    return status;
}

void uct_tcp_ep_mod_events(uct_tcp_ep_t *ep, int add, int remove)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    int old_events         = ep->events;
    int new_events         = (ep->events | add) & ~remove;
    ucs_status_t status;

    if (new_events != ep->events) {
        ep->events = new_events;
        ucs_trace("tcp_ep %p: set events to %c%c", ep,
                  (new_events & UCS_EVENT_SET_EVREAD)  ? 'r' : '-',
                  (new_events & UCS_EVENT_SET_EVWRITE) ? 'w' : '-');
        if (new_events == 0) {
            status = ucs_event_set_del(iface->event_set, ep->fd);
        } else if (old_events != 0) {
            status = ucs_event_set_mod(iface->event_set, ep->fd,
                                       ep->events, (void *)ep);
        } else {
            status = ucs_event_set_add(iface->event_set, ep->fd,
                                       ep->events, (void *)ep);
        }
        if (status != UCS_OK) {
            ucs_fatal("unable to modify event set for tcp_ep %p (fd=%d)", ep,
                      ep->fd);
        }
    }
}

void uct_tcp_ep_pending_queue_dispatch(uct_tcp_ep_t *ep)
{
    uct_pending_req_priv_queue_t *priv;

    uct_pending_queue_dispatch(priv, &ep->pending_q,
                               uct_tcp_ep_ctx_buf_empty(&ep->tx));
    if (uct_tcp_ep_ctx_buf_empty(&ep->tx)) {
        ucs_assert(ucs_queue_is_empty(&ep->pending_q));
        uct_tcp_ep_mod_events(ep, 0, UCS_EVENT_SET_EVWRITE);
    }
}

/* Fill iovec data structure by data provided in uct_iov_t.
 * The function avoids copying IOVs with zero length.
 * @return Number of elements in io_vec[].
 */
static inline size_t
uct_tcp_ep_iovec_fill_iov(struct iovec *io_vec, const uct_iov_t *iov,
                          size_t iovcnt, size_t *total_length)
{
    size_t iov_it, io_vec_it = 0;

    *total_length = 0;

    for (iov_it = 0; iov_it < iovcnt; ++iov_it) {
        io_vec[io_vec_it].iov_len = uct_iov_get_length(&iov[iov_it]);

        /* Avoid zero length elements in resulted iov_vec */
        if (io_vec[io_vec_it].iov_len != 0) {
            io_vec[io_vec_it].iov_base = iov[iov_it].buffer;
            *total_length += io_vec[io_vec_it].iov_len;
            ++io_vec_it;
        }
    }

    return io_vec_it;
}

static void uct_tcp_ep_handle_disconnected(uct_tcp_ep_t *ep,
                                           uct_tcp_ep_ctx_t *ctx)
{
    ucs_debug("tcp_ep %p: remote disconnected", ep);

    uct_tcp_ep_mod_events(ep, 0, UCS_EVENT_SET_EVREAD);
    uct_tcp_ep_ctx_reset(ctx);

    if (ep->ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX)) {
        if (ep->ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_TX)) {
            uct_tcp_ep_remove_ctx_cap(ep, UCT_TCP_EP_CTX_TYPE_RX);
            uct_tcp_ep_mod_events(ep, 0, UCS_EVENT_SET_EVREAD);
        } else {
            /* If the EP supports RX only, destroy it */
            uct_tcp_ep_destroy_internal(&ep->super.super);
        }
    }
}

static inline unsigned uct_tcp_ep_send(uct_tcp_ep_t *ep, size_t *sent_length)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    ucs_status_t status;

    *sent_length = ep->tx.length - ep->tx.offset;
    ucs_assert(*sent_length > 0);

    status = ucs_socket_send_nb(ep->fd, ep->tx.buf + ep->tx.offset,
                                sent_length, NULL, NULL);
    if (status != UCS_OK) {
        return 0;
    }

    iface->outstanding -= *sent_length;
    ep->tx.offset      += *sent_length;

    return (*sent_length > 0);
}

static inline unsigned uct_tcp_ep_sendv(uct_tcp_ep_t *ep, size_t *sent_length)
{
    uct_tcp_iface_t *iface      = ucs_derived_of(ep->super.super.iface,
                                                 uct_tcp_iface_t);
    uct_tcp_ep_zcopy_ctx_t *ctx = (uct_tcp_ep_zcopy_ctx_t*)ep->tx.buf;
    ucs_status_t status;

    ucs_assertv(ep->tx.offset < ep->tx.length, "ep=%p", ep);

    status = ucs_socket_sendv_nb(ep->fd, &ctx->iov[ctx->iov_index],
                                 ctx->iov_cnt - ctx->iov_index,
                                 sent_length, NULL, NULL);

    ep->tx.offset      += *sent_length;
    iface->outstanding -= *sent_length;

    if ((ep->tx.offset != ep->tx.length) &&
        ((status == UCS_OK) || (status == UCS_ERR_NO_PROGRESS))) {
        ucs_iov_advance(ctx->iov, ctx->iov_cnt,
                        &ctx->iov_index, *sent_length);
    } else {
        ep->ctx_caps  &= ~UCS_BIT(UCT_TCP_EP_CTX_TYPE_ZCOPY_TX);
        if (ctx->comp != NULL) {
            uct_invoke_completion(ctx->comp, status);
        }
    }

    return (*sent_length > 0);
}

static ucs_status_t uct_tcp_ep_io_err_handler_cb(void *arg, int io_errno)
{
    uct_tcp_ep_t *ep                    = (uct_tcp_ep_t*)arg;
    uct_tcp_iface_t UCS_V_UNUSED *iface = ucs_derived_of(ep->super.super.iface,
                                                         uct_tcp_iface_t);
    char str_local_addr[UCS_SOCKADDR_STRING_LEN];
    char str_remote_addr[UCS_SOCKADDR_STRING_LEN];

    if ((io_errno == ECONNRESET) &&
        (ep->conn_state == UCT_TCP_EP_CONN_STATE_CONNECTED) &&
        (ep->ctx_caps == UCS_BIT(UCT_TCP_EP_CTX_TYPE_RX)) /* only RX cap */) {
        ucs_debug("tcp_ep %p: detected %d (%s) error, the [%s <-> %s] "
                  "connection was dropped by the peer",
                  ep, io_errno, strerror(io_errno),
                  ucs_sockaddr_str((const struct sockaddr*)&iface->config.ifaddr,
                                   str_local_addr, UCS_SOCKADDR_STRING_LEN),
                  ucs_sockaddr_str((const struct sockaddr*)&ep->peer_addr,
                                   str_remote_addr, UCS_SOCKADDR_STRING_LEN));
        return UCS_OK;
    }

    return UCS_ERR_NO_PROGRESS;
}

static inline unsigned uct_tcp_ep_recv(uct_tcp_ep_t *ep, size_t recv_length)
{
    ucs_status_t status;

    ucs_assertv(recv_length, "ep=%p", ep);

    status = ucs_socket_recv_nb(ep->fd, ep->rx.buf + ep->rx.length, &recv_length,
                                uct_tcp_ep_io_err_handler_cb, ep);
    if (status != UCS_OK) {
        if (status == UCS_ERR_NO_PROGRESS) {
            /* If no data were read to the allocated buffer,
             * we can safely reset it for futher re-use and to
             * avoid overwriting this buffer, because `rx::length == 0` */
            if (!ep->rx.length) {
                uct_tcp_ep_ctx_reset(&ep->rx);
            }
        } else {
            uct_tcp_ep_handle_disconnected(ep, &ep->rx);
        }
        return 0;
    }

    ucs_assertv(recv_length, "ep=%p", ep);

    ep->rx.length += recv_length;
    ucs_trace_data("tcp_ep %p: recvd %zu bytes", ep, recv_length);

    return 1;
}

static unsigned uct_tcp_ep_progress_data_tx(uct_tcp_ep_t *ep)
{
    unsigned count = 0;
    size_t sent_length;

    ucs_trace_func("ep=%p", ep);

    if (uct_tcp_ep_ctx_buf_need_progress(&ep->tx)) {
        if (!(ep->ctx_caps & UCS_BIT(UCT_TCP_EP_CTX_TYPE_ZCOPY_TX))) {
            count += uct_tcp_ep_send(ep, &sent_length);
        } else {
            count += uct_tcp_ep_sendv(ep, &sent_length);
        }

        ucs_trace_data("ep %p fd %d sent %zu/%zu bytes, moved to offest %zu",
                       ep, ep->fd, ep->tx.offset, ep->tx.length, sent_length);

        if (!uct_tcp_ep_ctx_buf_need_progress(&ep->tx)) {
            uct_tcp_ep_ctx_reset(&ep->tx);
        }
    }

    if (!ucs_queue_is_empty(&ep->pending_q)) {
        uct_tcp_ep_pending_queue_dispatch(ep);
        return count;
    }

    if (uct_tcp_ep_ctx_buf_empty(&ep->tx)) {
        ucs_assert(ucs_queue_is_empty(&ep->pending_q));
        uct_tcp_ep_mod_events(ep, 0, UCS_EVENT_SET_EVWRITE);
    }

    return count;
}

static inline void
uct_tcp_ep_comp_recv_am(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                        uct_tcp_am_hdr_t *hdr)
{
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_RECV, hdr->am_id,
                       hdr + 1, hdr->length,
                       "RECV: ep %p fd %d received %zu/%zu bytes",
                       ep, ep->fd, ep->rx.offset, ep->rx.length);
    uct_iface_invoke_am(&iface->super, hdr->am_id, hdr + 1, hdr->length, 0);
}

unsigned uct_tcp_ep_progress_rx(uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    unsigned handled       = 0;
    uct_tcp_am_hdr_t *hdr;
    size_t recv_length;
    size_t remainder;

    ucs_trace_func("ep=%p", ep);

    if (!uct_tcp_ep_ctx_buf_need_progress(&ep->rx)) {
        ucs_assert(ep->rx.buf == NULL);

        ep->rx.buf = ucs_mpool_get_inline(&iface->rx_mpool);
        if (ucs_unlikely(ep->rx.buf == NULL)) {
            ucs_warn("tcp_ep %p: unable to get a buffer from RX memory pool", ep);
            return 0;
        }

        /* post the entire AM buffer */
        recv_length = iface->config.rx_seg_size;
    } else if (ep->rx.length - ep->rx.offset < sizeof(*hdr)) {
        ucs_assert(ep->rx.buf != NULL);

        /* do partial receive of the remaining part of the hdr
         * and post the entire AM buffer */
        recv_length = iface->config.rx_seg_size - ep->rx.length;
    } else {
        ucs_assert(ep->rx.buf != NULL);

        /* do partial receive of the remaining user data */
        hdr         = ep->rx.buf + ep->rx.offset;
        recv_length = hdr->length - (ep->rx.length - ep->rx.offset - sizeof(*hdr));
    }

    if (!uct_tcp_ep_recv(ep, recv_length)) {
        goto out;
    }

    /* Parse received active messages */
    while (uct_tcp_ep_ctx_buf_need_progress(&ep->rx)) {
        remainder = ep->rx.length - ep->rx.offset;
        if (remainder < sizeof(*hdr)) {
            /* Move the partially received hdr to the beginning of the buffer */
            memmove(ep->rx.buf, ep->rx.buf + ep->rx.offset, remainder);
            ep->rx.offset = 0;
            ep->rx.length = remainder;
            handled++;
            goto out;
        }

        hdr = ep->rx.buf + ep->rx.offset;
        ucs_assert(hdr->length <= (iface->config.rx_seg_size -
                                   sizeof(uct_tcp_am_hdr_t)));

        if (remainder < sizeof(*hdr) + hdr->length) {
            handled++;
            goto out;
        }

        /* Full message was received */
        ep->rx.offset += sizeof(*hdr) + hdr->length;

        if (ucs_likely(hdr->am_id < UCT_AM_ID_MAX)) {
            uct_tcp_ep_comp_recv_am(iface, ep, hdr);
            handled++;
        } else {
            handled += 1 + uct_tcp_cm_handle_conn_pkt(&ep, hdr + 1, hdr->length);
            if (ep == NULL) {
                goto out;
            }
        }
    }

    uct_tcp_ep_ctx_reset(&ep->rx);

out:
    return handled;
}

static inline ucs_status_t
uct_tcp_ep_am_prepare(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                      uint8_t am_id, uct_tcp_am_hdr_t **hdr)
{
    ucs_status_t status;

    UCT_CHECK_AM_ID(am_id);

    status = uct_tcp_ep_check_tx_res(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        if (ucs_likely(status == UCS_ERR_NO_RESOURCE)) {
            goto err_no_res;
        }
        return status;
    }

    ucs_assertv(ep->tx.buf == NULL, "ep=%p", ep);

    ep->tx.buf = ucs_mpool_get_inline(&iface->tx_mpool);
    if (ucs_unlikely(ep->tx.buf == NULL)) {
        goto err_no_res;
    }

    *hdr          = ep->tx.buf;
    (*hdr)->am_id = am_id;

    return UCS_OK;

err_no_res:
    uct_tcp_ep_mod_events(ep, UCS_EVENT_SET_EVWRITE, 0);
    UCS_STATS_UPDATE_COUNTER(ep->super.stats, UCT_EP_STAT_NO_RES, 1);
    return UCS_ERR_NO_RESOURCE;
}

static inline void
uct_tcp_ep_set_outstanding_zcopy(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                                 uct_tcp_ep_zcopy_ctx_t *ctx, const void *header,
                                 unsigned header_length, uct_completion_t *comp)
{
    ctx->comp     = comp;
    ep->ctx_caps |= UCS_BIT(UCT_TCP_EP_CTX_TYPE_ZCOPY_TX);

    if ((header_length != 0) &&
        /* check whether a user's header was sent or not */
        (ep->tx.offset < (sizeof(uct_tcp_am_hdr_t) + header_length))) {
        ucs_assert(header_length <= iface->config.zcopy.max_hdr);
        /* if the user's header wasn't sent completely, copy it to
         * the EP TX buffer (after Zcopy context and IOVs) for
         * retransmission. iov_len is already set to the proper value */
        ctx->iov[1].iov_base = ep->tx.buf +
                               iface->config.zcopy.hdr_offset;
        memcpy(ctx->iov[1].iov_base, header, header_length);
    }

    ctx->iov_index = 0;
    ucs_iov_advance(ctx->iov, ctx->iov_cnt, &ctx->iov_index, ep->tx.offset);
    uct_tcp_ep_mod_events(ep, UCS_EVENT_SET_EVWRITE, 0);
}

static inline void uct_tcp_ep_am_send(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                                      const uct_tcp_am_hdr_t *hdr)
{
    size_t sent_length;

    ep->tx.length       = sizeof(*hdr) + hdr->length;
    iface->outstanding += ep->tx.length;

    uct_tcp_ep_send(ep, &sent_length);

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, hdr->am_id,
                       hdr + 1, hdr->length, "SEND: ep %p fd %d sent "
                       "%zu/%zu bytes, moved to offest %zu",
                       ep, ep->fd, ep->tx.offset, ep->tx.length, sent_length);

    if (ucs_likely(!uct_tcp_ep_ctx_buf_need_progress(&ep->tx))) {
        uct_tcp_ep_ctx_reset(&ep->tx);
    } else {
        uct_tcp_ep_mod_events(ep, UCS_EVENT_SET_EVWRITE, 0);
    }
}

static const void*
uct_tcp_ep_am_sendv_get_trace_payload(uct_tcp_am_hdr_t *hdr,
                                      const void *header,
                                      const struct iovec *payload_iov,
                                      int short_sendv)
{
    if (!short_sendv) {
        return header;
    }

    /* If user requested trace data, we copy header and payload
     * to EP TX buffer in order to trace correct data */
    uct_am_short_fill_data(hdr + 1, *(const uint64_t*)header,
                           payload_iov->iov_base, payload_iov->iov_len);
    return (hdr + 1);
}

static inline ucs_status_t
uct_tcp_ep_am_sendv(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                    int short_sendv, uct_tcp_am_hdr_t *hdr,
                    size_t send_limit, const void *header,
                    struct iovec *iov, size_t iov_cnt)
{
    ucs_status_t status;

    ep->tx.length = hdr->length + sizeof(*hdr);

    ucs_assertv(ep->tx.length <= send_limit, "ep=%p", ep);

    status = ucs_socket_sendv_nb(ep->fd, iov, iov_cnt,
                                 &ep->tx.offset, NULL, NULL);

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, hdr->am_id,
                       /* the function will be invoked only in case of
                        * data tracing is enabled */
                       uct_tcp_ep_am_sendv_get_trace_payload(hdr, header,
                                                             &iov[2], short_sendv),
                       hdr->length, "SEND: ep %p fd %d sent %zu/%zu bytes, "
                       "moved to offest %zu, iov cnt %zu "
                       "[addr %p len %zu] [addr %p len %zu]",
                       ep, ep->fd, ep->tx.offset, ep->tx.length,
                       ep->tx.offset, iov_cnt,
                       /* print user-defined header or
                        * first iovec with a payload */
                       ((iov_cnt > 1) ? iov[1].iov_base : NULL),
                       ((iov_cnt > 1) ? iov[1].iov_len  : 0),
                       /* print first/second iovec with a payload */
                       ((iov_cnt > 2) ? iov[2].iov_base : NULL),
                       ((iov_cnt > 2) ? iov[2].iov_len  : 0));

    iface->outstanding += ep->tx.length - ep->tx.offset;

    return status;
}

ucs_status_t uct_tcp_ep_am_short(uct_ep_h uct_ep, uint8_t am_id, uint64_t header,
                                 const void *payload, unsigned length)
{
    uct_tcp_ep_t *ep       = ucs_derived_of(uct_ep, uct_tcp_ep_t);
    uct_tcp_iface_t *iface = ucs_derived_of(uct_ep->iface, uct_tcp_iface_t);
    uct_tcp_am_hdr_t *hdr  = NULL;
    struct iovec iov[UCT_TCP_EP_AM_SHORTV_IOV_COUNT];
    uint32_t payload_length;
    size_t offset;
    ucs_status_t status;

    UCT_CHECK_LENGTH(length + sizeof(header), 0,
                     iface->config.tx_seg_size - sizeof(uct_tcp_am_hdr_t),
                     "am_short");

    status = uct_tcp_ep_am_prepare(iface, ep, am_id, &hdr);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assertv(hdr != NULL, "ep=%p", ep);

    /* Save the length of the payload, because hdr (ep::buf)
     * can be released inside `uct_tcp_ep_am_send` call */
    hdr->length = payload_length = length + sizeof(header);

    if (length <= iface->config.sendv_thresh) {
        uct_am_short_fill_data(hdr + 1, header, payload, length);
        uct_tcp_ep_am_send(iface, ep, hdr);
        UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_length);
    } else {
        offset = ((ep->tx.offset >= sizeof(*hdr)) ?
                  (ep->tx.offset - sizeof(*hdr)) : 0);

        iov[0].iov_base = hdr;
        iov[0].iov_len  = sizeof(*hdr);

        iov[1].iov_base = &header;
        iov[1].iov_len  = sizeof(header);

        iov[2].iov_base = (void*)payload;
        iov[2].iov_len  = length;

        status = uct_tcp_ep_am_sendv(iface, ep, 1, hdr,
                                     iface->config.tx_seg_size, &header,
                                     iov, UCT_TCP_EP_AM_SHORTV_IOV_COUNT);
        if ((status == UCS_OK) || (status == UCS_ERR_NO_PROGRESS)) {
            UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, payload_length);

            if (uct_tcp_ep_ctx_buf_need_progress(&ep->tx)) {
                /* Copy only user's header and payload to the TX buffer,
                 * TCP AM header is placed at the beginning of the buffer */
                ucs_iov_copy(&iov[1], UCT_TCP_EP_AM_SHORTV_IOV_COUNT - 1,
                             offset, UCS_PTR_BYTE_OFFSET(hdr + 1, offset),
                             (ep->tx.length - sizeof(*hdr)) - offset,
                             UCS_IOV_COPY_TO_BUF);
                uct_tcp_ep_mod_events(ep, UCS_EVENT_SET_EVWRITE, 0);
                return UCS_OK;
            }

            status = UCS_OK;
        }

        uct_tcp_ep_ctx_reset(&ep->tx);
    }

    return status;
}

ssize_t uct_tcp_ep_am_bcopy(uct_ep_h uct_ep, uint8_t am_id,
                            uct_pack_callback_t pack_cb, void *arg,
                            unsigned flags)
{
    uct_tcp_ep_t *ep       = ucs_derived_of(uct_ep, uct_tcp_ep_t);
    uct_tcp_iface_t *iface = ucs_derived_of(uct_ep->iface, uct_tcp_iface_t);
    uct_tcp_am_hdr_t *hdr  = NULL;
    uint32_t payload_length;
    ucs_status_t status;

    status = uct_tcp_ep_am_prepare(iface, ep, am_id, &hdr);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assertv(hdr != NULL, "ep=%p", ep);

    /* Save the length of the payload, because hdr (ep::buf)
     * can be released inside `uct_tcp_ep_am_send` call */
    hdr->length = payload_length = pack_cb(hdr + 1, arg);

    uct_tcp_ep_am_send(iface, ep, hdr);

    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, payload_length);

    return payload_length;
}

ucs_status_t uct_tcp_ep_am_zcopy(uct_ep_h uct_ep, uint8_t am_id, const void *header,
                                 unsigned header_length, const uct_iov_t *iov,
                                 size_t iovcnt, unsigned flags,
                                 uct_completion_t *comp)
{
    uct_tcp_ep_t *ep       = ucs_derived_of(uct_ep, uct_tcp_ep_t);
    uct_tcp_iface_t *iface = ucs_derived_of(uct_ep->iface, uct_tcp_iface_t);
    uct_tcp_am_hdr_t *hdr  = NULL;
    uct_tcp_ep_zcopy_ctx_t *ctx;
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.zcopy.max_iov,
                       "uct_tcp_ep_am_zcopy");
    UCT_CHECK_LENGTH(header_length, 0, iface->config.zcopy.max_hdr,
                     "am_zcopy header");
    UCT_CHECK_LENGTH(header_length + uct_iov_total_length(iov, iovcnt), 0,
                     iface->config.rx_seg_size - sizeof(uct_tcp_am_hdr_t),
                     "am_zcopy");

    status = uct_tcp_ep_am_prepare(iface, ep, am_id, &hdr);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assertv(hdr != NULL, "ep=%p", ep);

    ctx          = ucs_derived_of(hdr, uct_tcp_ep_zcopy_ctx_t);
    ctx->iov_cnt = 0;

    /* TCP transport header */
    ctx->iov[ctx->iov_cnt].iov_base = hdr;
    ctx->iov[ctx->iov_cnt].iov_len  = sizeof(*hdr);
    ctx->iov_cnt++;

    if (header_length != 0) {
        /* User-defined header */
        ucs_assert(header != NULL);
        ctx->iov[ctx->iov_cnt].iov_base = (void*)header;
        ctx->iov[ctx->iov_cnt].iov_len  = header_length;
        ctx->iov_cnt++;
    }

    /* User-defined payload */
    ctx->iov_cnt += uct_tcp_ep_iovec_fill_iov(&ctx->iov[ctx->iov_cnt], iov,
                                              iovcnt, &ep->tx.length);
    hdr->length   = ep->tx.length + header_length;

    status = uct_tcp_ep_am_sendv(iface, ep, 0, hdr,
                                 iface->config.rx_seg_size,
                                 header, ctx->iov, ctx->iov_cnt);
    if ((status == UCS_OK) || (status == UCS_ERR_NO_PROGRESS)) {
        UCT_TL_EP_STAT_OP(&ep->super, AM, ZCOPY, hdr->length);

        if (uct_tcp_ep_ctx_buf_need_progress(&ep->tx)) {
            uct_tcp_ep_set_outstanding_zcopy(iface, ep, ctx, header,
                                             header_length, comp);
            return UCS_INPROGRESS;
        }
    }

    uct_tcp_ep_ctx_reset(&ep->tx);
    return status;
}

ucs_status_t uct_tcp_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req,
                                    unsigned flags)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);

    if (uct_tcp_ep_check_tx_res(ep) == UCS_OK) {
        return UCS_ERR_BUSY;
    }

    uct_pending_req_queue_push(&ep->pending_q, req);
    UCT_TL_EP_STAT_PEND(&ep->super);
    return UCS_OK;
}

void uct_tcp_ep_pending_purge(uct_ep_h tl_ep, uct_pending_purge_callback_t cb,
                              void *arg)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);
    uct_pending_req_priv_queue_t *priv;

    uct_pending_queue_purge(priv, &ep->pending_q, 1, cb, arg);
}

ucs_status_t uct_tcp_ep_flush(uct_ep_h tl_ep, unsigned flags,
                              uct_completion_t *comp)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);

    if (uct_tcp_ep_check_tx_res(ep) == UCS_ERR_NO_RESOURCE) {
        UCT_TL_EP_STAT_FLUSH_WAIT(&ep->super);
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_TL_EP_STAT_FLUSH(&ep->super);
    return UCS_OK;
}

