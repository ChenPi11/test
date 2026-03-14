// SPDX-License-Identifier: GPL-2.0
/*
 * Linux UFS (Fast File System) driver – superblock operations
 * Inspired by OpenBSD sys/ufs/ffs/ffs_vfsops.c
 *
 * Supports read-only mounting of UFS1 and UFS2 filesystems as created
 * by OpenBSD/FreeBSD/NetBSD newfs(8).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/vfs.h>

#include "ufs.h"
#include "ufs_fs.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Linux UFS Driver");
MODULE_DESCRIPTION("Read-only UFS1/UFS2 (BSD FFS) filesystem driver");
MODULE_ALIAS_FS("ufs2bsd");

/* Slab cache for ufs_inode_info */
static struct kmem_cache *ufs_inode_cachep;

/* ------------------------------------------------------------------ */
/* Inode allocation / destruction                                       */
/* ------------------------------------------------------------------ */

static struct inode *ufs_alloc_inode(struct super_block *sb)
{
	struct ufs_inode_info *ui;

	ui = alloc_inode_sb(sb, ufs_inode_cachep, GFP_KERNEL);
	if (!ui)
		return NULL;
	return &ui->vfs_inode;
}

static void ufs_free_inode_cb(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(ufs_inode_cachep, UFS_I(inode));
}

static void ufs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, ufs_free_inode_cb);
}

/* ------------------------------------------------------------------ */
/* statfs                                                               */
/* ------------------------------------------------------------------ */

static int ufs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb  = dentry->d_sb;
	struct ufs_sb_info *sbi = UFS_SB(sb);

	buf->f_type    = sb->s_magic;
	buf->f_bsize   = sbi->fs_bsize;
	/* Convert fragment counts to block counts (fs_frag frags per block) */
	buf->f_blocks  = sbi->fs_total_frags / sbi->fs_frag;
	buf->f_bfree   = sbi->fs_free_frags  / sbi->fs_frag;
	buf->f_bavail  = buf->f_bfree;
	buf->f_files   = sbi->fs_total_inodes;
	buf->f_ffree   = sbi->fs_free_inodes;
	buf->f_namelen = UFS_MAXNAMLEN;

	return 0;
}

/* ------------------------------------------------------------------ */
/* show_options                                                          */
/* ------------------------------------------------------------------ */

