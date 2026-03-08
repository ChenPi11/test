/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux UFS (Fast File System) driver
 * Inspired by OpenBSD sys/ufs/
 *
 * On-disk data structures for UFS1/UFS2 (BSD FFS format).
 * Layout matches OpenBSD sys/ufs/ffs/fs.h and sys/ufs/ufs/dinode.h.
 *
 * UFS1 magic is 0x011954, UFS2 magic is 0x19540119.
 * Superblock is at byte offset 8192 (UFS1) or 65536 (UFS2).
 * The fs_magic field is always at offset 1372 from the superblock start.
 */
#ifndef _LINUX_UFS_FS_H
#define _LINUX_UFS_FS_H

#include <linux/types.h>

/* ---- Magic numbers ---- */
#define UFS_MAGIC	0x011954	/* UFS1 (FFS) magic number */
#define UFS2_MAGIC	0x19540119	/* UFS2 magic number */

/* ---- fs_flags bits (fs_flags field in superblock at offset 1308) ---- */
#define FS_UNCLEAN	0x0001		/* filesystem not cleanly unmounted */

/* ---- Superblock byte offsets on disk ---- */
#define UFS_SBLOCK_UFS1		8192
#define UFS_SBLOCK_UFS2		65536
#define UFS_SBLOCK_PIGGY	262144
#define UFS_SBLOCKSIZE		8192	/* superblock size in bytes */

/* ---- Root inode ---- */
#define UFS_ROOTINO		2	/* root inode number */

/* ---- Inode address constants ---- */
#define UFS_NDADDR	12	/* number of direct block pointers */
#define UFS_NIADDR	3	/* number of indirect block pointers */
#define UFS_NXADDR	2	/* extended attribute blocks (UFS2) */

/* ---- Inline symlink limits ---- */
#define UFS1_MAXSYMLINKLEN	((UFS_NDADDR + UFS_NIADDR) * sizeof(__le32))
#define UFS2_MAXSYMLINKLEN	((UFS_NDADDR + UFS_NIADDR) * sizeof(__le64))

/* ---- Directory constants ---- */
#define UFS_MAXNAMLEN	255	/* max filename length */
#define UFS_DIRBLKSIZ	512	/* directory block granularity */

/* ---- Directory entry file types ---- */
#define UFS_DT_UNKNOWN	0
#define UFS_DT_FIFO	1
#define UFS_DT_CHR	2
#define UFS_DT_DIR	4
#define UFS_DT_BLK	6
#define UFS_DT_REG	8
#define UFS_DT_LNK	10
#define UFS_DT_SOCK	12
#define UFS_DT_WHT	14

/* ---- Inode mode bits ---- */
#define UFS_IFMT	0170000
#define UFS_IFIFO	0010000
#define UFS_IFCHR	0020000
#define UFS_IFDIR	0040000
#define UFS_IFBLK	0060000
#define UFS_IFREG	0100000
#define UFS_IFLNK	0120000
#define UFS_IFSOCK	0140000

/*
 * On-disk UFS1 inode (128 bytes).
 * Matches struct ufs1_dinode from OpenBSD sys/ufs/ufs/dinode.h.
 */
struct ufs1_dinode {
	__le16	di_mode;		/*   0: file type + permissions */
	__le16	di_nlink;		/*   2: link count */
	__le16	di_ouid;		/*   4: old (16-bit) owner uid */
	__le16	di_ogid;		/*   6: old (16-bit) owner gid */
	__le64	di_size;		/*   8: file size in bytes */
	__le32	di_atime;		/*  16: last access time (sec) */
	__le32	di_atimensec;		/*  20: last access time (nsec) */
	__le32	di_mtime;		/*  24: last modify time (sec) */
	__le32	di_mtimensec;		/*  28: last modify time (nsec) */
	__le32	di_ctime;		/*  32: last change time (sec) */
	__le32	di_ctimensec;		/*  36: last change time (nsec) */
	__le32	di_db[UFS_NDADDR];	/*  40: direct block addresses */
	__le32	di_ib[UFS_NIADDR];	/*  88: indirect block addresses */
	__le32	di_flags;		/* 100: file flags (chflags) */
	__le32	di_blocks;		/* 104: 512-byte blocks held */
	__le32	di_gen;			/* 108: generation number */
	__le32	di_uid;			/* 112: owner uid (32-bit) */
	__le32	di_gid;			/* 116: owner gid (32-bit) */
	__le32	di_spare[2];		/* 120: reserved */
}; /* total: 128 bytes */

