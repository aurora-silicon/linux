// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host bridge driver for Apple system-on-chips.
 *
 * The HW is ECAM compliant, so once the controller is initialized,
 * the driver mostly deals MSI mapping and handling of per-port
 * interrupts (INTx, management and error signals).
 *
 * Initialization requires enabling power and clocks, along with a
 * number of register pokes.
 *
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2021 Google LLC
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 *
 * Author: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Author: Marc Zyngier <maz@kernel.org>
 */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/iopoll.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/pci-ecam.h>

#include "pci-host-common.h"

static int link_up_timeout = 500;
module_param(link_up_timeout, int, 0644);
MODULE_PARM_DESC(link_up_timeout, "PCIe link training timeout in milliseconds");

/* T8103 (original M1) and related SoCs */
#define CORE_RC_PHYIF_CTL		0x00024
#define   CORE_RC_PHYIF_CTL_RUN		BIT(0)
#define CORE_RC_PHYIF_STAT		0x00028
#define   CORE_RC_PHYIF_STAT_REFCLK	BIT(4)
#define CORE_RC_CTL			0x00050
#define   CORE_RC_CTL_RUN		BIT(0)
#define CORE_RC_STAT			0x00058
#define   CORE_RC_STAT_READY		BIT(0)
#define CORE_FABRIC_STAT		0x04000
#define   CORE_FABRIC_STAT_MASK		0x001F001F

#define CORE_PHY_DEFAULT_BASE(port)	(0x84000 + 0x4000 * (port))

#define PHY_LANE_CFG			0x00000
#define   PHY_LANE_CFG_REFCLK0REQ	BIT(0)
#define   PHY_LANE_CFG_REFCLK1REQ	BIT(1)
#define   PHY_LANE_CFG_REFCLK0ACK	BIT(2)
#define   PHY_LANE_CFG_REFCLK1ACK	BIT(3)
#define   PHY_LANE_CFG_REFCLKEN		(BIT(9) | BIT(10))
#define   PHY_LANE_CFG_REFCLKCGEN	(BIT(30) | BIT(31))
#define PHY_LANE_CTL			0x00004
#define   PHY_LANE_CTL_CFGACC		BIT(15)

#define PORT_LTSSMCTL			0x00080
#define   PORT_LTSSMCTL_START		BIT(0)
#define PORT_INTSTAT			0x00100
#define   PORT_INT_TUNNEL_ERR		31
#define   PORT_INT_CPL_TIMEOUT		23
#define   PORT_INT_RID2SID_MAPERR	22
#define   PORT_INT_CPL_ABORT		21
#define   PORT_INT_MSI_BAD_DATA		19
#define   PORT_INT_MSI_ERR		18
#define   PORT_INT_REQADDR_GT32		17
#define   PORT_INT_AF_TIMEOUT		15
#define   PORT_INT_LINK_DOWN		14
#define   PORT_INT_LINK_UP		12
#define   PORT_INT_LINK_BWMGMT		11
#define   PORT_INT_AER_MASK		(15 << 4)
#define   PORT_INT_PORT_ERR		4
#define   PORT_INT_INTx(i)		i
#define   PORT_INT_INTx_MASK		15
#define PORT_INTMSK			0x00104
#define PORT_INTMSKSET			0x00108
#define PORT_INTMSKCLR			0x0010c
#define PORT_MSICFG			0x00124
#define   PORT_MSICFG_EN		BIT(0)
#define   PORT_MSICFG_L2MSINUM_SHIFT	4
#define PORT_MSIBASE			0x00128
#define   PORT_MSIBASE_1_SHIFT		16
#define PORT_MSIADDR			0x00168
#define PORT_LINKSTS			0x00208
#define   PORT_LINKSTS_UP		BIT(0)
#define   PORT_LINKSTS_BUSY		BIT(2)
#define PORT_LINKCMDSTS			0x00210
#define PORT_OUTS_NPREQS		0x00284
#define   PORT_OUTS_NPREQS_REQ		BIT(24)
#define   PORT_OUTS_NPREQS_CPL		BIT(16)
#define PORT_RXWR_FIFO			0x00288
#define   PORT_RXWR_FIFO_HDR		GENMASK(15, 10)
#define   PORT_RXWR_FIFO_DATA		GENMASK(9, 0)
#define PORT_RXRD_FIFO			0x0028C
#define   PORT_RXRD_FIFO_REQ		GENMASK(6, 0)
#define PORT_OUTS_CPLS			0x00290
#define   PORT_OUTS_CPLS_SHRD		GENMASK(14, 8)
#define   PORT_OUTS_CPLS_WAIT		GENMASK(6, 0)
#define PORT_APPCLK			0x00800
#define   PORT_APPCLK_EN		BIT(0)
#define   PORT_APPCLK_CGDIS		BIT(8)
#define PORT_STATUS			0x00804
#define   PORT_STATUS_READY		BIT(0)
#define PORT_REFCLK			0x00810
#define   PORT_REFCLK_EN		BIT(0)
#define   PORT_REFCLK_CGDIS		BIT(8)
#define PORT_PERST			0x00814
#define   PORT_PERST_OFF		BIT(0)
#define PORT_RID2SID			0x00828
#define   PORT_RID2SID_VALID		BIT(31)
#define   PORT_RID2SID_SID_SHIFT	16
#define   PORT_RID2SID_BUS_SHIFT	8
#define   PORT_RID2SID_DEV_SHIFT	3
#define   PORT_RID2SID_FUNC_SHIFT	0
#define PORT_OUTS_PREQS_HDR		0x00980
#define   PORT_OUTS_PREQS_HDR_MASK	GENMASK(9, 0)
#define PORT_OUTS_PREQS_DATA		0x00984
#define   PORT_OUTS_PREQS_DATA_MASK	GENMASK(15, 0)
#define PORT_TUNCTRL			0x00988
#define   PORT_TUNCTRL_PERST_ON		BIT(0)
#define   PORT_TUNCTRL_PERST_ACK_REQ	BIT(1)
#define PORT_TUNSTAT			0x0098c
#define   PORT_TUNSTAT_PERST_ON		BIT(0)
#define   PORT_TUNSTAT_PERST_ACK_PEND	BIT(1)
#define PORT_PREFMEM_ENABLE		0x00994

