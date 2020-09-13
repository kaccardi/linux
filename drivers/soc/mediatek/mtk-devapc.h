/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 MediaTek Inc.
 */

#ifndef __MTK_DEVAPC_H__
#define __MTK_DEVAPC_H__

#define VIO_MOD_TO_REG_IND(m)	((m) / 32)
#define VIO_MOD_TO_REG_OFF(m)	((m) % 32)

struct mtk_devapc_pd_offset {
	u32 vio_mask;
	u32 vio_sta;
	u32 vio_dbg0;
	u32 vio_dbg1;
	u32 apc_con;
	u32 vio_shift_sta;
	u32 vio_shift_sel;
	u32 vio_shift_con;
};

struct mtk_devapc_vio_dbgs_desc {
	u32 mask;
	u32 start;
};

struct mtk_devapc_vio_dbgs {
	struct mtk_devapc_vio_dbgs_desc mstid;
	struct mtk_devapc_vio_dbgs_desc dmnid;
	struct mtk_devapc_vio_dbgs_desc vio_w;
	struct mtk_devapc_vio_dbgs_desc vio_r;
	struct mtk_devapc_vio_dbgs_desc addr_h;
};

struct mtk_devapc_vio_info {
	bool read;
	bool write;
	u32 vio_addr;
	u32 vio_addr_high;
	u32 master_id;
	u32 domain_id;
};

struct mtk_devapc_context {
	struct device *dev;
	u32 vio_idx_num;
	void __iomem *devapc_pd_base;
	struct mtk_devapc_vio_info *vio_info;
	const struct mtk_devapc_pd_offset *offset;
	const struct mtk_devapc_vio_dbgs *vio_dbgs;
};

#endif /* __MTK_DEVAPC_H__ */
