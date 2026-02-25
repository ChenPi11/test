// SPDX-License-Identifier: GPL-2.0
/*
 * Linux UFS (Fast File System) driver – inode operations
 * Inspired by OpenBSD sys/ufs/ffs/ffs_inode.c and sys/ufs/ufs/ufs_vnops.c
 *
 * Handles reading on-disk inodes into the VFS inode structure and mapping
 * logical file blocks to physical fragment addresses.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>
#include <linux/stat.h>
#include <linux/time.h>

#include "ufs.h"
#include "ufs_fs.h"

/* ------------------------------------------------------------------ */
/* Block mapping                                                         */
/* ------------------------------------------------------------------ */

/*
 * Map logical block number @iblock (in units of sb->s_blocksize = fs_bsize)
 * to the physical block number on disk (also in units of fs_bsize).
 *
 * UFS stores block pointers as fragment addresses; divide by fs_frag to get
 * the Linux block number.
 *
 * Returns the block number, or 0 if the block is a hole / unmapped.
 *
 * Supports direct blocks and a single level of indirection.
 */
u64 ufs_block_map(struct inode *inode, sector_t iblock)
{
	struct ufs_sb_info   *sbi = UFS_SB(inode->i_sb);
	struct ufs_inode_info *ui = UFS_I(inode);
	u64 frag_addr;

	if (iblock < UFS_NDADDR) {
		/* Direct block: di_db[i] is a fragment address */
		if (sbi->fs_ufs2)
			frag_addr = le64_to_cpu(ui->i_u.i2.db[iblock]);
		else
			frag_addr = le32_to_cpu(ui->i_u.i1.db[iblock]);
		if (!frag_addr)
			return 0;
		/* Convert fragment address → block number */
		return frag_addr / sbi->fs_frag;
	}

	/* Single indirect: block index within the indirect block */
	{
		sector_t ind_off = iblock - UFS_NDADDR;
		u64 ind_frag;
		struct buffer_head *bh;

		if (ind_off >= sbi->fs_nindir)
			return 0;	/* beyond single-indirect range */

		if (sbi->fs_ufs2)
			ind_frag = le64_to_cpu(ui->i_u.i2.ib[0]);
		else
			ind_frag = le32_to_cpu(ui->i_u.i1.ib[0]);

		if (!ind_frag)
			return 0;	/* hole */

		/* Read the indirect block (using its block number) */
		bh = sb_bread(inode->i_sb, ind_frag / sbi->fs_frag);
		if (!bh)
			return 0;

		if (sbi->fs_ufs2) {
			__le64 *ptrs = (__le64 *)bh->b_data;
			frag_addr = le64_to_cpu(ptrs[ind_off]);
		} else {
			__le32 *ptrs = (__le32 *)bh->b_data;
			frag_addr = le32_to_cpu(ptrs[ind_off]);
		}
		brelse(bh);
		if (!frag_addr)
			return 0;
		return frag_addr / sbi->fs_frag;
	}
}

/*
 * get_block callback for mpage_read_folio / generic_file_read_iter.
 *
 * Maps logical block @iblock (in units of fs_bsize) to physical block number.
 * Sets bh_result appropriately; create=0 always (read-only).
 */
int ufs_get_block(struct inode *inode, sector_t iblock,
		  struct buffer_head *bh_result, int create)
{
	u64 phys_block;

	if (create)
		return -EROFS;

	phys_block = ufs_block_map(inode, iblock);
	if (phys_block)
		map_bh(bh_result, inode->i_sb, phys_block);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Page / folio I/O                                                     */
/* ------------------------------------------------------------------ */

static int ufs_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, ufs_get_block);
}

static void ufs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ufs_get_block);
}

const struct address_space_operations ufs_aops = {
	.read_folio	= ufs_read_folio,
	.readahead	= ufs_readahead,
};

/* ------------------------------------------------------------------ */
/* Inode attribute helper                                               */
/* ------------------------------------------------------------------ */