/* T602x (M2-pro and co) */
#define PORT_T602X_MSIADDR	0x016c
#define PORT_T602X_MSIADDR_HI	0x0170
#define PORT_T602X_PERST	0x082c
#define PORT_T602X_RID2SID	0x3000
#define PORT_T602X_MSIMAP	0x3800

#define PORT_MSIMAP_ENABLE	BIT(31)
#define PORT_MSIMAP_TARGET	GENMASK(7, 0)

/*
 * The doorbell address is set to 0xfffff000, which by convention
 * matches what MacOS does, and it is possible to use any other
 * address (in the bottom 4GB, as the base register is only 32bit).
 * However, it has to be excluded from the IOVA range, and the DART
 * driver has to know about it.
 */
#define DOORBELL_ADDR		CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR

struct hw_info {
	u32 phy_lane_ctl;
	u32 port_msiaddr;
	u32 port_msiaddr_hi;
	u32 port_refclk;
	u32 port_perst;
	u32 port_rid2sid;
	u32 port_msimap;
	u32 max_rid2sid;
	bool tunneled;
};

static const struct hw_info t8103_hw = {
	.phy_lane_ctl		= PHY_LANE_CTL,
	.port_msiaddr		= PORT_MSIADDR,
	.port_msiaddr_hi	= 0,
	.port_refclk		= PORT_REFCLK,
	.port_perst		= PORT_PERST,
	.port_rid2sid		= PORT_RID2SID,
	.port_msimap		= 0,
	.max_rid2sid		= 64,
};

static const struct hw_info t602x_hw = {
	.phy_lane_ctl		= 0,
	.port_msiaddr		= PORT_T602X_MSIADDR,
	.port_msiaddr_hi	= PORT_T602X_MSIADDR_HI,
	.port_refclk		= 0,
	.port_perst		= PORT_T602X_PERST,
	.port_rid2sid		= PORT_T602X_RID2SID,
	.port_msimap		= PORT_T602X_MSIMAP,
	/* 16 on t602x, guess for autodetect on future HW */
	.max_rid2sid		= 512,
};

static const struct hw_info t8103_pciec_hw = {
	.port_msiaddr		= PORT_MSIADDR,
	.port_perst		= PORT_PERST,
	.port_rid2sid		= PORT_RID2SID,
	.max_rid2sid		= 64,
	.tunneled		= true,
};

struct apple_pcie {
	struct mutex		lock;
	struct device		*dev;
	void __iomem            *base;
	const struct hw_info	*hw;
	unsigned long		*bitmap;
	struct list_head	ports;
	struct completion	event;
	struct irq_fwspec	fwspec;
	struct irq_domain	*msi_domain;
	u32			nvecs;
};

struct apple_pcie_port {
	raw_spinlock_t		lock;
	struct apple_pcie	*pcie;
	struct device_node	*np;
	void __iomem		*base;
	void __iomem		*phy;
	struct irq_domain	*domain;
	struct list_head	entry;
	unsigned long		*sid_map;
	unsigned int		irq;
	unsigned int		link_irqs[2];
	int			sid_map_sz;
	int			idx;
	bool			started;
};

static void rmw_set(u32 set, void __iomem *addr)
{
	writel_relaxed(readl_relaxed(addr) | set, addr);
}

static void rmw_clear(u32 clr, void __iomem *addr)
{
	writel_relaxed(readl_relaxed(addr) & ~clr, addr);
}

/*
 * PCIe-C lives behind the same tunneled fabric as its DART.  A live Linux
 * /dev/mem replay of the root-port setup sequence showed that the aperture is
 * available, but each access needs a full completion barrier.  Without it,
 * back-to-back relaxed accesses can leave a transaction pending until a later
 * access reports an asynchronous SError.  Keep conventional root ports on the
 * existing fast path.
 */
static inline void apple_pcie_port_writel(struct apple_pcie_port *port,
					  u32 value, u32 offset)
{
	writel_relaxed(value, port->base + offset);
	if (port->pcie->hw->tunneled) {
		mb();
		isb();
	}
}

static inline u32 apple_pcie_port_readl(struct apple_pcie_port *port,
					u32 offset)
{
	u32 value = readl_relaxed(port->base + offset);

	if (port->pcie->hw->tunneled) {
		mb();
		isb();
	}

	return value;
}

