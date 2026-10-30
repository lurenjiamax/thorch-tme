// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"tmecom: [%s][%d]:" fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mailbox_client.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/mailbox/qmp.h>
#include <linux/uaccess.h>
#include <linux/mailbox_controller.h>

#include "tmecom.h"

struct tmecom {
	struct device *dev;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct mutex lock;
	struct qmp_pkt pkt;
	wait_queue_head_t waitq;
	void *txbuf;
	bool rx_done;
};

static struct tmecom *tmedev;

/**
 * tmecom_msg_hdr - Request/Response message header between HLOS and TME.
 *
 * This header is proceeding any request specific parameters.
 * The transaction id is used to match request with response.
 *
 * Note: glink/QMP layer provides the rx/tx data size, so user payload size
 * is calculated by reducing the header size.
 */
struct tmecom_msg_hdr {
	unsigned int reserved; /* for future use */
	unsigned int txnid;    /* transaction id */
} __packed;
#define TMECOM_TX_HDR_SIZE sizeof(struct tmecom_msg_hdr)
#define CBOR_NUM_BYTES (sizeof(unsigned int))
#define TMECOM_RX_HDR_SIZE (TMECOM_TX_HDR_SIZE + CBOR_NUM_BYTES)

/*
 * CBOR encode emulation
 * Prepend tmecom_msg_hdr space
 * CBOR tag is prepended in request
 */
static inline size_t tmecom_encode(struct tmecom *tdev, const void *reqbuf,
		size_t size)
{
	unsigned int *msg = tdev->txbuf + TMECOM_TX_HDR_SIZE;
	unsigned int *src = (unsigned int *)reqbuf;

	memcpy(msg, src, size);
	return (size + TMECOM_TX_HDR_SIZE);
}

/*
 * CBOR decode emulation
 * Strip tmecom_msg_hdr & CBOR tag
 */
static inline size_t tmecom_decode(struct tmecom *tdev, void *respbuf)
{
	unsigned int *msg = tdev->pkt.data + TMECOM_RX_HDR_SIZE;
	unsigned int *rbuf = (unsigned int *)respbuf;

	memcpy(rbuf, msg, (tdev->pkt.size - TMECOM_RX_HDR_SIZE));
	return (tdev->pkt.size - TMECOM_RX_HDR_SIZE);
}

static bool tmecom_check_rx_done(struct tmecom *tdev)
{
	return  tdev->rx_done;
}

int tmecom_process_request(const void *reqbuf, size_t reqsize, void *respbuf,
		size_t *respsize)
{
	struct tmecom *tdev = tmedev;
	long time_left = 0;
	int ret = 0;

	/*
	 * Check to handle if probe is not successful or not completed yet
	 */
	if (!tdev) {
		pr_err("%s: tmecom dev is NULL\n", __func__);
		return -ENODEV;
	}

	if (!reqbuf || !reqsize || (reqsize > MBOX_MAX_MSG_LEN)) {
		dev_err(tdev->dev, "invalid reqbuf or reqsize\n");
		return -EINVAL;
	}

	if (!respbuf || !respsize || (*respsize > MBOX_MAX_MSG_LEN)) {
		dev_err(tdev->dev, "invalid respbuf or respsize\n");
		return -EINVAL;
	}

	dev_dbg(tdev->dev, "PMDIAG tme request lock begin\n");
	mutex_lock(&tdev->lock);
	dev_dbg(tdev->dev, "PMDIAG tme request lock end\n");

	tdev->rx_done = false;
	tdev->pkt.size = tmecom_encode(tdev, reqbuf, reqsize);
	/*
	 * Controller expects a 4 byte aligned buffer
	 */
	tdev->pkt.size = (tdev->pkt.size + 0x3) & ~0x3;
	tdev->pkt.data = tdev->txbuf;

	pr_debug("tmecom encoded request size = %u\n", tdev->pkt.size);
	

	dev_dbg(tdev->dev, "PMDIAG tme mailbox-send begin\n");
	if (mbox_send_message(tdev->chan, &tdev->pkt) < 0) {
		dev_err(tdev->dev, "failed to send qmp message\n");
		ret = -EAGAIN;
		goto err_exit;
	}

	dev_dbg(tdev->dev, "PMDIAG tme mailbox-send end; response-wait begin\n");
	time_left = wait_event_interruptible_timeout(tdev->waitq,
			tmecom_check_rx_done(tdev), msecs_to_jiffies(tdev->cl.tx_tout));
	dev_dbg(tdev->dev, "PMDIAG tme response-wait end remaining=%ld\n", time_left);

	if (time_left < 0) { ret = time_left; goto err_exit; }
	if (!time_left) {
		dev_err(tdev->dev, "request timed out\n");
		ret = -ETIMEDOUT;
		goto err_exit;
	}

	dev_dbg(tdev->dev, "response received\n");

	pr_debug("tmecom received size = %u\n", tdev->pkt.size);
	

	if (tdev->pkt.size <= TMECOM_RX_HDR_SIZE) {
		dev_err(tdev->dev, "invalid pkt.size received\n");
		ret = -EPROTO;
		goto err_exit;
	}

	if (tdev->pkt.size - TMECOM_RX_HDR_SIZE > *respsize) {
		ret = -EMSGSIZE; goto err_exit;
	}
	dev_dbg(tdev->dev, "PMDIAG tme response-decode begin\n");
	*respsize = tmecom_decode(tdev, respbuf);
	dev_dbg(tdev->dev, "PMDIAG tme response-decode end\n");

	tdev->rx_done = false;
	ret = 0;

err_exit:
	dev_dbg(tdev->dev, "PMDIAG tme request unlock begin ret=%d\n", ret);
	mutex_unlock(&tdev->lock);
	dev_dbg(tdev->dev, "PMDIAG tme request end ret=%d\n", ret);
	return ret;
}
EXPORT_SYMBOL(tmecom_process_request);