static int ufs_show_options(struct seq_file *m, struct dentry *root)
{
	struct ufs_sb_info *sbi = UFS_SB(root->d_sb);

	seq_printf(m, ",version=%s", sbi->fs_ufs2 ? "ufs2" : "ufs1");
	if (sbi->fs_volname[0])
		seq_printf(m, ",volname=%s", sbi->fs_volname);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Super-block operations                                               */
/* ------------------------------------------------------------------ */

static const struct super_operations ufs_sops = {
	.alloc_inode	= ufs_alloc_inode,
	.destroy_inode	= ufs_destroy_inode,
	.statfs		= ufs_statfs,
	.show_options	= ufs_show_options,
};

/* ------------------------------------------------------------------ */
/* Superblock parsing                                                    */
/* ------------------------------------------------------------------ */

/*
 * Read exactly `len' bytes from byte offset `byte_off' on the block device
 * into `buf'.  Uses sb_bread() so it depends on sb->s_blocksize being set
 * (call sb_min_blocksize before this function).
 */
static int ufs_read_raw(struct super_block *sb, loff_t byte_off,
			void *buf, size_t len)
{
	unsigned blksz = sb->s_blocksize;
	size_t done = 0;

	while (done < len) {
		sector_t blkno = (byte_off + done) / blksz;
		size_t   off   = (byte_off + done) % blksz;
		size_t   copy  = min_t(size_t, blksz - off, len - done);
		struct buffer_head *bh;

		bh = sb_bread(sb, blkno);
		if (!bh) {
			pr_err("ufs: sb_bread failed at block %llu\n",
			       (unsigned long long)blkno);
			return -EIO;
		}
		memcpy((u8 *)buf + done, bh->b_data + off, copy);
		brelse(bh);
		done += copy;
	}
	return 0;
}

/*
 * Try to locate and parse a UFS superblock.
 * Checks UFS2 (offset 65536) then UFS1 (offset 8192).
 * Returns 0 on success with *sbi filled, -EINVAL if not a UFS fs.
 */
static int ufs_parse_superblock(struct super_block *sb,
				struct ufs_sb_info *sbi)
{
	/* Superblock is 1376 bytes; read a few extra for safety */
	static const loff_t probe_offsets[] = {
		UFS_SBLOCK_UFS2, UFS_SBLOCK_UFS1, UFS_SBLOCK_PIGGY, 0
	};
	struct ufs_super_block *fsb;
	u32 magic;
	int i, ret;

	fsb = kmalloc(sizeof(*fsb), GFP_KERNEL);
	if (!fsb)
		return -ENOMEM;

	for (i = 0; probe_offsets[i]; i++) {
		ret = ufs_read_raw(sb, probe_offsets[i], fsb, sizeof(*fsb));
		if (ret)
			continue;

		magic = le32_to_cpu(fsb->fs_magic);
		if (magic == UFS_MAGIC || magic == UFS2_MAGIC)
			goto found;
	}

	kfree(fsb);
	return -EINVAL;

found:
	sbi->fs_ufs2  = (magic == UFS2_MAGIC) ? 1 : 0;
	sbi->fs_bsize = le32_to_cpu(fsb->fs_bsize);
	sbi->fs_fsize = le32_to_cpu(fsb->fs_fsize);
	sbi->fs_frag  = le32_to_cpu(fsb->fs_frag);
	sbi->fs_ncg   = le32_to_cpu(fsb->fs_ncg);
	sbi->fs_ipg   = le32_to_cpu(fsb->fs_ipg);
	sbi->fs_fpg   = le32_to_cpu(fsb->fs_fpg);
	sbi->fs_inopb = le32_to_cpu(fsb->fs_inopb);
	sbi->fs_iblkno  = le32_to_cpu(fsb->fs_iblkno);
	sbi->fs_cgoffset = le32_to_cpu(fsb->fs_cgoffset);
	sbi->fs_cgmask   = le32_to_cpu(fsb->fs_cgmask);
	sbi->fs_fsbtodb  = le32_to_cpu(fsb->fs_fsbtodb);
	sbi->fs_nindir   = le32_to_cpu(fsb->fs_nindir);
	sbi->fs_maxsymlinklen = le32_to_cpu(fsb->fs_maxsymlinklen);

	/* Populate statistics */
	if (sbi->fs_ufs2) {
		sbi->fs_total_frags  = le64_to_cpu(fsb->fs_size);
		sbi->fs_free_frags   = le64_to_cpu(fsb->fs_ufs2_cs_nbfree)
					* sbi->fs_frag
					+ le64_to_cpu(fsb->fs_ufs2_cs_nffree);
		sbi->fs_total_inodes = (u64)sbi->fs_ncg * sbi->fs_ipg;
		sbi->fs_free_inodes  = le64_to_cpu(fsb->fs_ufs2_cs_nifree);
	} else {
		sbi->fs_total_frags  = le32_to_cpu(fsb->fs_ffs1_size);
		sbi->fs_free_frags   = (u64)le32_to_cpu(fsb->fs_cs_nbfree)
					* sbi->fs_frag
					+ le32_to_cpu(fsb->fs_cs_nffree);
		sbi->fs_total_inodes = (u64)sbi->fs_ncg * sbi->fs_ipg;
		sbi->fs_free_inodes  = le32_to_cpu(fsb->fs_cs_nifree);
	}

	/* Volume name (may be empty) */
	memcpy(sbi->fs_volname, fsb->fs_volname, sizeof(sbi->fs_volname) - 1);
	sbi->fs_volname[sizeof(sbi->fs_volname) - 1] = '\0';

	/*
	 * Verify filesystem clean status.
	 *
	 * FFS1 uses a superblock checksum (fs_state) to guard the validity
	 * of fs_clean.  At clean-unmount time, newfs/fsck write:
	 *   fs_state = FS_OKAY - fs_ffs1_time   (unsigned 32-bit arithmetic)
	 * At mount time we verify:
	 *   (fs_state + fs_ffs1_time) == FS_OKAY
	 * If the equation holds, fs_clean is trustworthy; otherwise the
	 * superblock may be partially written or from a foreign tool that
	 * did not maintain this invariant.
	 *
	 * FFS2 does not use fs_state as a checksum; trust fs_clean and
	 * FS_UNCLEAN directly.
	 *
	 * Since this is a read-only driver we always mount the filesystem
	 * regardless of clean status, but warn the operator to run
	 * fsck_ffs(8) on any questionable image.  This mirrors the warning
	 * emitted by OpenBSD ffs_mountfs() when MNT_FORCE is used.
	 */
	if (!sbi->fs_ufs2) {
		/* FFS1: validate superblock checksum before trusting fs_clean */
		u32 ffs1_time  = le32_to_cpu(fsb->fs_ffs1_time);
		u32 ffs1_state = le32_to_cpu(fsb->fs_state);

		if ((ffs1_state + ffs1_time) != FS_OKAY) {
			pr_warn("ufs: %s: FFS1 superblock checksum invalid "
				"(fs_state=0x%08x + fs_time=0x%08x = 0x%08x, "
				"expected FS_OKAY=0x%08x); "
				"filesystem clean-state unverifiable\n",
				sb->s_id, ffs1_state, ffs1_time,
				ffs1_state + ffs1_time, (u32)FS_OKAY);
		} else if (fsb->fs_clean == 0 ||
			   (le32_to_cpu(fsb->fs_flags) & FS_UNCLEAN)) {
			pr_warn("ufs: %s: filesystem not cleanly unmounted "
				"(fs_clean=0x%02x); "
				"data may be inconsistent (run fsck_ffs)\n",
				sb->s_id, fsb->fs_clean);
		}
	} else {
		/*
		 * FFS2: fs_state is not used as a checksum guard.
		 * Trust fs_clean and the FS_UNCLEAN flag directly.
		 */
		if (fsb->fs_clean == 0 ||
		    (le32_to_cpu(fsb->fs_flags) & FS_UNCLEAN)) {
			pr_warn("ufs: %s: filesystem not cleanly unmounted; "
				"data may be inconsistent (run fsck_ffs)\n",
				sb->s_id);
		}
	}

	kfree(fsb);
	return 0;
}

/* ------------------------------------------------------------------ */
/* fill_super                                                            */
/* ------------------------------------------------------------------ */

static int ufs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ufs_sb_info *sbi;
	struct inode *root;
	int ret;

	pr_info("ufs: fill_super called, s_bdev=%p\n", sb->s_bdev);

	/*
	 * Set a safe initial block size (>= BLOCK_SIZE = 1024) before any I/O.
	 * UFS minimum block size (MINBSIZE) is 4096, so 4096 is always safe.
	 */
	if (!sb_min_blocksize(sb, 4096)) {
		pr_err("ufs: cannot set minimum block size\n");
		return -EINVAL;
	}
	pr_info("ufs: s_blocksize=%lu after sb_min_blocksize\n", sb->s_blocksize);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	/* Parse the on-disk superblock */
	ret = ufs_parse_superblock(sb, sbi);
	if (ret) {
		if (!(fc->sb_flags & SB_SILENT))
			pr_err("ufs: not a UFS1/UFS2 filesystem\n");
		goto err_sbi;
	}

	/* Basic sanity checks */
	if (!is_power_of_2(sbi->fs_bsize) ||
	    !is_power_of_2(sbi->fs_fsize) ||
	    sbi->fs_bsize < sbi->fs_fsize ||
	    sbi->fs_bsize < 512 ||
	    sbi->fs_frag  == 0  ||
	    sbi->fs_inopb == 0  ||
	    sbi->fs_ncg   == 0  ||
	    sbi->fs_ipg   == 0  ||
	    sbi->fs_fpg   == 0) {
		pr_err("ufs: corrupted superblock\n");
		ret = -EINVAL;
		goto err_sbi;
	}

	/*
	 * Set the VFS block size to fs_io_bsize = min(fs_bsize, PAGE_SIZE).
	 *
	 * Linux requires sb->s_blocksize <= PAGE_SIZE.  OpenBSD's default
	 * fs_bsize is 16 KiB (newfs(8) default) or 32 KiB (on large-RAM
	 * systems), both of which can exceed PAGE_SIZE on kernels built with
	 * 4 KiB pages.  By using min(fs_bsize, PAGE_SIZE) we support all
	 * valid FFS images regardless of their block size.
	 *
	 * Constraint: fs_fsize must be <= PAGE_SIZE (otherwise individual
	 * fragments don't fit in a page, which is unsupported).
	 *
	 * When fs_io_bsize < fs_bsize, each FFS block is read as
	 * (fs_bsize / fs_io_bsize) consecutive VFS I/O blocks; the block
	 * mapping arithmetic in inode.c accounts for this.
	 */
	if (sbi->fs_fsize > PAGE_SIZE) {
		pr_err("ufs: fragment size %u exceeds PAGE_SIZE %lu; not supported\n", sbi->fs_fsize, PAGE_SIZE);
		ret = -EINVAL;
		goto err_sbi;
	}
	sbi->fs_io_bsize = min_t(u32, sbi->fs_bsize, (u32)PAGE_SIZE);

	if (!sb_set_blocksize(sb, sbi->fs_io_bsize)) {
		pr_err("ufs: cannot set I/O block size %u (fs_bsize=%u, PAGE_SIZE=%lu)\n",
		       sbi->fs_io_bsize, sbi->fs_bsize, PAGE_SIZE);
		ret = -EINVAL;
		goto err_sbi;
	}

	sb->s_magic   = sbi->fs_ufs2 ? UFS2_MAGIC : UFS_MAGIC;
	sb->s_op      = &ufs_sops;
	sb->s_flags  |= SB_RDONLY;	/* read-only */
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/* Read the root inode (always inode 2) */
	root = ufs_iget(sb, UFS_ROOTINO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		pr_err("ufs: cannot read root inode\n");
		goto err_sbi;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto err_sbi;
	}

	pr_info("ufs: mounted %s (UFS%s, fs_bsize=%u, fs_fsize=%u, "
		"io_bsize=%u, ncg=%u)\n",
		sbi->fs_volname[0] ? sbi->fs_volname : "(unnamed)",
		sbi->fs_ufs2 ? "2" : "1",
		sbi->fs_bsize, sbi->fs_fsize,
		sbi->fs_io_bsize, sbi->fs_ncg);

	return 0;

err_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* ------------------------------------------------------------------ */
/* VFS registration                                                      */
/* ------------------------------------------------------------------ */

static int ufs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, ufs_fill_super);
}