static void apple_pcie_port_rmw_set(struct apple_pcie_port *port, u32 set,
				    u32 offset)
{
	apple_pcie_port_writel(port, apple_pcie_port_readl(port, offset) | set,
				 offset);
}

static void apple_pcie_port_rmw_clear(struct apple_pcie_port *port, u32 clear,
				      u32 offset)
{
	apple_pcie_port_writel(port,
				 apple_pcie_port_readl(port, offset) & ~clear,
				 offset);
}

static void apple_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	msg->address_hi = upper_32_bits(DOORBELL_ADDR);
	msg->address_lo = lower_32_bits(DOORBELL_ADDR);
	msg->data = data->hwirq;
}

static struct irq_chip apple_msi_bottom_chip = {
	.name			= "MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_compose_msi_msg	= apple_msi_compose_msg,
};

static int apple_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct apple_pcie *pcie = domain->host_data;
	struct irq_fwspec fwspec = pcie->fwspec;
	unsigned int i;
	int ret, hwirq;

	mutex_lock(&pcie->lock);

	hwirq = bitmap_find_free_region(pcie->bitmap, pcie->nvecs,
					order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	if (hwirq < 0)
		return -ENOSPC;

	fwspec.param[fwspec.param_count - 2] += hwirq;

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);
	if (ret) {
		mutex_lock(&pcie->lock);
		bitmap_release_region(pcie->bitmap, hwirq,
				      order_base_2(nr_irqs));
		mutex_unlock(&pcie->lock);
		return ret;
	}

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &apple_msi_bottom_chip, pcie);
	}

	return 0;
}

static void apple_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct apple_pcie *pcie = domain->host_data;

	mutex_lock(&pcie->lock);

	bitmap_release_region(pcie->bitmap, d->hwirq, order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);
}

static const struct irq_domain_ops apple_msi_domain_ops = {
	.alloc	= apple_msi_domain_alloc,
	.free	= apple_msi_domain_free,
};

static void apple_port_irq_mask(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	guard(raw_spinlock_irqsave)(&port->lock);
	apple_pcie_port_rmw_set(port, BIT(data->hwirq), PORT_INTMSK);
}

static void apple_port_irq_unmask(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	guard(raw_spinlock_irqsave)(&port->lock);
	apple_pcie_port_rmw_clear(port, BIT(data->hwirq), PORT_INTMSK);
}

static bool hwirq_is_intx(unsigned int hwirq)
{
	return BIT(hwirq) & PORT_INT_INTx_MASK;
}

static void apple_port_irq_ack(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	if (!hwirq_is_intx(data->hwirq))
		apple_pcie_port_writel(port, BIT(data->hwirq), PORT_INTSTAT);
}

static int apple_port_irq_set_type(struct irq_data *data, unsigned int type)
{
	/*
	 * It doesn't seem that there is any way to configure the
	 * trigger, so assume INTx have to be level (as per the spec),
	 * and the rest is edge (which looks likely).
	 */
	if (hwirq_is_intx(data->hwirq) ^ !!(type & IRQ_TYPE_LEVEL_MASK))
		return -EINVAL;

	irqd_set_trigger_type(data, type);
	return 0;
}

static struct irq_chip apple_port_irqchip = {
	.name		= "PCIe",
	.irq_ack	= apple_port_irq_ack,
	.irq_mask	= apple_port_irq_mask,
	.irq_unmask	= apple_port_irq_unmask,
	.irq_set_type	= apple_port_irq_set_type,
};

static int apple_port_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct apple_pcie_port *port = domain->host_data;
	struct irq_fwspec *fwspec = args;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_flow_handler_t flow = handle_edge_irq;
		unsigned int type = IRQ_TYPE_EDGE_RISING;

		if (hwirq_is_intx(fwspec->param[0] + i)) {
			flow = handle_level_irq;
			type = IRQ_TYPE_LEVEL_HIGH;
		}

		irq_domain_set_info(domain, virq + i, fwspec->param[0] + i,
				    &apple_port_irqchip, port, flow,
				    NULL, NULL);

		irq_set_irq_type(virq + i, type);
	}

	return 0;
}

static void apple_port_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

static const struct irq_domain_ops apple_port_irq_domain_ops = {
	.translate	= irq_domain_translate_onecell,
	.alloc		= apple_port_irq_domain_alloc,
	.free		= apple_port_irq_domain_free,
};

static void apple_port_irq_handler(struct irq_desc *desc)
{
	struct apple_pcie_port *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long stat;
	int i;

	chained_irq_enter(chip, desc);

	stat = apple_pcie_port_readl(port, PORT_INTSTAT);

	for_each_set_bit(i, &stat, 32)
		generic_handle_domain_irq(port->domain, i);

	chained_irq_exit(chip, desc);
}