static int ufs_getattr(struct mnt_idmap *idmap, const struct path *path,
		       struct kstat *stat, u32 request_mask,
		       unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct ufs_inode_info *ui = UFS_I(inode);

	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = UFS_SB(inode->i_sb)->fs_fsize;
	/* Expose BSD flags via stat.attributes */
	stat->attributes      = ui->i_flags;
	stat->attributes_mask = ~0U;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Symlink helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Inline (short) symlinks are stored directly in the block-pointer region
 * of the inode (di_db / di_ib fields used as a char array).
 */
static const char *ufs_get_link(struct dentry *dentry, struct inode *inode,
				struct delayed_call *done)
{
	struct ufs_inode_info *ui = UFS_I(inode);
	struct ufs_sb_info    *sbi = UFS_SB(inode->i_sb);
	unsigned maxlen;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	maxlen = sbi->fs_ufs2 ? UFS2_MAXSYMLINKLEN : UFS1_MAXSYMLINKLEN;

	/* Short symlinks stored inline */
	if (inode->i_size <= maxlen) {
		return (const char *)ui->i_u.i1.db;
	}

	/* Long symlink: read from data pages */
	return page_get_link(dentry, inode, done);
}

/* ------------------------------------------------------------------ */
/* File / dir / symlink inode_operations and file_operations            */
/* ------------------------------------------------------------------ */

const struct inode_operations ufs_file_inode_ops = {
	.getattr = ufs_getattr,
};

const struct file_operations ufs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.mmap		= generic_file_mmap,
};

const struct inode_operations ufs_symlink_inode_ops = {
	.get_link	= ufs_get_link,
	.getattr	= ufs_getattr,
};

/* ------------------------------------------------------------------ */
/* Reading the on-disk inode                                            */
/* ------------------------------------------------------------------ */

/*
 * Fill a VFS inode from the on-disk UFS1 inode at *din.
 * Caller holds the inode lock (new inode).
 */
static void ufs1_fill_inode(struct inode *inode, struct ufs1_dinode *din)
{
	struct ufs_inode_info *ui = UFS_I(inode);
	u16 mode = le16_to_cpu(din->di_mode);

	inode->i_mode  = mode;
	set_nlink(inode, le16_to_cpu(din->di_nlink));
	i_uid_write(inode, le32_to_cpu(din->di_uid));
	i_gid_write(inode, le32_to_cpu(din->di_gid));
	inode->i_size  = le64_to_cpu(din->di_size);

	inode_set_atime(inode, (s32)le32_to_cpu(din->di_atime),
			le32_to_cpu(din->di_atimensec));
	inode_set_mtime(inode, (s32)le32_to_cpu(din->di_mtime),
			le32_to_cpu(din->di_mtimensec));
	inode_set_ctime(inode,
			(s32)le32_to_cpu(din->di_ctime),
			le32_to_cpu(din->di_ctimensec));

	inode->i_blocks = le32_to_cpu(din->di_blocks);
	inode->i_generation = le32_to_cpu(din->di_gen);
	ui->i_flags = le32_to_cpu(din->di_flags);

	memcpy(ui->i_u.i1.db, din->di_db, sizeof(din->di_db));
	memcpy(ui->i_u.i1.ib, din->di_ib, sizeof(din->di_ib));
}

/*
 * Fill a VFS inode from the on-disk UFS2 inode at *din.
 */
static void ufs2_fill_inode(struct inode *inode, struct ufs2_dinode *din)
{
	struct ufs_inode_info *ui = UFS_I(inode);
	u16 mode = le16_to_cpu(din->di_mode);

	inode->i_mode  = mode;
	set_nlink(inode, le16_to_cpu(din->di_nlink));
	i_uid_write(inode, le32_to_cpu(din->di_uid));
	i_gid_write(inode, le32_to_cpu(din->di_gid));
	inode->i_size  = le64_to_cpu(din->di_size);

	inode_set_atime(inode, le64_to_cpu(din->di_atime),
			le32_to_cpu(din->di_atimensec));
	inode_set_mtime(inode, le64_to_cpu(din->di_mtime),
			le32_to_cpu(din->di_mtimensec));
	inode_set_ctime(inode,
			le64_to_cpu(din->di_ctime),
			le32_to_cpu(din->di_ctimensec));

	inode->i_blocks    = le64_to_cpu(din->di_blocks) >> 9; /* bytes -> 512-B */
	inode->i_generation = le32_to_cpu(din->di_gen);
	ui->i_flags = le32_to_cpu(din->di_flags);

	memcpy(ui->i_u.i2.db, din->di_db, sizeof(din->di_db));
	memcpy(ui->i_u.i2.ib, din->di_ib, sizeof(din->di_ib));
}