static const struct fs_context_operations ufs_context_ops = {
	.get_tree	= ufs_get_tree,
};

static int ufs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &ufs_context_ops;
	return 0;
}

static void ufs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

struct file_system_type ufs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "ufs2bsd",
	.init_fs_context	= ufs_init_fs_context,
	.kill_sb		= ufs_kill_sb,
	.fs_flags		= FS_REQUIRES_DEV,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                    */
/* ------------------------------------------------------------------ */

static void ufs_inode_init_once(void *obj)
{
	struct ufs_inode_info *ui = obj;

	inode_init_once(&ui->vfs_inode);
}

static int __init ufs_init(void)
{
	int ret;

	ufs_inode_cachep = kmem_cache_create_usercopy("ufs_inode_cache",
				sizeof(struct ufs_inode_info), 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				offsetof(struct ufs_inode_info, i_u),
				sizeof_field(struct ufs_inode_info, i_u),
				ufs_inode_init_once);
	if (!ufs_inode_cachep)
		return -ENOMEM;

	ret = register_filesystem(&ufs_fs_type);
	if (ret) {
		kmem_cache_destroy(ufs_inode_cachep);
		return ret;
	}

	pr_info("ufs: UFS1/UFS2 (BSD FFS) read-only driver loaded\n");
	return 0;
}

static void __exit ufs_exit(void)
{
	unregister_filesystem(&ufs_fs_type);
	/*
	 * Ensure all RCU-deferred frees have completed before destroying
	 * the slab cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(ufs_inode_cachep);
	pr_info("ufs: driver unloaded\n");
}

module_init(ufs_init);
module_exit(ufs_exit);