static int apple_pcie_port_setup_irq(struct apple_pcie_port *port)
{
	struct fwnode_handle *fwnode = &port->np->fwnode;
	struct apple_pcie *pcie = port->pcie;
	u32 val = 0;

	/* FIXME: consider moving each interrupt under each port */
	port->irq = irq_of_parse_and_map(to_of_node(dev_fwnode(port->pcie->dev)),
					 port->idx);
	if (!port->irq)
		return -ENXIO;

	port->domain = irq_domain_create_linear(fwnode, 32,
						&apple_port_irq_domain_ops,
						port);
	if (!port->domain) {
		irq_dispose_mapping(port->irq);
		port->irq = 0;
		return -ENOMEM;
	}

	/* Disable all interrupts */
	apple_pcie_port_writel(port, ~0, PORT_INTMSK);
	apple_pcie_port_writel(port, ~0, PORT_INTSTAT);
	apple_pcie_port_writel(port, ~0, PORT_LINKCMDSTS);

	irq_set_chained_handler_and_data(port->irq, apple_port_irq_handler, port);

	/* Configure MSI base address */
	BUILD_BUG_ON(upper_32_bits(DOORBELL_ADDR));
	apple_pcie_port_writel(port, lower_32_bits(DOORBELL_ADDR),
				 pcie->hw->port_msiaddr);
	if (pcie->hw->port_msiaddr_hi)
		apple_pcie_port_writel(port, 0, pcie->hw->port_msiaddr_hi);

	/* Enable MSIs, shared between all ports */
	if (pcie->hw->port_msimap) {
		for (int i = 0; i < pcie->nvecs; i++)
			apple_pcie_port_writel(port,
				FIELD_PREP(PORT_MSIMAP_TARGET, i) |
				PORT_MSIMAP_ENABLE,
				pcie->hw->port_msimap + 4 * i);
	} else {
		apple_pcie_port_writel(port, 0, PORT_MSIBASE);
		val = ilog2(pcie->nvecs) << PORT_MSICFG_L2MSINUM_SHIFT;
	}

	apple_pcie_port_writel(port, val | PORT_MSICFG_EN, PORT_MSICFG);
	return 0;
}

static irqreturn_t apple_pcie_port_irq(int irq, void *data)
{
	struct apple_pcie_port *port = data;
	unsigned int hwirq = irq_domain_get_irq_data(port->domain, irq)->hwirq;

	switch (hwirq) {
	case PORT_INT_LINK_UP:
		dev_info_ratelimited(port->pcie->dev, "Link up on %pOF\n",
				     port->np);
		complete_all(&port->pcie->event);
		break;
	case PORT_INT_LINK_DOWN:
		dev_info_ratelimited(port->pcie->dev, "Link down on %pOF\n",
				     port->np);
		break;
	default:
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int apple_pcie_port_register_irqs(struct apple_pcie_port *port)
{
	static struct {
		unsigned int	hwirq;
		const char	*name;
	} port_irqs[] = {
		{ PORT_INT_LINK_UP,	"Link up",	},
		{ PORT_INT_LINK_DOWN,	"Link down",	},
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(port_irqs); i++) {
		struct irq_fwspec fwspec = {
			.fwnode		= &port->np->fwnode,
			.param_count	= 1,
			.param		= {
				[0]	= port_irqs[i].hwirq,
			},
		};
		int irq, ret;

		irq = irq_domain_alloc_irqs(port->domain, 1, NUMA_NO_NODE,
					    &fwspec);
		if (irq <= 0)
			return irq ?: -ENOMEM;

		ret = request_irq(irq, apple_pcie_port_irq, 0,
				  port_irqs[i].name, port);
		if (ret) {
			irq_domain_free_irqs(irq, 1);
			return ret;
		}

		port->link_irqs[i] = irq;
	}

	return 0;
}

static void apple_pcie_port_unregister_irqs(struct apple_pcie_port *port)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(port->link_irqs); i++) {
		if (!port->link_irqs[i])
			continue;

		free_irq(port->link_irqs[i], port);
		irq_domain_free_irqs(port->link_irqs[i], 1);
		port->link_irqs[i] = 0;
	}
}

static int apple_pcie_tunnel_release_reset(struct apple_pcie_port *port)
{
	u32 stat;
	int ret;

	/*
	 * Starting the CIO PCIe tunnel leaves the tunneled root port in reset.
	 * Releasing it means writing zero to PORT_TUNCTRL, waiting for the
	 * reset-active indication to clear, and only then enabling the LTSSM.
	 */
	apple_pcie_port_writel(port, 0, PORT_TUNCTRL);
	ret = read_poll_timeout_atomic(apple_pcie_port_readl, stat,
				       !(stat & PORT_TUNSTAT_PERST_ON),
				       1000, 100000, false, port, PORT_TUNSTAT);
	if (ret)
		dev_err(port->pcie->dev,
			"port %pOF tunnel reset release timed out\n", port->np);

	return ret;
}

static void apple_pcie_tunnel_stop(struct apple_pcie_port *port)
{
	struct apple_pcie *pcie = port->pcie;
	u32 stat;
	int ret;

	/*
	 * PCIe-C has no external PERST# line. The USB4 router asserts the
	 * tunneled reset request, and the root port must acknowledge it before
	 * ACIO loses power. A cable disconnect requires that same ordering.
	 */
	apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ON, PORT_TUNCTRL);
	ret = read_poll_timeout_atomic(apple_pcie_port_readl, stat,
				       stat & PORT_TUNSTAT_PERST_ON,
				       1000, 100000, false, port, PORT_TUNSTAT);
	if (ret)
		dev_warn(pcie->dev, "port %pOF tunnel reset assertion timed out\n",
			 port->np);

	apple_pcie_port_rmw_clear(port, PORT_PERST_OFF,
				  pcie->hw->port_perst);
	apple_pcie_port_rmw_clear(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);
	apple_pcie_port_rmw_clear(port, PORT_APPCLK_EN, PORT_APPCLK);

	ret = read_poll_timeout_atomic(apple_pcie_port_readl, stat,
				       !(stat & PORT_STATUS_READY),
				       10, 100000, false, port, PORT_STATUS);
	if (ret)
		dev_warn(pcie->dev, "port %pOF disable timed out\n", port->np);

	apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ACK_REQ, PORT_TUNCTRL);
	ret = read_poll_timeout_atomic(apple_pcie_port_readl, stat,
				       stat & PORT_TUNSTAT_PERST_ACK_PEND,
				       1000, 1000000, false, port, PORT_TUNSTAT);
	if (ret)
		dev_warn(pcie->dev, "port %pOF tunnel reset acknowledgment timed out\n",
			 port->np);
	apple_pcie_port_rmw_clear(port, PORT_TUNCTRL_PERST_ACK_REQ,
				  PORT_TUNCTRL);
	port->started = false;
}

