/**
 * Copyright (c) UT-Battelle, LLC. 2014-2015. ALL RIGHTS RESERVED.
 * Copyright (C) Mellanox Technologies Ltd. 2001-2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "knem_md.h"
#include "knem_iface.h"
#include "knem_ep.h"

#include <uct/base/uct_md.h>
#include <ucs/sys/string.h>


static ucs_config_field_t uct_knem_iface_config_table[] = {
    {"", "BW=13862MBs", NULL,
    ucs_offsetof(uct_knem_iface_config_t, super),
    UCS_CONFIG_TYPE_TABLE(uct_sm_iface_config_table)},

    {NULL}
};

static ucs_status_t uct_knem_iface_query(uct_iface_h tl_iface,
                                         uct_iface_attr_t *iface_attr)
{
    uct_knem_iface_t *iface = ucs_derived_of(tl_iface, uct_knem_iface_t);

    uct_base_iface_query(&iface->super.super, iface_attr);

    /* default values for all shared memory transports */
    iface_attr->cap.put.min_zcopy       = 0;
    iface_attr->cap.put.max_zcopy       = SIZE_MAX;
    iface_attr->cap.put.opt_zcopy_align = 1;
    iface_attr->cap.put.align_mtu       = iface_attr->cap.put.opt_zcopy_align;
    iface_attr->cap.put.max_iov         = uct_sm_get_max_iov();

    iface_attr->cap.get.min_zcopy       = 0;
    iface_attr->cap.get.max_zcopy       = SIZE_MAX;
    iface_attr->cap.get.opt_zcopy_align = 1;
    iface_attr->cap.get.align_mtu       = iface_attr->cap.get.opt_zcopy_align;
    iface_attr->cap.get.max_iov         = uct_sm_get_max_iov();

    iface_attr->cap.am.max_iov         = 1;
    iface_attr->cap.am.opt_zcopy_align = 1;
    iface_attr->cap.am.align_mtu       = iface_attr->cap.am.opt_zcopy_align;

    iface_attr->iface_addr_len         = 0;
    iface_attr->device_addr_len        = UCT_SM_IFACE_DEVICE_ADDR_LEN;
    iface_attr->ep_addr_len            = 0;
    iface_attr->max_conn_priv          = 0;
    iface_attr->cap.flags              = UCT_IFACE_FLAG_GET_ZCOPY |
                                         UCT_IFACE_FLAG_PUT_ZCOPY |
                                         UCT_IFACE_FLAG_PENDING   |
                                         UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    iface_attr->latency.overhead       = 80e-9; /* 80 ns */
    iface_attr->latency.growth         = 0;
    iface_attr->bandwidth.shared       = iface->super.config.bandwidth;
    iface_attr->bandwidth.dedicated    = 0;
    iface_attr->overhead               = 0.25e-6; /* 0.25 us */

    return UCS_OK;
}

static UCS_CLASS_DECLARE_DELETE_FUNC(uct_knem_iface_t, uct_iface_t);

static uct_iface_ops_t uct_knem_iface_ops = {
    .ep_put_zcopy             = uct_knem_ep_put_zcopy,
    .ep_get_zcopy             = uct_knem_ep_get_zcopy,
    .ep_pending_add           = (void*)ucs_empty_function_return_busy,
    .ep_pending_purge         = (void*)ucs_empty_function,
    .ep_flush                 = uct_base_ep_flush,
    .ep_fence                 = uct_sm_ep_fence,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_knem_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_knem_ep_t),
    .iface_fence              = uct_sm_iface_fence,
    .iface_progress_enable    = ucs_empty_function,
    .iface_progress_disable   = ucs_empty_function,
    .iface_progress           = ucs_empty_function_return_zero,
    .iface_flush              = uct_base_iface_flush,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_knem_iface_t),
    .iface_query              = uct_knem_iface_query,
    .iface_get_device_address = uct_sm_iface_get_device_address,
    .iface_get_address        = (void*)ucs_empty_function_return_success,
    .iface_is_reachable       = uct_sm_iface_is_reachable
};

static UCS_CLASS_INIT_FUNC(uct_knem_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    UCS_CLASS_CALL_SUPER_INIT(uct_sm_iface_t, &uct_knem_iface_ops, md,
                              worker, params, tl_config);
    self->knem_md = (uct_knem_md_t *)md;
    uct_sm_get_max_iov(); /* to initialize ucs_get_max_iov static variable */

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_knem_iface_t)
{
    /* No OP */
}

UCS_CLASS_DEFINE(uct_knem_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_knem_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t *);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_knem_iface_t, uct_iface_t);

UCT_TL_DEFINE(&uct_knem_component, knem, uct_sm_base_query_tl_devices,
              uct_knem_iface_t, "KNEM_", uct_knem_iface_config_table,
              uct_knem_iface_config_t);