/*
 * Set up the VFS inode's operations based on its file type.
 */
static void ufs_set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & UFS_IFMT) {
	case UFS_IFREG:
		inode->i_op   = &ufs_file_inode_ops;
		inode->i_fop  = &ufs_file_operations;
		inode->i_data.a_ops = &ufs_aops;
		break;
	case UFS_IFDIR:
		inode->i_op   = &ufs_dir_inode_ops;
		inode->i_fop  = &ufs_dir_operations;
		inode->i_data.a_ops = &ufs_aops;
		break;
	case UFS_IFLNK:
		inode->i_op   = &ufs_symlink_inode_ops;
		inode->i_data.a_ops = &ufs_aops;
		break;
	default:
		init_special_inode(inode, inode->i_mode,
				   (dev_t)le32_to_cpu(
					UFS_I(inode)->i_u.i1.db[0]));
		break;
	}
}

/*
 * ufs_iget – read inode @ino from disk and return a VFS inode.
 *
 * Algorithm (mirrors OpenBSD ffs_vget / ffs_read_inode):
 *   1. Compute the fragment address of the inode table block (fsba).
 *   2. Convert fsba to a block number: block = fsba / fs_frag.
 *   3. Compute the byte offset of the inode within that block.
 *   4. Read the block with sb_bread().
 *   5. Copy the on-disk dinode into the in-memory inode_info.
 *   6. Set up VFS operations.
 */
struct inode *ufs_iget(struct super_block *sb, unsigned long ino)
{
	struct ufs_sb_info *sbi = UFS_SB(sb);
	struct inode *inode;
	u64  fsba;		/* fragment address of inode table block */
	u32  fsbo;		/* inode index within that block (0..inopb-1) */
	u64  blk_num;		/* block number for sb_bread */
	u32  off_in_blk;	/* byte offset of inode within that block */
	struct buffer_head *bh;
	int ret = 0;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;		/* already cached */

	/* Calculate where the inode lives on disk */
	fsba = ufs_ino_to_fsba(sbi, ino);
	fsbo = ufs_ino_to_fsbo(sbi, ino);

	/*
	 * fsba is the fragment address of the fs_bsize-byte block holding the
	 * inodes.  sb->s_blocksize == fs_bsize, so the block number is:
	 *   blk_num    = fsba / fs_frag
	 * The byte offset of this inode within that block is:
	 *   off_in_blk = fsbo * sizeof(dinode)
	 */
	if (sbi->fs_ufs2) {
		blk_num    = fsba / sbi->fs_frag;
		off_in_blk = fsbo * sizeof(struct ufs2_dinode);
	} else {
		blk_num    = fsba / sbi->fs_frag;
		off_in_blk = fsbo * sizeof(struct ufs1_dinode);
	}

	pr_debug("ufs: iget ino=%lu fsba=%llu blk=%llu off=%u\n",
		 ino, fsba, blk_num, off_in_blk);

	bh = sb_bread(sb, blk_num);
	if (!bh) {
		pr_err("ufs: cannot read inode %lu (block %llu)\n",
		       ino, blk_num);
		ret = -EIO;
		goto bad_inode;
	}

	if (sbi->fs_ufs2) {
		struct ufs2_dinode *din =
			(struct ufs2_dinode *)(bh->b_data + off_in_blk);
		ufs2_fill_inode(inode, din);
	} else {
		struct ufs1_dinode *din =
			(struct ufs1_dinode *)(bh->b_data + off_in_blk);
		ufs1_fill_inode(inode, din);
	}
	brelse(bh);

	ufs_set_inode_ops(inode);
	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}
