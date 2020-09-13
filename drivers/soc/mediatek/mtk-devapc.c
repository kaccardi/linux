// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include "mtk-devapc.h"

static u32 get_shift_group(struct mtk_devapc_context *ctx, u32 vio_idx)
{
	u32 vio_shift_sta;
	void __iomem *reg;

	reg = ctx->devapc_pd_base + ctx->offset->vio_shift_sta;
	vio_shift_sta = readl(reg);

	if (vio_shift_sta)
		return __ffs(vio_shift_sta);

	return 31;
}

static int check_vio_mask_sta(struct mtk_devapc_context *ctx, u32 module,
			      u32 offset)
{
	void __iomem *reg;
	u32 value;

	reg = ctx->devapc_pd_base + offset;
	reg += 0x4 * VIO_MOD_TO_REG_IND(module);

	value = readl(reg);

	return ((value >> VIO_MOD_TO_REG_OFF(module)) & 0x1);
}

static int check_vio_mask(struct mtk_devapc_context *ctx, u32 module)
{
	return check_vio_mask_sta(ctx, module, ctx->offset->vio_mask);
}

static int check_vio_status(struct mtk_devapc_context *ctx, u32 module)
{
	return check_vio_mask_sta(ctx, module, ctx->offset->vio_sta);
}

static void clear_vio_status(struct mtk_devapc_context *ctx, u32 module)
{
	void __iomem *reg;

	reg = ctx->devapc_pd_base + ctx->offset->vio_sta;
	reg += 0x4 * VIO_MOD_TO_REG_IND(module);

	writel(0x1 << VIO_MOD_TO_REG_OFF(module), reg);

	if (check_vio_status(ctx, module))
		dev_err(ctx->dev, "%s: Clear failed, module_index:0x%x\n",
			__func__, module);
}

static void mask_module_irq(struct mtk_devapc_context *ctx, u32 module,
			    bool mask)
{
	void __iomem *reg;
	u32 value;

	reg = ctx->devapc_pd_base + ctx->offset->vio_mask;
	reg += 0x4 * VIO_MOD_TO_REG_IND(module);

	value = readl(reg);
	if (mask)
		value |= (0x1 << VIO_MOD_TO_REG_OFF(module));
	else
		value &= ~(0x1 << VIO_MOD_TO_REG_OFF(module));

	writel(value, reg);
}

#define PHY_DEVAPC_TIMEOUT	0x10000

/*
 * sync_vio_dbg - do "shift" mechansim" to get full violation information.
 *                shift mechanism is depends on devapc hardware design.
 *                Mediatek devapc set multiple slaves as a group. When violation
 *                is triggered, violation info is kept inside devapc hardware.
 *                Driver should do shift mechansim to "shift" full violation
 *                info to VIO_DBGs registers.
 *
 */
static int sync_vio_dbg(struct mtk_devapc_context *ctx, u32 shift_bit)
{
	void __iomem *pd_vio_shift_sta_reg;
	void __iomem *pd_vio_shift_sel_reg;
	void __iomem *pd_vio_shift_con_reg;
	int ret;
	u32 val;

	pd_vio_shift_sta_reg = ctx->devapc_pd_base + ctx->offset->vio_shift_sta;
	pd_vio_shift_sel_reg = ctx->devapc_pd_base + ctx->offset->vio_shift_sel;
	pd_vio_shift_con_reg = ctx->devapc_pd_base + ctx->offset->vio_shift_con;

	/* Enable shift mechansim */
	writel(0x1 << shift_bit, pd_vio_shift_sel_reg);
	writel(0x1, pd_vio_shift_con_reg);

	ret = readl_poll_timeout(pd_vio_shift_con_reg, val, val & 0x3, 1000,
				 PHY_DEVAPC_TIMEOUT);
	if (ret)
		dev_err(ctx->dev, "%s: Shift violation info failed\n",
			__func__);

	/* Disable shift mechanism */
	writel(0x0, pd_vio_shift_con_reg);
	writel(0x0, pd_vio_shift_sel_reg);
	writel(0x1 << shift_bit, pd_vio_shift_sta_reg);

	return ret;
}