/*
 * On-disk UFS2 inode (256 bytes).
 * Matches struct ufs2_dinode from OpenBSD sys/ufs/ufs/dinode.h.
 */
struct ufs2_dinode {
	__le16	di_mode;		/*   0: file type + permissions */
	__le16	di_nlink;		/*   2: link count */
	__le32	di_uid;			/*   4: owner uid */
	__le32	di_gid;			/*   8: owner gid */
	__le32	di_blksize;		/*  12: inode block size */
	__le64	di_size;		/*  16: file size in bytes */
	__le64	di_blocks;		/*  24: bytes actually held */
	__le64	di_atime;		/*  32: last access time (sec) */
	__le64	di_mtime;		/*  40: last modify time (sec) */
	__le64	di_ctime;		/*  48: last change time (sec) */
	__le64	di_birthtime;		/*  56: creation time (sec) */
	__le32	di_mtimensec;		/*  64: last modify time (nsec) */
	__le32	di_atimensec;		/*  68: last access time (nsec) */
	__le32	di_ctimensec;		/*  72: last change time (nsec) */
	__le32	di_birthnsec;		/*  76: creation time (nsec) */
	__le32	di_gen;			/*  80: generation number */
	__le32	di_kernflags;		/*  84: kernel flags */
	__le32	di_flags;		/*  88: file flags (chflags) */
	__le32	di_extsize;		/*  92: extended attribute size */
	__le64	di_extb[UFS_NXADDR];	/*  96: extended attribute blocks */
	__le64	di_db[UFS_NDADDR];	/* 112: direct block addresses */
	__le64	di_ib[UFS_NIADDR];	/* 208: indirect block addresses */
	__le64	di_spare[3];		/* 232: reserved */
}; /* total: 256 bytes */

/*
 * On-disk UFS directory entry (variable length).
 * Matches struct direct from OpenBSD sys/ufs/ufs/dir.h.
 *
 * Records are padded to 4-byte boundaries.
 * d_reclen covers the entire record including padding.
 * d_ino == 0 means the entry is deleted/unused.
 */
struct ufs_direct {
	__le32	d_ino;			/* inode number */
	__le16	d_reclen;		/* length of this record */
	__u8	d_type;			/* file type (UFS_DT_*) */
	__u8	d_namlen;		/* name length (no NUL counted) */
	char	d_name[UFS_MAXNAMLEN + 1]; /* name, NUL-terminated */
};

/* Minimum record size (name length = 0): ino+reclen+type+namlen = 8 bytes */
#define UFS_DIR_REC_LEN(namlen) \
	(((offsetof(struct ufs_direct, d_name) + (namlen) + 1) + 3) & ~3)

/*
 * On-disk UFS superblock.
 *
 * The full structure is laid out to match BSD's struct fs exactly.
 * fs_magic is always at byte offset 1372 from the superblock start,
 * regardless of whether the filesystem was created on a 32- or 64-bit host.
 * (The 128-byte pointer section is sized so that its total is always 128
 *  bytes: NOCSPTRS = (128/sizeof(void*)) - 4.)
 *
 * Fields are little-endian (UFS on x86/amd64 systems).
 */