static void apple_pcie_port_teardown(struct apple_pcie_port *port)
{
	if (port->base)
		apple_pcie_port_writel(port, ~0, PORT_INTMSK);

	if (port->pcie->hw->tunneled && port->started)
		apple_pcie_tunnel_stop(port);

	apple_pcie_port_unregister_irqs(port);

	if (port->irq) {
		irq_set_chained_handler_and_data(port->irq, NULL, NULL);
		irq_dispose_mapping(port->irq);
		port->irq = 0;
	}

	if (port->domain) {
		irq_domain_remove(port->domain);
		port->domain = NULL;
	}

	if (!list_empty(&port->entry))
		list_del_init(&port->entry);

	if (port->np) {
		of_node_put(port->np);
		port->np = NULL;
	}
}

static int apple_pcie_setup_refclk(struct apple_pcie *pcie,
				   struct apple_pcie_port *port)
{
	u32 stat;
	int res;

	if (pcie->hw->phy_lane_ctl)
		rmw_set(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	rmw_set(PHY_LANE_CFG_REFCLK0REQ, port->phy + PHY_LANE_CFG);

	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK0ACK,
					 100, 50000);
	if (res < 0)
		return res;

	rmw_set(PHY_LANE_CFG_REFCLK1REQ, port->phy + PHY_LANE_CFG);
	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK1ACK,
					 100, 50000);

	if (res < 0)
		return res;

	if (pcie->hw->phy_lane_ctl)
		rmw_clear(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	rmw_set(PHY_LANE_CFG_REFCLKEN, port->phy + PHY_LANE_CFG);

	if (pcie->hw->port_refclk)
		rmw_set(PORT_REFCLK_EN, port->base + pcie->hw->port_refclk);

	return 0;
}

static u32 port_rid2sid_offset(struct apple_pcie_port *port, int idx)
{
	return port->pcie->hw->port_rid2sid + 4 * idx;
}

static u32 apple_pcie_rid2sid_write(struct apple_pcie_port *port,
				    int idx, u32 val)
{
	u32 offset = port_rid2sid_offset(port, idx);

	apple_pcie_port_writel(port, val, offset);
	/* Read back to ensure completion of the write */
	return apple_pcie_port_readl(port, offset);
}

static int apple_pcie_setup_link(struct apple_pcie *pcie,
				 struct apple_pcie_port *port,
				 struct device_node *np)
{
#define MAX_AUX_PERST 3
	struct gpio_desc *aux_reset[MAX_AUX_PERST] = { NULL };
	u32 num_aux_resets = 0;
	struct gpio_desc *reset, *pwren = NULL;
	u32 stat;
	int ret;

	/*
	 * Assert PERST# and configure the pin as output.
	 * The Aquantia AQC113 10GB nic used desktop macs is sensitive to
	 * deasserting it without prior clock setup.
	 * Observed on M1 Max/Ultra Mac Studios under m1n1's hypervisor.
	 */
	reset = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_HIGH, "PERST#");
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	// HACK: use additional "reset-gpios" until pci-pwrctrl gains PERST# support.
	for (u32 idx = 0; idx < MAX_AUX_PERST; idx++) {
		aux_reset[idx] = devm_fwnode_gpiod_get_index(pcie->dev,
							     of_fwnode_handle(np),
							     "reset", idx + 1,
							     GPIOD_OUT_HIGH,
							     "PERST#");
		if (IS_ERR(aux_reset[idx])) {
			if (PTR_ERR(aux_reset[idx]) == -ENOENT)
				break;
			else
				return PTR_ERR(aux_reset[idx]);
		}
		num_aux_resets++;
	}
	dev_info(pcie->dev, "Using %u auxiliary PERST#\n", num_aux_resets);

	pwren = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "pwren",
					    GPIOD_ASIS, "PWREN");
	if (IS_ERR(pwren)) {
		if (PTR_ERR(pwren) == -ENOENT)
			pwren = NULL;
		else
			return PTR_ERR(pwren);
	}

	rmw_set(PORT_APPCLK_EN, port->base + PORT_APPCLK);

	/* Assert PERST# before setting up the clock */
	gpiod_set_value_cansleep(reset, 1);
	for (u32 idx = 0; idx < num_aux_resets; idx++)
		gpiod_set_value_cansleep(aux_reset[idx], 1);

	/* Power on the device if required */
	gpiod_set_value_cansleep(pwren, 1);

	ret = apple_pcie_setup_refclk(pcie, port);
	if (ret < 0)
		return ret;

	/*
	 * The minimal Tperst-clk value is 100us (PCIe CEM r5.0, 2.9.2)
	 * If powering up, the minimal Tpvperl is 100ms
	 */
	if (pwren)
		msleep(100);
	else
		usleep_range(100, 200);

	/* Deassert PERST# */
	rmw_set(PORT_PERST_OFF, port->base + pcie->hw->port_perst);
	gpiod_set_value_cansleep(reset, 0);
	for (u32 idx = 0; idx < num_aux_resets; idx++)
		gpiod_set_value_cansleep(aux_reset[idx], 0);

	/* Wait for 100ms after PERST# deassertion (PCIe r5.0, 6.6.1) */
	msleep(100);

	ret = readl_relaxed_poll_timeout(port->base + PORT_STATUS, stat,
					 stat & PORT_STATUS_READY, 100, 250000);
	if (ret < 0) {
		dev_err(pcie->dev, "port %pOF ready wait timeout\n", np);
		return ret;
	}

	return 0;
}

