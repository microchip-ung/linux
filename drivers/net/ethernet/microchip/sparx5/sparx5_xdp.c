// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/bpf.h>
#include <linux/bpf_trace.h>
#include <linux/filter.h>

#include <net/page_pool/helpers.h>

#include "lan969x/lan969x.h"
#include "sparx5_main.h"

void sparx5_xdp_mem_type_set(struct sparx5 *sparx5, enum xdp_mem_type type,
			     void *allocator)
{
	for (int i = 0; i < sparx5->data->consts.chip_ports; i++) {
		struct sparx5_port *port;

		if (!sparx5->ports[i])
			continue;

		port = sparx5->ports[i];

		if (!port->ndev->xdp_features)
			continue;

		xdp_rxq_info_unreg_mem_model(&port->xdp_rxq);
		xdp_rxq_info_reg_mem_model(&port->xdp_rxq, type, allocator);
	}
}

bool sparx5_port_has_xdp(struct sparx5_port *port)
{
	return !!port->xdp_prog;
}

bool sparx5_has_xdp(struct sparx5 *sparx5)
{
	for (int i = 0; i < sparx5->data->consts.chip_ports; i++)
		if (sparx5->ports[i])
			return sparx5_port_has_xdp(sparx5->ports[i]);

	return false;
}

static int sparx5_xdp_setup(struct net_device *dev, struct netdev_bpf *xdp)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;
	const struct sparx5_ops *ops;
	struct bpf_prog *old_prog;
	bool old_xdp, new_xdp;
	int err;

	ops = &sparx5->data->ops;

	if (!sparx5->fdma_irq) {
		NL_SET_ERR_MSG_MOD(xdp->extack, "XDP requires FDMA enabled");
		return -EOPNOTSUPP;
	}

	old_xdp = sparx5_has_xdp(sparx5);
	old_prog = xchg(&port->xdp_prog, xdp->prog);
	new_xdp = sparx5_has_xdp(sparx5);

	/* We need to change the DMA direction of the page pool to
	 * DMA_BIRECTIONAL, when n_progs > 0 and to DMA_TO_DEVICE when
	 * n_progs == 0. As a consequence, we also need to reset the FDMA
	 * memory.
	 */
	if (!is_sparx5(sparx5) && old_xdp != new_xdp) {
		err = ops->fdma_resize(sparx5);
		if (err) {
			xchg(&port->xdp_prog, old_prog);
			return err;
		}
	}

	if (old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

int sparx5_xdp_port_init(struct sparx5_port *port)
{
	struct sparx5 *sparx5 = port->sparx5;

	return xdp_rxq_info_reg(&port->xdp_rxq,
				port->ndev,
				0,
				sparx5->rx.napi.napi_id);
}

void sparx5_xdp_port_deinit(struct sparx5_port *port)
{
	if (xdp_rxq_info_is_reg(&port->xdp_rxq))
		xdp_rxq_info_unreg(&port->xdp_rxq);
}

int sparx5_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return sparx5_xdp_setup(dev, xdp);
	default:
		return -EINVAL;
	}
}

int sparx5_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames,
		    u32 flags)
{
	struct sparx5_port *port = netdev_priv(dev);
	int err, n_xmit = 0;

	for (int i = 0; i < n; i++) {
		struct xdp_frame *xdpf = frames[i];

		err = lan969x_fdma_xmit_xdp(port, xdpf, 0);
		if (err)
			break;

		n_xmit++;
	}

	return n_xmit;
}

int sparx5_xdp_run(struct sparx5_port *port, struct page *page, u32 len)
{
	struct bpf_prog *xdp_prog = port->xdp_prog;
	struct sparx5 *sparx5 = port->sparx5;
	struct xdp_buff xdp;
	u32 act;

	xdp_init_buff(&xdp,
		      PAGE_SIZE << sparx5->rx.page_order,
		      &port->xdp_rxq);

	xdp_prepare_buff(&xdp,
			 page_address(page),
			 IFH_LEN * 4 + XDP_PACKET_HEADROOM,
			 len - IFH_LEN * 4,
			 false);

	act = bpf_prog_run_xdp(xdp_prog, &xdp);
	switch (act) {
	case XDP_PASS:
		return FDMA_PASS;
	case XDP_TX:
		if (lan969x_fdma_xmit_xdp(port, page, len - IFH_LEN_BYTES))
			return FDMA_DROP;
		else
			return FDMA_TX;

	case XDP_REDIRECT:
		if (xdp_do_redirect(port->ndev, &xdp, xdp_prog))
			return FDMA_DROP;

		return FDMA_REDIRECT;
	default:
		bpf_warn_invalid_xdp_action(port->ndev, xdp_prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(port->ndev, xdp_prog, act);
		fallthrough;
	case XDP_DROP:
		return FDMA_DROP;
	}

	return 0;
}