struct ufs_super_block {
	__le32	fs_firstfield;		/*    0 */
	__le32	fs_unused_1;		/*    4 */
	__le32	fs_sblkno;		/*    8: super-block in fs */
	__le32	fs_cblkno;		/*   12: cyl-block in fs */
	__le32	fs_iblkno;		/*   16: inode-blocks offset */
	__le32	fs_dblkno;		/*   20: first data block */
	__le32	fs_cgoffset;		/*   24: cyl group offset */
	__le32	fs_cgmask;		/*   28: cyl group mask */
	__le32	fs_ffs1_time;		/*   32: last write time (UFS1) */
	__le32	fs_ffs1_size;		/*   36: blocks in fs (UFS1) */
	__le32	fs_ffs1_dsize;		/*   40: data blocks (UFS1) */
	__le32	fs_ncg;			/*   44: number of cyl groups */
	__le32	fs_bsize;		/*   48: block size */
	__le32	fs_fsize;		/*   52: fragment size */
	__le32	fs_frag;		/*   56: fragments per block */
	__le32	fs_minfree;		/*   60: min free % */
	__le32	fs_rotdelay;		/*   64 */
	__le32	fs_rps;			/*   68 */
	__le32	fs_bmask;		/*   72 */
	__le32	fs_fmask;		/*   76 */
	__le32	fs_bshift;		/*   80 */
	__le32	fs_fshift;		/*   84 */
	__le32	fs_maxcontig;		/*   88 */
	__le32	fs_maxbpg;		/*   92 */
	__le32	fs_fragshift;		/*   96 */
	__le32	fs_fsbtodb;		/*  100: frag-to-sector shift */
	__le32	fs_sbsize;		/*  104 */
	__le32	fs_csmask;		/*  108 */
	__le32	fs_csshift;		/*  112 */
	__le32	fs_nindir;		/*  116: ptrs per indirect block */
	__le32	fs_inopb;		/*  120: inodes per block */
	__le32	fs_nspf;		/*  124 */
	__le32	fs_optim;		/*  128 */
	__le32	fs_npsect;		/*  132 */
	__le32	fs_interleave;		/*  136 */
	__le32	fs_trackskew;		/*  140 */
	__le32	fs_id[2];		/*  144 */
	__le32	fs_ffs1_csaddr;		/*  152 */
	__le32	fs_cssize;		/*  156 */
	__le32	fs_cgsize;		/*  160 */
	__le32	fs_ntrak;		/*  164 */
	__le32	fs_nsect;		/*  168 */
	__le32	fs_spc;			/*  172 */
	__le32	fs_ncyl;		/*  176 */
	__le32	fs_cpg;			/*  180 */
	__le32	fs_ipg;			/*  184: inodes per cyl group */
	__le32	fs_fpg;			/*  188: frags per cyl group */
	/* UFS1 cylinder summary: */
	__le32	fs_cs_ndir;		/*  192 */
	__le32	fs_cs_nbfree;		/*  196 */
	__le32	fs_cs_nifree;		/*  200 */
	__le32	fs_cs_nffree;		/*  204 */
	__u8	fs_fmod;		/*  208 */
	__u8	fs_clean;		/*  209 */
	__u8	fs_ronly;		/*  210 */
	__u8	fs_ffs1_flags;		/*  211 */
	__u8	fs_fsmnt[468];		/*  212: mounted-on path */
	__u8	fs_volname[32];		/*  680: volume name */
	__le64	fs_swuid;		/*  712 */
	__le32	fs_pad;			/*  720 */
	__le32	fs_cgrotor;		/*  724 */
	/*
	 * Pointer section: always 128 bytes regardless of host word size.
	 * (NOCSPTRS = (128/sizeof(void*)) - 4; total = 128 bytes.)
	 */
	__u8	fs_ptrs[128];		/*  728 */
	__le32	fs_cpc;			/*  856 */
	__le32	fs_maxbsize;		/*  860 */
	__le64	fs_spareconf64[17];	/*  864 */
	__le64	fs_sblockloc;		/* 1000 */
	/* UFS2 cylinder summary (struct csum_total): */
	__le64	fs_ufs2_cs_ndir;	/* 1008 */
	__le64	fs_ufs2_cs_nbfree;	/* 1016 */
	__le64	fs_ufs2_cs_nifree;	/* 1024 */
	__le64	fs_ufs2_cs_nffree;	/* 1032 */
	__le64	fs_ufs2_cs_spare[4];	/* 1040 */
	__le64	fs_time;		/* 1072: last write time (UFS2) */
	__le64	fs_size;		/* 1080: blocks in fs (UFS2) */
	__le64	fs_dsize;		/* 1088: data blocks (UFS2) */
	__le64	fs_csaddr;		/* 1096 */
	__le64	fs_pendingblocks;	/* 1104 */
	__le32	fs_pendinginodes;	/* 1112 */
	__le32	fs_snapinum[20];	/* 1116 */
	__le32	fs_avgfilesize;		/* 1196 */
	__le32	fs_avgfpdir;		/* 1200 */
	__le32	fs_sparecon[26];	/* 1204 */
	__le32	fs_flags;		/* 1308 */
	__le32	fs_fscktime;		/* 1312 */
	__le32	fs_contigsumsize;	/* 1316 */
	__le32	fs_maxsymlinklen;	/* 1320 */
	__le32	fs_inodefmt;		/* 1324 */
	__le64	fs_maxfilesize;		/* 1328 */
	__le64	fs_qbmask;		/* 1336 */
	__le64	fs_qfmask;		/* 1344 */
	__le32	fs_state;		/* 1352 */
	__le32	fs_postblformat;	/* 1356 */
	__le32	fs_nrpos;		/* 1360 */
	__le32	fs_postbloff;		/* 1364 */
	__le32	fs_rotbloff;		/* 1368 */
	__le32	fs_magic;		/* 1372: UFS_MAGIC or UFS2_MAGIC */
}; /* 1376 bytes */

#endif /* _LINUX_UFS_FS_H */