static int apple_pcie_setup_port(struct apple_pcie *pcie,
				 struct device_node *np)
{
	struct platform_device *platform = to_platform_device(pcie->dev);
	struct apple_pcie_port *port;
	struct resource *res;
	char name[16];
	u32 link_stat, preinit_status, stat, idx;
	int ret, i;

	port = devm_kzalloc(pcie->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->sid_map = devm_bitmap_zalloc(pcie->dev, pcie->hw->max_rid2sid, GFP_KERNEL);
	if (!port->sid_map)
		return -ENOMEM;

	ret = of_property_read_u32_index(np, "reg", 0, &idx);
	if (ret)
		return ret;

	/* Use the first reg entry to work out the port index */
	port->idx = idx >> 11;
	port->pcie = pcie;
	port->np = of_node_get(np);

	raw_spin_lock_init(&port->lock);
	INIT_LIST_HEAD(&port->entry);

	snprintf(name, sizeof(name), "port%d", port->idx);
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	if (!res)
		res = platform_get_resource(platform, IORESOURCE_MEM, port->idx + 2);
	if (!res) {
		ret = -ENODEV;
		goto err_teardown;
	}

	/*
	 * PCIe-C register transactions are non-posted on the tunneled fabric.
	 * Mark the resource so devm_ioremap_resource() selects ioremap_np() on
	 * arm64, matching the successful /dev/mem Device-nGnRnE replay.
	 */
	if (pcie->hw->tunneled)
		res->flags |= IORESOURCE_MEM_NONPOSTED;

	port->base = devm_ioremap_resource(&platform->dev, res);
	if (IS_ERR(port->base)) {
		ret = PTR_ERR(port->base);
		port->base = NULL;
		goto err_teardown;
	}

	if (!pcie->hw->tunneled) {
		snprintf(name, sizeof(name), "phy%d", port->idx);
		res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
		if (res)
			port->phy = devm_ioremap_resource(&platform->dev, res);
		else
			port->phy = pcie->base + CORE_PHY_DEFAULT_BASE(port->idx);
		if (IS_ERR(port->phy)) {
			ret = PTR_ERR(port->phy);
			port->phy = NULL;
			goto err_teardown;
		}
	}
	if (pcie->hw->tunneled) {
		/*
		 * m1n1 owns PCIe-C cold initialization. Replaying the port reset or
		 * tunnel-reset handshake against that live handoff raises an
		 * asynchronous SError on T6020. Accept only an explicitly successful
		 * handoff and begin with the read-only RUN state check used by m1n1.
		 */
		ret = of_property_read_u32(pcie->dev->of_node,
					   "apple,pciec-preinit-status",
					   &preinit_status);
		if (ret || preinit_status != 1) {
			dev_err(pcie->dev,
				"PCIe-C requires a successful m1n1 preinit handoff\n");
			ret = -ENODEV;
			goto err_teardown;
		}

		ret = read_poll_timeout_atomic(apple_pcie_port_readl, stat,
					       stat & PORT_STATUS_READY,
					       100, 250000, false, port,
					       PORT_STATUS);
		if (ret < 0) {
			dev_err(pcie->dev,
				"port %pOF ready wait timeout\n", np);
			goto err_teardown;
		}
	} else {
		/* U-Boot may already have brought up a conventional root port. */
		link_stat = apple_pcie_port_readl(port, PORT_LINKSTS);
		if (!(link_stat & PORT_LINKSTS_UP)) {
			ret = apple_pcie_setup_link(pcie, port, np);
			if (ret)
				goto err_teardown;
		}
	}

	if (pcie->hw->port_refclk)
		rmw_clear(PORT_REFCLK_CGDIS, port->base + pcie->hw->port_refclk);
	else if (port->phy)
		rmw_set(PHY_LANE_CFG_REFCLKCGEN, port->phy + PHY_LANE_CFG);

	/* The preinitialized PCIe-C APPCLK state is part of the m1n1 handoff. */
	if (!pcie->hw->tunneled)
		rmw_clear(PORT_APPCLK_CGDIS, port->base + PORT_APPCLK);

	ret = apple_pcie_port_setup_irq(port);
	if (ret)
		goto err_teardown;

	/* Reset all RID/SID mappings, and check for RAZ/WI registers */
	for (i = 0; i < pcie->hw->max_rid2sid; i++) {
		if (apple_pcie_rid2sid_write(port, i, 0xbad1d) != 0xbad1d)
			break;
		apple_pcie_rid2sid_write(port, i, 0);
	}

	dev_dbg(pcie->dev, "%pOF: %d RID/SID mapping entries\n", np, i);

	port->sid_map_sz = i;

	list_add_tail(&port->entry, &pcie->ports);
	init_completion(&pcie->event);

	ret = apple_pcie_port_register_irqs(port);
	if (ret)
		goto err_teardown;

	link_stat = apple_pcie_port_readl(port, PORT_LINKSTS);
	if (!(link_stat & PORT_LINKSTS_UP)) {
		unsigned long timeout, left;

		if (pcie->hw->tunneled) {
			ret = apple_pcie_tunnel_release_reset(port);
			if (ret)
				goto err_teardown;
		}

		/* start link training */
		apple_pcie_port_writel(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);

		timeout = link_up_timeout * HZ / 1000;
		left = wait_for_completion_timeout(&pcie->event, timeout);
		if (!left)
			dev_warn(pcie->dev, "%pOF link didn't come up\n", np);
		else
			dev_info(pcie->dev, "%pOF link up after %ldms\n", np,
				 (timeout - left) * 1000 / HZ);

	}

	return 0;

err_teardown:
	apple_pcie_port_teardown(port);
	return ret;
}

static const struct msi_parent_ops apple_msi_parent_ops = {
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX		|
				   MSI_FLAG_MULTI_PCI_MSI),
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   MSI_FLAG_PCI_MSI_MASK_PARENT),
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

