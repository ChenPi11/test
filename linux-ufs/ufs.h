/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux UFS (Fast File System) driver
 * Inspired by OpenBSD sys/ufs/
 *
 * In-memory data structures, helper macros, and function prototypes.
 */
#ifndef _LINUX_UFS_H
#define _LINUX_UFS_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "ufs_fs.h"

/* ---- In-memory superblock info (sb->s_fs_info) ---- */
struct ufs_sb_info {
	int	fs_ufs2;	/* 1 = UFS2, 0 = UFS1 */

	/* Key superblock fields cached in native byte order */
	u32	fs_bsize;	/* block size (fs_bsize from superblock) */
	u32	fs_fsize;	/* fragment size */
	u32	fs_frag;	/* fragments per block */
	u32	fs_ncg;		/* number of cylinder groups */
	u32	fs_ipg;		/* inodes per cylinder group */
	u32	fs_fpg;		/* fragments per cylinder group */
	u32	fs_inopb;	/* inodes per block */
	u32	fs_iblkno;	/* inode-block offset within CG */
	u32	fs_cgoffset;	/* CG offset for UFS1 layout */
	u32	fs_cgmask;	/* CG mask for UFS1 layout */
	u32	fs_fsbtodb;	/* fragment -> 512-byte sector shift */
	u32	fs_nindir;	/* pointers per indirect block */

	/*
	 * VFS I/O block size used for all sb_bread() calls.
	 *
	 * Linux requires sb->s_blocksize <= PAGE_SIZE.  OpenBSD's default
	 * fs_bsize is 16 KiB (or 32 KiB on large-RAM systems), which can
	 * exceed PAGE_SIZE on many Linux configurations.  We therefore clamp:
	 *
	 *   fs_io_bsize = min(fs_bsize, PAGE_SIZE)
	 *
	 * When fs_io_bsize < fs_bsize, each FFS block is read as
	 * (fs_bsize / fs_io_bsize) consecutive VFS I/O blocks.
	 *
	 * Note: BSD VFS does not have an equivalent PAGE_SIZE constraint
	 * because its buffer cache tracks block size independently of the
	 * virtual-memory page size.  OpenBSD's newfs(8) defaults to
	 * fs_bsize = 16 KiB regardless of the host PAGE_SIZE.
	 */
	u32	fs_io_bsize;	/* VFS I/O block size = min(fs_bsize, PAGE_SIZE) */

	/* Filesystem statistics */
	u64	fs_total_frags;		/* total fragments */
	u64	fs_free_frags;		/* free fragments */
	u64	fs_total_inodes;	/* total inodes */
	u64	fs_free_inodes;		/* free inodes */

	u32	fs_maxsymlinklen;	/* max inline symlink length */

	char	fs_volname[32];		/* volume name (NUL-terminated) */
};

static inline struct ufs_sb_info *UFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* ---- In-memory inode info ---- */
struct ufs_inode_info {
	union {
		struct {
			__le32 db[UFS_NDADDR];	/* direct blocks (UFS1) */
			__le32 ib[UFS_NIADDR];	/* indirect blocks (UFS1) */
		} i1;
		struct {
			__le64 db[UFS_NDADDR];	/* direct blocks (UFS2) */
			__le64 ib[UFS_NIADDR];	/* indirect blocks (UFS2) */
		} i2;
	} i_u;
	u32	i_flags;		/* BSD file flags */
	/* FFS2 birth time (creation time); zero for FFS1 */
	s64	i_birthtime;		/* seconds since epoch */
	u32	i_birthtime_nsec;	/* nanosecond fraction */
	struct inode vfs_inode;		/* must be last */
};

static inline struct ufs_inode_info *UFS_I(struct inode *inode)
{
	return container_of(inode, struct ufs_inode_info, vfs_inode);
}

