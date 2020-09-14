// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, Red Hat, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_iunlink_item.h"
#include "xfs_trace.h"
#include "xfs_error.h"

struct kmem_cache	*xfs_iunlink_zone;

static inline struct xfs_iunlink_item *IUL_ITEM(struct xfs_log_item *lip)
{
	return container_of(lip, struct xfs_iunlink_item, iu_item);
}

static void
xfs_iunlink_item_release(
	struct xfs_log_item	*lip)
{
	kmem_cache_free(xfs_iunlink_zone, IUL_ITEM(lip));
}


static uint64_t
xfs_iunlink_item_sort(
	struct xfs_log_item	*lip)
{
	return IUL_ITEM(lip)->iu_ip->i_ino;
}

/*
 * Look up the inode cluster buffer and log the on-disk unlinked inode change
 * we need to make.
 */
static int
xfs_iunlink_log_inode(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_agino_t		old_agino,
	xfs_agino_t		next_agino)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_agnumber_t		agno = XFS_INO_TO_AGNO(mp, ip->i_ino);
	struct xfs_dinode	*dip;
	struct xfs_buf		*ibp;
	int			offset;
	int			error;

	ASSERT(xfs_verify_agino_or_null(mp, agno, next_agino));

	error = xfs_imap_to_bp(mp, tp, &ip->i_imap, &dip, &ibp, 0);
	if (error)
		return error;

	/*
	 * Don't bother updating the unlinked field on stale buffers as
	 * it will never get to disk anyway.
	 */
	if (ibp->b_flags & XBF_STALE)
		return 0;

	if (be32_to_cpu(dip->di_next_unlinked) != old_agino) {
		xfs_inode_verifier_error(ip, -EFSCORRUPTED, __func__, dip,
					sizeof(*dip), __this_address);
		xfs_trans_brelse(tp, ibp);
		return -EFSCORRUPTED;
	}

	trace_xfs_iunlink_update_dinode(mp, agno,
			XFS_INO_TO_AGINO(mp, ip->i_ino),
			be32_to_cpu(dip->di_next_unlinked), next_agino);

	dip->di_next_unlinked = cpu_to_be32(next_agino);
	offset = ip->i_imap.im_boffset +
			offsetof(struct xfs_dinode, di_next_unlinked);

	xfs_dinode_calc_crc(mp, dip);
	xfs_trans_inode_buf(tp, ibp);
	xfs_trans_log_buf(tp, ibp, offset, offset + sizeof(xfs_agino_t) - 1);
	return 0;
}

/*
 * On precommit, we grab the inode cluster buffer for the inode number
 * we were passed, then update the next unlinked field for that inode in
 * the buffer and log the buffer. This ensures that the inode cluster buffer
 * was logged in the correct order w.r.t. other inode cluster buffers.
 *
 * Note: if the inode cluster buffer is marked stale, this transaction is
 * actually freeing the inode cluster. In that case, do not relog the buffer
 * as this removes the stale state from it. That then causes the post-commit
 * processing that is dependent on the cluster buffer being stale to go wrong
 * and we'll leave stale inodes in the AIL that cannot be removed, hanging the
 * log.
 */
static int
xfs_iunlink_item_precommit(
	struct xfs_trans	*tp,
	struct xfs_log_item	*lip)
{
	struct xfs_iunlink_item	*iup = IUL_ITEM(lip);
	int			error;

	error = xfs_iunlink_log_inode(tp, iup->iu_ip, iup->iu_old_agino,
					iup->iu_next_agino);

	/*
	 * This log item only exists to perform this action. We now remove
	 * it from the transaction and free it as it should never reach the
	 * CIL.
	 */
	list_del(&lip->li_trans);
	xfs_iunlink_item_release(lip);
	return error;
}

static const struct xfs_item_ops xfs_iunlink_item_ops = {
	.iop_release	= xfs_iunlink_item_release,
	.iop_sort	= xfs_iunlink_item_sort,
	.iop_precommit	= xfs_iunlink_item_precommit,
};


/*
 * Initialize the inode log item for a newly allocated (in-core) inode.
 *
 * Inode extents can only reside within an AG. Hence specify the starting
 * block for the inode chunk by offset within an AG as well as the
 * length of the allocated extent.
 *
 * This joins the item to the transaction and marks it dirty so
 * that we don't need a separate call to do this, nor does the
 * caller need to know anything about the iunlink item.
 */
void
xfs_iunlink_log(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_agino_t		old_agino,
	xfs_agino_t		next_agino)
{
	struct xfs_iunlink_item	*iup;

	iup = kmem_cache_zalloc(xfs_iunlink_zone, GFP_KERNEL | __GFP_NOFAIL);

	xfs_log_item_init(tp->t_mountp, &iup->iu_item, XFS_LI_IUNLINK,
			  &xfs_iunlink_item_ops);

	iup->iu_ip = ip;
	iup->iu_next_agino = next_agino;
	iup->iu_old_agino = old_agino;

	xfs_trans_add_item(tp, &iup->iu_item);
	tp->t_flags |= XFS_TRANS_DIRTY;
	set_bit(XFS_LI_DIRTY, &iup->iu_item.li_flags);
}