static void tmecom_receive_message(struct mbox_client *client, void *message)
{
	struct tmecom *tdev = dev_get_drvdata(client->dev);
	struct qmp_pkt *pkt = NULL;

	if (!message) {
		dev_err(tdev->dev, "spurious message received\n");
		goto tmecom_receive_end;
	}

	if (tdev->rx_done) {
		dev_err(tdev->dev, "tmecom response pending\n");
		goto tmecom_receive_end;
	}
	pkt = (struct qmp_pkt *)message;
	tdev->pkt.size = pkt->size;
	tdev->pkt.data = pkt->data;
	tdev->rx_done = true;
tmecom_receive_end:
	wake_up_interruptible(&tdev->waitq);
}

static int tmecom_probe(struct platform_device *pdev)
{
	struct tmecom *tdev;
	const char *label;
	char name[32];

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->cl.dev = &pdev->dev;
	tdev->cl.tx_block = true;
	tdev->cl.tx_tout = 500;
	tdev->cl.knows_txdone = false;
	tdev->cl.rx_callback = tmecom_receive_message;

	label = of_get_property(pdev->dev.of_node, "mbox-names", NULL);
	if (!label)
		return -EINVAL;
	snprintf(name, 32, "%s_send_message", label);

	tdev->chan = mbox_request_channel(&tdev->cl, 0);
	if (IS_ERR(tdev->chan)) {
		dev_err(&pdev->dev, "failed to get mbox channel\n");
		return PTR_ERR(tdev->chan);
	}

	mutex_init(&tdev->lock);

	if (tdev->chan) {
		tdev->txbuf =
			devm_kzalloc(&pdev->dev, MBOX_MAX_MSG_LEN, GFP_KERNEL);
		if (!tdev->txbuf) {
			dev_err(&pdev->dev, "message buffer alloc faile\n");
			return -ENOMEM;
		}
	}

	init_waitqueue_head(&tdev->waitq);



	tdev->rx_done = false;
	tdev->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, tdev);

	tmedev = tdev;

	dev_info(&pdev->dev, "tmecom probe success\n");
	return 0;

}

static void tmecom_remove(struct platform_device *pdev)
{
	struct tmecom *tdev = platform_get_drvdata(pdev);
	tmedev = NULL;



	if (tdev->chan)
		mbox_free_channel(tdev->chan);

	dev_info(&pdev->dev, "tmecom remove success\n");
	return;
}

static const struct of_device_id tmecom_match_tbl[] = {
	{.compatible = "qcom,tmecom-qmp-client"},
	{},
};

static struct platform_driver tmecom_driver = {
	.probe = tmecom_probe,
	.remove = tmecom_remove,
	.driver = {
		.name = "tmecom-qmp-client",
		.suppress_bind_attrs = true,
		.of_match_table = tmecom_match_tbl,
	},
};
module_platform_driver(tmecom_driver);

MODULE_DESCRIPTION("MSM TMECom QTI mailbox protocol client");
MODULE_LICENSE("GPL v2");
