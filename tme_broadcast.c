// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tme_hwkm_master_intf.h"
#include "tmecom.h"

#define TME_BORADCAST_KEY_CBOR_TAG 0x442F01D9
static uint32_t update_ext_err(struct tme_ext_err_info *err_info,
                              struct tme_response_sts *result)
{
    err_info->tme_err_status = result->tme_err_status;
    err_info->seq_err_status = result->seq_err_status;
    err_info->seq_kp_err_status0 = result->seq_kp_err_status0;
    err_info->seq_kp_err_status1 = result->seq_kp_err_status1;
    err_info->seq_rsp_status = result->seq_rsp_status;
    return result->tme_err_status || result->seq_err_status ||
           result->seq_kp_err_status0 || result->seq_kp_err_status1;
}
uint32_t tme_hwkm_master_broadcast_transportkey(
		struct tme_ext_err_info *err_info)
{
	struct broadcast_tpkey_req *request = NULL;
	struct tme_response_sts *response = NULL;
	uint32_t ret = 0;
	size_t response_len = sizeof(*response);

	if (!err_info)
		return -EINVAL;

	request = kzalloc(sizeof(*request), GFP_KERNEL);
	response = kzalloc(response_len, GFP_KERNEL);

	if (!request || !response) {
		ret = -ENOMEM;
		goto err_exit;
	}

	request->cbor_header = TME_BORADCAST_KEY_CBOR_TAG;
	request->cmd.code    = TME_HWKM_CMD_BROADCAST_TP_KEY;

	ret = tmecom_process_request(request, sizeof(*request), response,
			&response_len);

	if (ret != 0) {
		pr_err("HWKM broadcast TP key request failed\n");
		goto err_exit;
	}

	if (response_len != sizeof(*response)) {
		pr_err("HWKM response failed with invalid length: %zu, %zu\n",
				response_len, sizeof(*response));
		ret = -EBADMSG;
		goto err_exit;
	}

	ret = update_ext_err(err_info, response);

err_exit:
	kfree(request);
	kfree(response);
	return ret;
}

int aynthor_tme_broadcast_transport_key(void)
{
    struct tme_ext_err_info error = {0};
    return (int)tme_hwkm_master_broadcast_transportkey(&error);
}
EXPORT_SYMBOL_GPL(aynthor_tme_broadcast_transport_key);
