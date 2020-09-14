// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, Red Hat, Inc.
 * All Rights Reserved.
 */
#ifndef XFS_IUNLINK_ITEM_H
#define XFS_IUNLINK_ITEM_H	1

struct xfs_trans;
struct xfs_inode;

/* in memory log item structure */
struct xfs_iunlink_item {
	struct xfs_log_item	iu_item;
	struct xfs_inode	*iu_ip;
	xfs_agino_t		iu_next_agino;
	xfs_agino_t		iu_old_agino;
};

extern kmem_zone_t *xfs_iunlink_zone;

void xfs_iunlink_log(struct xfs_trans *tp, struct xfs_inode *ip,
			xfs_agino_t old_agino, xfs_agino_t next_agino);

#endif	/* XFS_IUNLINK_ITEM_H */
