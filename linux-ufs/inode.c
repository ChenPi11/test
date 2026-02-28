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
 * Read one block pointer from the indirect block at block number @blkno,
 * at index @idx within that block.
 *
 * UFS1 uses 32-bit pointers; UFS2 uses 64-bit pointers.
 * Returns the fragment address stored there, or 0 for a hole/error.
 *
 * This helper mirrors the inner loop of OpenBSD's ffs_indirtrunc().
 */
static u64 ufs_read_indir(struct super_block *sb, u64 blkno, u64 idx, int ufs2)
{
	struct buffer_head *bh;
	u64 result = 0;

	bh = sb_bread(sb, blkno);
	if (!bh)
		return 0;

	if (ufs2) {
		__le64 *ptrs = (__le64 *)bh->b_data;
		result = le64_to_cpu(ptrs[idx]);
	} else {
		__le32 *ptrs = (__le32 *)bh->b_data;
		result = le32_to_cpu(ptrs[idx]);
	}
	brelse(bh);
	return result;
}

/*
 * Map logical block number @iblock (in units of sb->s_blocksize = fs_bsize)
 * to the physical block number on disk (also in units of fs_bsize).
 *
 * UFS stores block pointers as fragment addresses; divide by fs_frag to get
 * the Linux block number.  Returns 0 for a hole / unmapped block.
 *
 * Mirrors the three indirection levels (SINGLE/DOUBLE/TRIPLE) described in
 * OpenBSD sys/ufs/ffs/ffs_inode.c.  Both UFS1 (32-bit pointers) and UFS2
 * (64-bit pointers) are handled via the ufs_read_indir() helper.
 */
u64 ufs_block_map(struct inode *inode, sector_t iblock)
{
	struct ufs_sb_info   *sbi = UFS_SB(inode->i_sb);
	struct ufs_inode_info *ui = UFS_I(inode);
	int   ufs2   = sbi->fs_ufs2;
	u64   frag   = sbi->fs_frag;
	u64   nindir = sbi->fs_nindir;
	u64   frag_addr;

	/* ---- Direct blocks (di_db[0..11]) ---- */
	if (iblock < UFS_NDADDR) {
		frag_addr = ufs2 ? le64_to_cpu(ui->i_u.i2.db[iblock])
				 : le32_to_cpu(ui->i_u.i1.db[iblock]);
		return frag_addr ? frag_addr / frag : 0;
	}
	iblock -= UFS_NDADDR;

	/* ---- Single-indirect (di_ib[0]) ---- */
	if (iblock < nindir) {
		frag_addr = ufs2 ? le64_to_cpu(ui->i_u.i2.ib[0])
				 : le32_to_cpu(ui->i_u.i1.ib[0]);
		if (!frag_addr)
			return 0;
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag, iblock, ufs2);
		return frag_addr ? frag_addr / frag : 0;
	}
	iblock -= nindir;

	/* ---- Double-indirect (di_ib[1]) ---- */
	if (iblock < nindir * nindir) {
		frag_addr = ufs2 ? le64_to_cpu(ui->i_u.i2.ib[1])
				 : le32_to_cpu(ui->i_u.i1.ib[1]);
		if (!frag_addr)
			return 0;
		/* Level 1: index into the double-indirect block */
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag,
					   iblock / nindir, ufs2);
		if (!frag_addr)
			return 0;
		/* Level 2: index into the single-indirect block */
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag,
					   iblock % nindir, ufs2);
		return frag_addr ? frag_addr / frag : 0;
	}
	iblock -= nindir * nindir;

	/* ---- Triple-indirect (di_ib[2]) ---- */
	{
		frag_addr = ufs2 ? le64_to_cpu(ui->i_u.i2.ib[2])
				 : le32_to_cpu(ui->i_u.i1.ib[2]);
		if (!frag_addr)
			return 0;
		/* Level 1 */
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag,
					   iblock / (nindir * nindir), ufs2);
		if (!frag_addr)
			return 0;
		/* Level 2 */
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag,
					   (iblock / nindir) % nindir, ufs2);
		if (!frag_addr)
			return 0;
		/* Level 3 */
		frag_addr = ufs_read_indir(inode->i_sb,
					   frag_addr / frag,
					   iblock % nindir, ufs2);
		return frag_addr ? frag_addr / frag : 0;
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

	/* Short symlinks stored inline in the inode's block-pointer area */
	if (inode->i_size <= maxlen) {
		/*
		 * UFS1: di_db[] is __le32[15]; cast to char *.
		 * UFS2: di_db[] is __le64[15]; cast to char *.
		 * The union members are laid out identically in memory;
		 * we just need the right pointer width for the type check.
		 */
		return sbi->fs_ufs2 ? (const char *)ui->i_u.i2.db
				    : (const char *)ui->i_u.i1.db;
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
		{
			struct ufs_sb_info *sbi = UFS_SB(inode->i_sb);
			dev_t rdev;

			/*
			 * Block/char device number is stored in di_db[0].
			 * UFS1: 32-bit pointer; UFS2: 64-bit pointer.
			 */
			if (sbi->fs_ufs2)
				rdev = (dev_t)le64_to_cpu(
						UFS_I(inode)->i_u.i2.db[0]);
			else
				rdev = (dev_t)le32_to_cpu(
						UFS_I(inode)->i_u.i1.db[0]);
			init_special_inode(inode, inode->i_mode, rdev);
		}
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