static void devapc_vio_info_print(struct mtk_devapc_context *ctx)
{
	struct mtk_devapc_vio_info *vio_info = ctx->vio_info;

	/* Print violation information */
	if (vio_info->write)
		dev_info(ctx->dev, "Write Violation\n");
	else if (vio_info->read)
		dev_info(ctx->dev, "Read Violation\n");

	dev_info(ctx->dev, "Vio Addr:0x%x, High:0x%x, Bus ID:0x%x, Dom ID:%x\n",
		 vio_info->vio_addr, vio_info->vio_addr_high,
		 vio_info->master_id, vio_info->domain_id);
}

/*
 * devapc_extract_vio_dbg - extract full violation information after doing
 *                          shift mechanism.
 */
static void devapc_extract_vio_dbg(struct mtk_devapc_context *ctx)
{
	const struct mtk_devapc_vio_dbgs *vio_dbgs;
	struct mtk_devapc_vio_info *vio_info;
	void __iomem *vio_dbg0_reg;
	void __iomem *vio_dbg1_reg;
	u32 dbg0;

	vio_dbg0_reg = ctx->devapc_pd_base + ctx->offset->vio_dbg0;
	vio_dbg1_reg = ctx->devapc_pd_base + ctx->offset->vio_dbg1;

	vio_dbgs = ctx->vio_dbgs;
	vio_info = ctx->vio_info;

	/* Starts to extract violation information */
	dbg0 = readl(vio_dbg0_reg);
	vio_info->vio_addr = readl(vio_dbg1_reg);

	vio_info->master_id = (dbg0 & vio_dbgs->mstid.mask) >>
			      vio_dbgs->mstid.start;
	vio_info->domain_id = (dbg0 & vio_dbgs->dmnid.mask) >>
			      vio_dbgs->dmnid.start;
	vio_info->write = ((dbg0 & vio_dbgs->vio_w.mask) >>
			    vio_dbgs->vio_w.start) == 1;
	vio_info->read = ((dbg0 & vio_dbgs->vio_r.mask) >>
			  vio_dbgs->vio_r.start) == 1;
	vio_info->vio_addr_high = (dbg0 & vio_dbgs->addr_h.mask) >>
				  vio_dbgs->addr_h.start;

	devapc_vio_info_print(ctx);
}

/*
 * mtk_devapc_dump_vio_dbg - get the violation index and dump the full violation
 *                           debug information.
 */
static bool mtk_devapc_dump_vio_dbg(struct mtk_devapc_context *ctx, u32 vio_idx)
{
	u32 shift_bit;

	if (check_vio_mask(ctx, vio_idx))
		return false;

	if (!check_vio_status(ctx, vio_idx))
		return false;

	shift_bit = get_shift_group(ctx, vio_idx);

	if (sync_vio_dbg(ctx, shift_bit))
		return false;

	devapc_extract_vio_dbg(ctx);

	return true;
}

/*
 * devapc_violation_irq - the devapc Interrupt Service Routine (ISR) will dump
 *                        violation information including which master violates
 *                        access slave.
 */
static irqreturn_t devapc_violation_irq(int irq_number,
					struct mtk_devapc_context *ctx)
{
	u32 vio_idx;

	for (vio_idx = 0; vio_idx < ctx->vio_idx_num; vio_idx++) {
		if (!mtk_devapc_dump_vio_dbg(ctx, vio_idx))
			continue;

		/* Ensure that violation info are written before
		 * further operations
		 */
		smp_mb();

		/*
		 * Mask slave's irq before clearing vio status.
		 * Must do it to avoid nested interrupt and prevent
		 * unexpected behavior.
		 */
		mask_module_irq(ctx, vio_idx, true);

		clear_vio_status(ctx, vio_idx);

		mask_module_irq(ctx, vio_idx, false);
	}

	return IRQ_HANDLED;
}

/*
 * start_devapc - initialize devapc status and start receiving interrupt
 *                while devapc violation is triggered.
 */