static int apple_msi_init(struct apple_pcie *pcie)
{
	struct fwnode_handle *fwnode = dev_fwnode(pcie->dev);
	struct irq_domain_info info = {
		.fwnode		= fwnode,
		.ops		= &apple_msi_domain_ops,
		.size		= pcie->nvecs,
		.host_data	= pcie,
	};
	struct of_phandle_args args = {};
	int ret;

	ret = of_parse_phandle_with_args(to_of_node(fwnode), "msi-ranges",
					 "#interrupt-cells", 0, &args);
	if (ret)
		return ret;

	ret = of_property_read_u32_index(to_of_node(fwnode), "msi-ranges",
					 args.args_count + 1, &pcie->nvecs);
	if (ret) {
		of_node_put(args.np);
		return ret;
	}

	of_phandle_args_to_fwspec(args.np, args.args, args.args_count,
				  &pcie->fwspec);
	of_node_put(args.np);

	pcie->bitmap = devm_bitmap_zalloc(pcie->dev, pcie->nvecs, GFP_KERNEL);
	if (!pcie->bitmap)
		return -ENOMEM;

	info.parent = irq_find_matching_fwspec(&pcie->fwspec, DOMAIN_BUS_WIRED);
	if (!info.parent) {
		dev_err(pcie->dev, "failed to find parent domain\n");
		return -ENXIO;
	}

	pcie->msi_domain = msi_create_parent_irq_domain(&info, &apple_msi_parent_ops);
	if (!pcie->msi_domain) {
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}
	return 0;
}

static void apple_pcie_cleanup(void *data)
{
	struct apple_pcie *pcie = data;
	struct apple_pcie_port *port, *tmp;

	list_for_each_entry_safe(port, tmp, &pcie->ports, entry)
		apple_pcie_port_teardown(port);

	if (pcie->msi_domain) {
		irq_domain_remove(pcie->msi_domain);
		pcie->msi_domain = NULL;
	}
}

static struct apple_pcie *apple_pcie_lookup(struct device *dev)
{
	return pci_host_bridge_priv(dev_get_drvdata(dev));
}

static struct apple_pcie_port *apple_pcie_get_port(struct pci_dev *pdev)
{
	struct pci_config_window *cfg = pdev->sysdata;
	struct apple_pcie *pcie;
	struct pci_dev *port_pdev;
	struct apple_pcie_port *port;

	pcie = apple_pcie_lookup(cfg->parent);
	if (WARN_ON(!pcie))
		return NULL;

	/* Find the root port this device is on */
	port_pdev = pcie_find_root_port(pdev);

	/* If finding the port itself, nothing to do */
	if (WARN_ON(!port_pdev) || pdev == port_pdev)
		return NULL;

	list_for_each_entry(port, &pcie->ports, entry) {
		if (port->idx == PCI_SLOT(port_pdev->devfn))
			return port;
	}

	return NULL;
}

static int apple_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	struct apple_pcie *pcie = pci_host_bridge_priv(bridge);
	struct resource *res;
	u32 sid, rid = pci_dev_id(pdev);
	struct apple_pcie_port *port;
	int idx, err;

	/*
	 * Endpoint BARs share PCIe-C's tunneled, non-posted MMIO fabric. Mark
	 * them before the function driver maps its BAR so pci_iomap() selects
	 * Device-nGnRnE on arm64.
	 */
	if (pcie->hw->tunneled)
		pci_dev_for_each_resource(pdev, res)
			if (res->flags & IORESOURCE_MEM)
				res->flags |= IORESOURCE_MEM_NONPOSTED;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return 0;

	dev_dbg(&pdev->dev, "added to bus %s, index %d\n",
		pci_name(pdev->bus->self), port->idx);

	err = of_map_id(port->pcie->dev->of_node, rid, "iommu-map",
			"iommu-map-mask", NULL, &sid);
	if (err)
		return err;

	mutex_lock(&port->pcie->lock);

	idx = bitmap_find_free_region(port->sid_map, port->sid_map_sz, 0);
	if (idx >= 0) {
		apple_pcie_rid2sid_write(port, idx,
					 PORT_RID2SID_VALID |
					 (sid << PORT_RID2SID_SID_SHIFT) | rid);

		dev_dbg(&pdev->dev, "mapping RID%x to SID%x (index %d)\n",
			rid, sid, idx);
	}

	mutex_unlock(&port->pcie->lock);

	return idx >= 0 ? 0 : -ENOSPC;
}