/*
 * ---- Address calculation macros ----
 *
 * These follow the same logic as the OpenBSD macros in sys/ufs/ffs/fs.h.
 * All addresses are in units of fragments (fs_fsize bytes).
 *
 * cgbase(cg)   = first fragment of cylinder group cg
 * cgstart(cg)  = cgbase + CG-layout offset (UFS1 only; UFS2 = cgbase)
 * cgimin(cg)   = first fragment of the inode table in cg
 * ino_fsba(no) = fragment address of the block holding inode no
 * ino_fsbo(no) = index of inode no within that block (0..fs_inopb-1)
 *
 * I/O block numbers for sb_bread() are in units of fs_io_bsize.
 * Convert a fragment address to an I/O block number with:
 *   io_blk_num = frag_addr / (fs_io_bsize / fs_fsize)
 *
 * When fs_io_bsize == fs_bsize (the common case where fs_bsize <= PAGE_SIZE):
 *   io_blk_num = frag_addr / fs_frag   (same as before)
 *
 * When fs_io_bsize == PAGE_SIZE < fs_bsize (large-block FFS):
 *   io_blk_num = frag_addr * fs_fsize / PAGE_SIZE
 *   Each FFS block spans (fs_bsize / fs_io_bsize) consecutive I/O blocks.
 */
static inline u64 ufs_cgbase(struct ufs_sb_info *sbi, u32 cg)
{
	return (u64)sbi->fs_fpg * cg;
}

static inline u64 ufs_cgstart(struct ufs_sb_info *sbi, u32 cg)
{
	u64 base = ufs_cgbase(sbi, cg);

	if (sbi->fs_ufs2)
		return base;
	return base + sbi->fs_cgoffset * (cg & ~sbi->fs_cgmask);
}

static inline u64 ufs_cgimin(struct ufs_sb_info *sbi, u32 cg)
{
	return ufs_cgstart(sbi, cg) + sbi->fs_iblkno;
}

/* Fragment address of the block containing inode ino */
static inline u64 ufs_ino_to_fsba(struct ufs_sb_info *sbi, u32 ino)
{
	u32 cg     = ino / sbi->fs_ipg;
	u32 ino_cg = ino % sbi->fs_ipg;	/* position within CG */
	u64 imin   = ufs_cgimin(sbi, cg);

	return imin + (u64)(ino_cg / sbi->fs_inopb) * sbi->fs_frag;
}

/* Index of inode ino within its block (0 .. fs_inopb-1) */
static inline u32 ufs_ino_to_fsbo(struct ufs_sb_info *sbi, u32 ino)
{
	return (ino % sbi->fs_ipg) % sbi->fs_inopb;
}

/*
 * Convert a UFS fragment address to a Linux VFS I/O block number.
 * sb->s_blocksize is set to fs_io_bsize = min(fs_bsize, PAGE_SIZE), so:
 *   io_block_num = frag_addr / (fs_io_bsize / fs_fsize)
 *                = frag_addr * fs_fsize / fs_io_bsize
 */
static inline sector_t ufs_frag_to_blk(struct ufs_sb_info *sbi,
					u64 frag_addr)
{
	return (sector_t)(frag_addr / (sbi->fs_io_bsize / sbi->fs_fsize));
}

/* ---- Function prototypes ---- */

/* super.c */
extern struct file_system_type ufs_fs_type;

/* inode.c */
struct inode *ufs_iget(struct super_block *sb, unsigned long ino);
int ufs_get_block(struct inode *inode, sector_t iblock,
		  struct buffer_head *bh_result, int create);
u64 ufs_block_map(struct inode *inode, sector_t iblock);

/* dir.c */
extern const struct inode_operations ufs_dir_inode_ops;
extern const struct file_operations  ufs_dir_operations;

/* file.c */
extern const struct inode_operations ufs_file_inode_ops;
extern const struct file_operations  ufs_file_operations;
extern const struct address_space_operations ufs_aops;
extern const struct inode_operations ufs_symlink_inode_ops;

#endif /* _LINUX_UFS_H */