static int start_devapc(struct mtk_devapc_context *ctx)
{
	void __iomem *pd_vio_shift_sta_reg;
	void __iomem *pd_apc_con_reg;
	u32 vio_shift_sta;
	u32 vio_idx;

	pd_apc_con_reg = ctx->devapc_pd_base + ctx->offset->apc_con;
	pd_vio_shift_sta_reg = ctx->devapc_pd_base + ctx->offset->vio_shift_sta;
	if (!pd_apc_con_reg || !pd_vio_shift_sta_reg)
		return -EINVAL;

	/* Clear devapc violation status */
	writel(BIT(31), pd_apc_con_reg);

	/* Clear violation shift status */
	vio_shift_sta = readl(pd_vio_shift_sta_reg);
	if (vio_shift_sta)
		writel(vio_shift_sta, pd_vio_shift_sta_reg);

	/* Clear slave violation status */
	for (vio_idx = 0; vio_idx < ctx->vio_idx_num; vio_idx++) {
		clear_vio_status(ctx, vio_idx);
		mask_module_irq(ctx, vio_idx, false);
	}

	return 0;
}

static const struct mtk_devapc_pd_offset mt6779_pd_offset = {
	.vio_mask = 0x0,
	.vio_sta = 0x400,
	.vio_dbg0 = 0x900,
	.vio_dbg1 = 0x904,
	.apc_con = 0xF00,
	.vio_shift_sta = 0xF10,
	.vio_shift_sel = 0xF14,
	.vio_shift_con = 0xF20,
};

static const struct mtk_devapc_vio_dbgs mt6779_vio_dbgs = {
	.mstid =  {0x0000FFFF, 0x0},
	.dmnid =  {0x003F0000, 0x10},
	.vio_w =  {0x00400000, 0x16},
	.vio_r =  {0x00800000, 0x17},
	.addr_h = {0x0F000000, 0x18},
};

static const struct mtk_devapc_context devapc_mt6779 = {
	.vio_idx_num = 510,
	.offset = &mt6779_pd_offset,
	.vio_dbgs = &mt6779_vio_dbgs,
};

static const struct of_device_id mtk_devapc_dt_match[] = {
	{
		.compatible = "mediatek,mt6779-devapc",
		.data = &devapc_mt6779,
	}, {
	},
};

static int mtk_devapc_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct mtk_devapc_context *ctx;
	struct clk *devapc_infra_clk;
	u32 devapc_irq;
	int ret;

	if (IS_ERR(node))
		return -ENODEV;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx = (struct mtk_devapc_context *)of_device_get_match_data(&pdev->dev);
	ctx->dev = &pdev->dev;

	ctx->vio_info = devm_kzalloc(&pdev->dev,
				     sizeof(struct mtk_devapc_vio_info),
				     GFP_KERNEL);
	if (!ctx->vio_info)
		return -ENOMEM;

	ctx->devapc_pd_base = of_iomap(node, 0);
	if (!ctx->devapc_pd_base)
		return -EINVAL;

	devapc_irq = irq_of_parse_and_map(node, 0);
	if (!devapc_irq)
		return -EINVAL;

	devapc_infra_clk = devm_clk_get(&pdev->dev, "devapc-infra-clock");
	if (IS_ERR(devapc_infra_clk))
		return -EINVAL;

	if (clk_prepare_enable(devapc_infra_clk))
		return -EINVAL;

	ret = start_devapc(ctx);
	if (ret)
		return ret;

	ret = devm_request_irq(&pdev->dev, devapc_irq,
			       (irq_handler_t)devapc_violation_irq,
			       IRQF_TRIGGER_NONE, "devapc", ctx);
	if (ret)
		return ret;

	return 0;
}

static int mtk_devapc_remove(struct platform_device *dev)
{
	return 0;
}

static struct platform_driver mtk_devapc_driver = {
	.probe = mtk_devapc_probe,
	.remove = mtk_devapc_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mtk_devapc_dt_match,
	},
};

module_platform_driver(mtk_devapc_driver);

MODULE_DESCRIPTION("Mediatek Device APC Driver");
MODULE_AUTHOR("Neal Liu <neal.liu@mediatek.com>");
MODULE_LICENSE("GPL");