static void apple_pcie_disable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	struct apple_pcie_port *port;
	u32 rid = pci_dev_id(pdev);
	int idx;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return;

	mutex_lock(&port->pcie->lock);

	for_each_set_bit(idx, port->sid_map, port->sid_map_sz) {
		u32 val;

		val = apple_pcie_port_readl(port,
					    port_rid2sid_offset(port, idx));
		if ((val & 0xffff) == rid) {
			apple_pcie_rid2sid_write(port, idx, 0);
			bitmap_release_region(port->sid_map, idx, 0);
			dev_dbg(&pdev->dev, "Released %x (%d)\n", val, idx);
			break;
		}
	}

	mutex_unlock(&port->pcie->lock);
}

static int apple_pcie_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct apple_pcie *pcie;
	int ret;

	pcie = apple_pcie_lookup(dev);
	if (WARN_ON(!pcie))
		return -ENOENT;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		ret = apple_pcie_setup_port(pcie, of_port);
		if (ret) {
			dev_err(dev, "Port %pOF setup fail: %d\n", of_port, ret);
			return ret;
		}
	}

	return 0;
}

static const struct pci_ecam_ops apple_pcie_cfg_ecam_ops = {
	.init		= apple_pcie_init,
	.enable_device	= apple_pcie_enable_device,
	.disable_device	= apple_pcie_disable_device,
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

static int apple_pcie_probe_port(struct device_node *np)
{
	struct gpio_desc *gd;

	/* check whether the GPPIO pin exists but leave it as is */
	gd = fwnode_gpiod_get_index(of_fwnode_handle(np), "reset", 0,
				    GPIOD_ASIS, "PERST#");
	if (IS_ERR(gd))
		return PTR_ERR(gd);

	gpiod_put(gd);

	gd = fwnode_gpiod_get_index(of_fwnode_handle(np), "pwren", 0,
				    GPIOD_ASIS, "PWREN");
	if (IS_ERR(gd)) {
		if (PTR_ERR(gd) != -ENOENT)
			return PTR_ERR(gd);
	} else {
		gpiod_put(gd);
	}

	return 0;
}

static int apple_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct hw_info *hw;
	struct pci_host_bridge *bridge;
	struct device_node *of_port;
	struct apple_pcie *pcie;
	int ret;

	hw = of_device_get_match_data(dev);
	if (!hw)
		return -ENODEV;

	/*
	 * A tunneled PCIe-C port has no host GPIO for PERST#: the tunnel
	 * firmware and m1n1 handoff own its reset state.  Requiring a
	 * reset-gpios provider here rejects the valid synthesized port before
	 * the tunneled setup path can consume that handoff.
	 */
	if (!hw->tunneled) {
		/* Check for probe dependencies for all ports first */
		for_each_available_child_of_node(dev->of_node, of_port) {
			ret = apple_pcie_probe_port(of_port);
			if (ret) {
				of_node_put(of_port);
				return dev_err_probe(dev, ret,
						     "Port %pOF probe fail\n", of_port);
			}
		}
	}

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	pcie->hw = hw;
	pcie->base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	mutex_init(&pcie->lock);
	INIT_LIST_HEAD(&pcie->ports);

	ret = apple_msi_init(pcie);
	if (ret)
		return ret;

	ret = pci_host_common_init(pdev, bridge, &apple_pcie_cfg_ecam_ops);
	if (ret)
		apple_pcie_cleanup(pcie);
	if (ret)
		return ret;

	/*
	 * Port mappings are allocated by the ECAM init callback. Register cleanup
	 * afterwards so devres runs it before unmapping those registers.
	 */
	return devm_add_action_or_reset(dev, apple_pcie_cleanup, pcie);
}

static void apple_pcie_remove(struct platform_device *pdev)
{
	pci_host_common_remove(pdev);
}

static const struct of_device_id apple_pcie_of_match[] = {
	{ .compatible = "apple,t8103-pciec",	.data = &t8103_pciec_hw },
	{ .compatible = "apple,t6000-pciec",	.data = &t8103_pciec_hw },
	{ .compatible = "apple,t6020-pcie",	.data = &t602x_hw },
	{ .compatible = "apple,pcie",		.data = &t8103_hw },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pcie_of_match);

static struct platform_driver apple_pcie_driver = {
	.probe	= apple_pcie_probe,
	.remove	= apple_pcie_remove,
	.driver	= {
		.name			= "pcie-apple",
		.of_match_table		= apple_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(apple_pcie_driver);

MODULE_DESCRIPTION("Apple PCIe host bridge driver");
MODULE_LICENSE("GPL v2");
