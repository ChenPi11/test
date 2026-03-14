// SPDX-License-Identifier: GPL-2.0
/*
 * Linux UFS (Fast File System) driver – directory operations
 * Inspired by OpenBSD sys/ufs/ufs/ufs_lookup.c
 *
 * Implements readdir (iterate_shared) and lookup for UFS directories.
 *
 * UFS directories are files filled with variable-length struct ufs_direct
 * records.  Each record is padded to a 4-byte boundary and d_reclen covers
 * the full padded size.  A record with d_ino == 0 is empty/deleted.
 *
 * Directory blocks are read with sb_bread using the fragment address
 * returned by ufs_get_block (identical to regular file data).
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>

#include "ufs.h"
#include "ufs_fs.h"

/* ------------------------------------------------------------------ */
/* Directory helpers                                                     */
/* ------------------------------------------------------------------ */

/* Map a UFS directory file-type code to the Linux DT_* constant */
static unsigned char ufs_type_to_dtype[] = {
	[UFS_DT_UNKNOWN]	= DT_UNKNOWN,
	[UFS_DT_FIFO]		= DT_FIFO,
	[UFS_DT_CHR]		= DT_CHR,
	[UFS_DT_DIR]		= DT_DIR,
	[UFS_DT_BLK]		= DT_BLK,
	[UFS_DT_REG]		= DT_REG,
	[UFS_DT_LNK]		= DT_LNK,
	[UFS_DT_SOCK]		= DT_SOCK,
	[UFS_DT_WHT]		= DT_WHT,
};

static inline unsigned char ufs_dt_to_dtype(u8 dt)
{
	if (dt < ARRAY_SIZE(ufs_type_to_dtype))
		return ufs_type_to_dtype[dt];
	return DT_UNKNOWN;
}

/*
 * Read one fragment of the directory at logical (fragment) offset @frag.
 *
 * We call ufs_block_map() to translate the logical fragment number to the
 * physical fragment address, then use sb_bread() to read it from disk.
 */
static struct buffer_head *ufs_dir_bread(struct inode *dir, sector_t frag)
{
	u64 phys;
	struct buffer_head *bh;

	phys = ufs_block_map(dir, frag);
	if (!phys)
		return ERR_PTR(-EIO);

	bh = sb_bread(dir->i_sb, phys);
	if (!bh)
		return ERR_PTR(-EIO);
	return bh;
}

/* ------------------------------------------------------------------ */
/* readdir (iterate_shared)                                              */
/* ------------------------------------------------------------------ */

static int ufs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	loff_t pos = ctx->pos;
	loff_t dir_size = inode->i_size;
	/*
	 * Use sb->s_blocksize (= fs_io_bsize) as the I/O granularity.
	 * Directory entries are padded to 4-byte boundaries and are never
	 * larger than UFS_DIRBLKSIZ (512) bytes, so they always fit within
	 * a single I/O block (>= BLOCK_SIZE = 1024).
	 */
	unsigned bsize = inode->i_sb->s_blocksize;

	/* Skip the empty directory case */
	if (pos >= dir_size)
		return 0;

	while (pos < dir_size) {
		sector_t frag = pos / bsize;	/* logical block within dir */
		unsigned off  = pos % bsize;	/* byte offset within block */
		struct buffer_head *bh;
		const char *blk_end;
		const char *p;

		bh = ufs_dir_bread(inode, frag);
		if (IS_ERR(bh))
			return PTR_ERR(bh);

		blk_end = bh->b_data + bsize;
		p = bh->b_data + off;

		while (p < blk_end) {
			const struct ufs_direct *de =
				(const struct ufs_direct *)p;
			u16 reclen = le16_to_cpu(de->d_reclen);
			u32 ino    = le32_to_cpu(de->d_ino);

			/* Minimal sanity: reclen must be nonzero and aligned */
			if (reclen < 8 || reclen > blk_end - p ||
			    (reclen & 3)) {
				brelse(bh);
				return -EIO;
			}

			if (ino != 0) {
				unsigned char dtype =
					ufs_dt_to_dtype(de->d_type);
				u8 namlen = de->d_namlen;

				if (!dir_emit(ctx, de->d_name, namlen,
					      ino, dtype)) {
					brelse(bh);
					return 0;
				}
			}

			pos += reclen;
			ctx->pos = pos;
			p += reclen;
		}

		brelse(bh);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* lookup                                                                */
/* ------------------------------------------------------------------ */

/*
 * Look up @dentry->d_name inside directory @dir.
 *
 * Scans every directory block linearly (matches OpenBSD ufs_lookup logic).
 * On success returns a dentry associated with the found inode.
 */
static struct dentry *ufs_lookup(struct inode *dir, struct dentry *dentry,
				 unsigned int flags)
{
	unsigned bsize     = dir->i_sb->s_blocksize;
	loff_t   dir_size  = dir->i_size;
	const char  *name  = dentry->d_name.name;
	unsigned     nlen  = dentry->d_name.len;
	struct inode *inode = NULL;
	loff_t pos;

	if (nlen > UFS_MAXNAMLEN)
		return ERR_PTR(-ENAMETOOLONG);

	for (pos = 0; pos < dir_size; pos += bsize) {
		sector_t frag = pos / bsize;
		struct buffer_head *bh;
		const char *p, *blk_end;

		bh = ufs_dir_bread(dir, frag);
		if (IS_ERR(bh))
			return ERR_CAST(bh);

		blk_end = bh->b_data + bsize;
		p = bh->b_data;

		while (p < blk_end) {
			const struct ufs_direct *de =
				(const struct ufs_direct *)p;
			u16 reclen = le16_to_cpu(de->d_reclen);
			u32 ino    = le32_to_cpu(de->d_ino);

			if (reclen < 8 || (reclen & 3) ||
			    p + reclen > blk_end) {
				brelse(bh);
				return ERR_PTR(-EIO);
			}

			if (ino != 0 && de->d_namlen == nlen &&
			    memcmp(de->d_name, name, nlen) == 0) {
				/* Found it */
				brelse(bh);
				inode = ufs_iget(dir->i_sb, ino);
				if (IS_ERR(inode))
					return ERR_CAST(inode);
				goto done;
			}

			p += reclen;
		}

		brelse(bh);
	}

done:
	return d_splice_alias(inode, dentry);
}

/* ------------------------------------------------------------------ */
/* Operations tables                                                     */
/* ------------------------------------------------------------------ */

const struct inode_operations ufs_dir_inode_ops = {
	.lookup		= ufs_lookup,
};

const struct file_operations ufs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ufs_readdir,
};
