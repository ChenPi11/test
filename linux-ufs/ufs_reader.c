/*
 * ufs_reader.c – Userspace UFS1/UFS2 filesystem reader for testing.
 *
 * Reads a UFS (BSD FFS) disk image and dumps its structure:
 *   - Superblock fields
 *   - Root directory listing
 *   - File contents
 *
 * This validates that the on-disk layout created by create_ufs1.py matches
 * what the kernel driver (linux_ufs.ko) expects to find.
 *
 * Build:  gcc -O2 -o ufs_reader ufs_reader.c
 * Usage:  ./ufs_reader <image-file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>

/* ---- On-disk constants ---- */
#define UFS_MAGIC        0x011954u
#define UFS2_MAGIC       0x19540119u
#define UFS_ROOTINO      2
#define UFS_NDADDR       12
#define UFS_NIADDR       3
#define UFS_NXADDR       2

#define SBLOCK_UFS1      8192
#define SBLOCK_UFS2      65536

/* ---- On-disk UFS1 inode (128 bytes) ---- */
typedef struct {
	uint16_t di_mode;
	int16_t  di_nlink;
	uint16_t di_ouid;
	uint16_t di_ogid;
	uint64_t di_size;
	uint32_t di_atime;
	uint32_t di_atimensec;
	uint32_t di_mtime;
	uint32_t di_mtimensec;
	uint32_t di_ctime;
	uint32_t di_ctimensec;
	uint32_t di_db[UFS_NDADDR];
	uint32_t di_ib[UFS_NIADDR];
	uint32_t di_flags;
	uint32_t di_blocks;
	uint32_t di_gen;
	uint32_t di_uid;
	uint32_t di_gid;
	uint32_t di_spare[2];
} ufs1_dinode_t;   /* 128 bytes */

/* ---- On-disk UFS2 inode (256 bytes) ---- */
typedef struct {
	uint16_t di_mode;
	int16_t  di_nlink;
	uint32_t di_uid;
	uint32_t di_gid;
	uint32_t di_blksize;
	uint64_t di_size;
	uint64_t di_blocks;
	int64_t  di_atime;
	int64_t  di_mtime;
	int64_t  di_ctime;
	int64_t  di_birthtime;
	uint32_t di_mtimensec;
	uint32_t di_atimensec;
	uint32_t di_ctimensec;
	uint32_t di_birthnsec;
	uint32_t di_gen;
	uint32_t di_kernflags;
	uint32_t di_flags;
	uint32_t di_extsize;
	int64_t  di_extb[UFS_NXADDR];
	int64_t  di_db[UFS_NDADDR];
	int64_t  di_ib[UFS_NIADDR];
	int64_t  di_spare[3];
} ufs2_dinode_t;   /* 256 bytes */

/* ---- Directory entry ---- */
typedef struct {
	uint32_t d_ino;
	uint16_t d_reclen;
	uint8_t  d_type;
	uint8_t  d_namlen;
	char     d_name[256];
} ufs_direct_t;

/* ---- Superblock key fields ---- */
typedef struct {
	int      ufs2;
	uint32_t bsize;
	uint32_t fsize;
	uint32_t frag;
	uint32_t ncg;
	uint32_t ipg;
	uint32_t fpg;
	uint32_t inopb;
	uint32_t iblkno;
	uint32_t cgoffset;
	uint32_t cgmask;
	uint32_t fsbtodb;
	uint32_t nindir;
	uint32_t magic;
	char     volname[33];
} ufs_sb_t;

/* ---- Helpers ---- */
static int fd_img;

static int read_at(off_t off, void *buf, size_t n)
{
	ssize_t r = pread(fd_img, buf, n, off);
	if (r < 0) { perror("pread"); return -1; }
	if ((size_t)r < n) { fprintf(stderr, "short read at %lld\n", (long long)off); return -1; }
	return 0;
}

/* Convert fragment address to byte offset */
static off_t frag_to_off(ufs_sb_t *sb, uint64_t frag)
{
	return (off_t)(frag * sb->fsize);
}

/* Power-of-2 check (same logic as super.c IS_POWER_OF_2) */
#define IS_POW2(n)  ((n) != 0 && (((n) & ((n)-1)) == 0))
/* FFS1 superblock checksum constant (OpenBSD FS_OKAY) */
#define FS_OKAY     0x7c269d38u
/* fs_flags bit: not cleanly unmounted (matches ufs_fs.h FS_UNCLEAN) */
#define FS_UNCLEAN  0x0001u

/*
 * Read the superblock.
 * Returns  0  on success (sb is filled; warnings may have been printed).
 *         -1  if no UFS1/UFS2 magic was found.
 *         -2  if magic was found but the superblock fails sanity checks
 *             (error message already printed to stderr).
 */
static int read_superblock(ufs_sb_t *sb)
{
	static const off_t probe[] = { SBLOCK_UFS2, SBLOCK_UFS1, 0 };
	uint8_t raw[1376];
	int i;

	for (i = 0; probe[i]; i++) {
		uint32_t magic;
		if (read_at(probe[i], raw, sizeof(raw)) != 0)
			continue;
		/* magic is at offset 1372 */
		memcpy(&magic, raw + 1372, 4);
		if (magic == UFS_MAGIC || magic == UFS2_MAGIC) {
			sb->ufs2 = (magic == UFS2_MAGIC);
			memcpy(&sb->bsize,    raw + 48,  4);
			memcpy(&sb->fsize,    raw + 52,  4);
			memcpy(&sb->frag,     raw + 56,  4);
			memcpy(&sb->ncg,      raw + 44,  4);
			memcpy(&sb->ipg,      raw + 184, 4);
			memcpy(&sb->fpg,      raw + 188, 4);
			memcpy(&sb->inopb,    raw + 120, 4);
			memcpy(&sb->iblkno,   raw + 16,  4);
			memcpy(&sb->cgoffset, raw + 24,  4);
			memcpy(&sb->cgmask,   raw + 28,  4);
			memcpy(&sb->fsbtodb,  raw + 100, 4);
			memcpy(&sb->nindir,   raw + 116, 4);
			sb->magic = magic;
			memcpy(sb->volname, raw + 680, 32);
			sb->volname[32] = '\0';
			printf("Found superblock at offset %lld\n",
			       (long long)probe[i]);

			/*
			 * Sanity checks – mirror super.c ufs_fill_super().
			 * A magic number alone is not sufficient; a partially-
			 * written or foreign-tool superblock may have garbage
			 * geometry that would cause undefined behaviour.
			 */
			if (!IS_POW2(sb->bsize)   || !IS_POW2(sb->fsize)  ||
			    sb->bsize < sb->fsize  || sb->bsize < 512       ||
			    sb->frag  == 0 || sb->ncg   == 0 ||
			    sb->ipg   == 0 || sb->fpg   == 0 ||
			    sb->inopb == 0) {
				fprintf(stderr,
				    "ERROR: superblock sanity check failed "
				    "(bsize=%u fsize=%u frag=%u ncg=%u "
				    "ipg=%u fpg=%u inopb=%u)\n",
				    sb->bsize, sb->fsize, sb->frag,
				    sb->ncg, sb->ipg, sb->fpg, sb->inopb);
				return -2;
			}

			/*
			 * The Linux kernel driver requires fs_fsize <= PAGE_SIZE.
			 * When fs_fsize > PAGE_SIZE, individual fragments cannot
			 * be mapped into a single page, which is unsupported.
			 * (This mirrors the check in super.c ufs_fill_super().)
			 *
			 * fs_bsize > PAGE_SIZE is fine: the driver uses
			 * fs_io_bsize = min(fs_bsize, PAGE_SIZE) and handles the
			 * sub-blocking arithmetic in ufs_block_map() / ufs_iget().
			 */
			if (sb->fsize > (uint32_t)getpagesize()) {
				fprintf(stderr,
				    "ERROR: fragment size %u exceeds page size "
				    "(%d); not supported by the kernel driver\n",
				    sb->fsize, getpagesize());
				return -2;
			}

			/*
			 * FFS1 superblock checksum (warning only).
			 * Mirrors the check in super.c ufs_parse_superblock().
			 * At clean-unmount time: fs_state = FS_OKAY - fs_ffs1_time.
			 * We verify: (fs_state + fs_ffs1_time) == FS_OKAY.
			 */
			if (!sb->ufs2) {
				uint32_t ffs1_time, ffs1_state;
				memcpy(&ffs1_time,  raw + 32,   4);
				memcpy(&ffs1_state, raw + 1352, 4);
				if (ffs1_state + ffs1_time != FS_OKAY)
					printf("WARN: FFS1 superblock checksum "
					       "invalid (fs_state=0x%08x + "
					       "fs_time=0x%08x = 0x%08x, "
					       "expected 0x%08x)\n",
					       ffs1_state, ffs1_time,
					       ffs1_state + ffs1_time,
					       FS_OKAY);
			}

			/*
			 * Clean-status check (warning only).
			 * Mirrors the check in super.c ufs_parse_superblock().
			 */
			{
				uint8_t  fs_clean = raw[209];
				uint32_t fs_flags;
				memcpy(&fs_flags, raw + 1308, 4);
				if (fs_clean == 0 || (fs_flags & FS_UNCLEAN))
					printf("WARN: filesystem not cleanly "
					       "unmounted (fs_clean=%u "
					       "flags=0x%08x); run fsck_ffs\n",
					       fs_clean, fs_flags);
			}

			return 0;
		}
	}
	return -1;
}

/* Compute fragment address of inode table block for inode @ino */
static uint64_t ino_to_fsba(ufs_sb_t *sb, uint32_t ino)
{
	uint32_t cg     = ino / sb->ipg;
	uint32_t ino_cg = ino % sb->ipg;
	uint64_t cgbase = (uint64_t)sb->fpg * cg;
	uint64_t cgstart;
	uint64_t cgimin;

	if (sb->ufs2)
		cgstart = cgbase;
	else
		cgstart = cgbase + sb->cgoffset * (cg & ~sb->cgmask);

	cgimin = cgstart + sb->iblkno;
	return cgimin + (ino_cg / sb->inopb) * sb->frag;
}

/* Read inode @ino into @din (UFS1) */
static int read_inode1(ufs_sb_t *sb, uint32_t ino, ufs1_dinode_t *din)
{
	uint64_t fsba    = ino_to_fsba(sb, ino);
	uint32_t fsbo    = (ino % sb->ipg) % sb->inopb;
	/* block number (in units of fs_bsize) and offset within block */
	uint64_t blk_num = fsba / sb->frag;
	off_t    blk_off = (off_t)blk_num * sb->bsize;
	off_t    ioff    = blk_off + (off_t)fsbo * sizeof(*din);

	return read_at(ioff, din, sizeof(*din));
}

/* Map logical block @iblk (UFS1) to fragment address */
static uint64_t ufs1_block_map(ufs_sb_t *sb, ufs1_dinode_t *din, uint32_t iblk)
{
	if (iblk < UFS_NDADDR)
		return din->di_db[iblk];
	/* single indirect */
	{
		uint32_t ind_off = iblk - UFS_NDADDR;
		uint64_t ind_frag = din->di_ib[0];
		uint32_t *ptrs;
		uint8_t  *buf;
		uint64_t  result;
		off_t     ind_off_bytes;

		if (!ind_frag || ind_off >= sb->nindir) return 0;
		buf = malloc(sb->bsize);
		if (!buf) return 0;
		ind_off_bytes = (off_t)(ind_frag / sb->frag) * sb->bsize;
		if (read_at(ind_off_bytes, buf, sb->bsize) != 0) { free(buf); return 0; }
		ptrs = (uint32_t *)buf;
		result = ptrs[ind_off];
		free(buf);
		return result;
	}
}

/* Read bytes of a UFS1 regular file */
static int read_file1(ufs_sb_t *sb, ufs1_dinode_t *din, uint8_t *out, uint64_t size)
{
	uint64_t done = 0;
	uint32_t iblk = 0;

	while (done < size) {
		uint64_t frag_addr = ufs1_block_map(sb, din, iblk);
		uint64_t copy = size - done;
		if (copy > sb->bsize) copy = sb->bsize;
		if (!frag_addr) { memset(out + done, 0, copy); }
		else {
			off_t off = (off_t)(frag_addr / sb->frag) * sb->bsize;
			if (read_at(off, out + done, copy) != 0) return -1;
		}
		done += copy;
		iblk++;
	}
	return 0;
}

/* List directory entries from a UFS1 dir inode */
static void list_dir1(ufs_sb_t *sb, ufs1_dinode_t *din)
{
	uint64_t dir_size = din->di_size;
	uint64_t pos = 0;
	uint32_t iblk = 0;
	uint8_t *blk_buf = malloc(sb->bsize);
	if (!blk_buf) return;

	while (pos < dir_size) {
		uint64_t frag_addr = ufs1_block_map(sb, din, iblk);
		off_t blk_off;
		uint8_t *p, *end;

		if (!frag_addr) { pos += sb->bsize; iblk++; continue; }
		blk_off = (off_t)(frag_addr / sb->frag) * sb->bsize;
		if (read_at(blk_off, blk_buf, sb->bsize) != 0) break;

		p   = blk_buf;
		end = blk_buf + sb->bsize;
		while (p < end) {
			ufs_direct_t *de = (ufs_direct_t *)p;
			uint32_t ino    = de->d_ino;
			uint16_t reclen = de->d_reclen;

			if (reclen < 8 || reclen > (uint16_t)(end - p)) break;
			if (ino != 0) {
				char name[256];
				memcpy(name, de->d_name, de->d_namlen);
				name[de->d_namlen] = '\0';
				printf("  ino=%-4u type=%u name=%s\n",
				       ino, de->d_type, name);
			}
			p += reclen;
			pos += reclen;
		}
		iblk++;
		if (pos % sb->bsize) pos += sb->bsize - (pos % sb->bsize);
	}
	free(blk_buf);
}

/* ---- UFS2 helpers ---- */

/* Read inode @ino into @din (UFS2) */
static int read_inode2(ufs_sb_t *sb, uint32_t ino, ufs2_dinode_t *din)
{
	uint64_t fsba    = ino_to_fsba(sb, ino);
	uint32_t fsbo    = (ino % sb->ipg) % sb->inopb;
	uint64_t blk_num = fsba / sb->frag;
	off_t    blk_off = (off_t)blk_num * sb->bsize;
	off_t    ioff    = blk_off + (off_t)fsbo * sizeof(*din);

	return read_at(ioff, din, sizeof(*din));
}

/* Read one indirect block pointer (UFS2 uses 64-bit pointers) */
static uint64_t read_indir2(ufs_sb_t *sb, uint64_t blk_num, uint64_t idx)
{
	uint8_t *buf = malloc(sb->bsize);
	uint64_t result = 0;
	if (!buf) return 0;
	if (read_at((off_t)blk_num * sb->bsize, buf, sb->bsize) == 0) {
		int64_t raw;
		memcpy(&raw, buf + idx * 8, 8);
		result = (uint64_t)raw;
	}
	free(buf);
	return result;
}

/* Map logical block @iblk (UFS2) to fragment address (0 = hole) */
static uint64_t ufs2_block_map(ufs_sb_t *sb, ufs2_dinode_t *din, uint64_t iblk)
{
	uint64_t nindir = sb->nindir;
	uint64_t fa;

	if (iblk < UFS_NDADDR)
		return (uint64_t)din->di_db[iblk];
	iblk -= UFS_NDADDR;

	/* Single indirect */
	if (iblk < nindir) {
		fa = (uint64_t)din->di_ib[0];
		if (!fa) return 0;
		return read_indir2(sb, fa / sb->frag, iblk);
	}
	iblk -= nindir;

	/* Double indirect */
	if (iblk < nindir * nindir) {
		fa = (uint64_t)din->di_ib[1];
		if (!fa) return 0;
		fa = read_indir2(sb, fa / sb->frag, iblk / nindir);
		if (!fa) return 0;
		return read_indir2(sb, fa / sb->frag, iblk % nindir);
	}
	iblk -= nindir * nindir;

	/* Triple indirect */
	fa = (uint64_t)din->di_ib[2];
	if (!fa) return 0;
	fa = read_indir2(sb, fa / sb->frag, iblk / (nindir * nindir));
	if (!fa) return 0;
	fa = read_indir2(sb, fa / sb->frag, (iblk / nindir) % nindir);
	if (!fa) return 0;
	return read_indir2(sb, fa / sb->frag, iblk % nindir);
}

/* Read bytes of a UFS2 regular file */
static int read_file2(ufs_sb_t *sb, ufs2_dinode_t *din, uint8_t *out,
		      uint64_t size)
{
	uint64_t done = 0, iblk = 0;

	while (done < size) {
		uint64_t frag_addr = ufs2_block_map(sb, din, iblk);
		uint64_t copy = size - done;
		if (copy > sb->bsize) copy = sb->bsize;
		if (!frag_addr) {
			memset(out + done, 0, copy);
		} else {
			off_t off = (off_t)(frag_addr / sb->frag) * sb->bsize;
			if (read_at(off, out + done, copy) != 0) return -1;
		}
		done += copy;
		iblk++;
	}
	return 0;
}

/* List directory entries from a UFS2 dir inode */
static void list_dir2(ufs_sb_t *sb, ufs2_dinode_t *din)
{
	uint64_t dir_size = din->di_size;
	uint64_t pos = 0, iblk = 0;
	uint8_t *blk_buf = malloc(sb->bsize);
	if (!blk_buf) return;

	while (pos < dir_size) {
		uint64_t frag_addr = ufs2_block_map(sb, din, iblk);
		off_t blk_off;
		uint8_t *p, *end;

		if (!frag_addr) { pos += sb->bsize; iblk++; continue; }
		blk_off = (off_t)(frag_addr / sb->frag) * sb->bsize;
		if (read_at(blk_off, blk_buf, sb->bsize) != 0) break;

		p   = blk_buf;
		end = blk_buf + sb->bsize;
		while (p < end) {
			ufs_direct_t *de = (ufs_direct_t *)p;
			uint32_t ino    = de->d_ino;
			uint16_t reclen = de->d_reclen;
			if (reclen < 8 || reclen > (uint16_t)(end - p)) break;
			if (ino != 0) {
				char name[256];
				memcpy(name, de->d_name, de->d_namlen);
				name[de->d_namlen] = '\0';
				printf("  ino=%-4u type=%u name=%s\n",
				       ino, de->d_type, name);
			}
			p   += reclen;
			pos += reclen;
		}
		iblk++;
		if (pos % sb->bsize) pos += sb->bsize - (pos % sb->bsize);
	}
	free(blk_buf);
}

/* ---- Main ---- */
int main(int argc, char **argv)
{
	ufs_sb_t   sb;
	uint8_t   *filebuf;

	if (argc < 2) {
		fprintf(stderr,
		    "Usage: %s <image-file> [--superblock-only]\n", argv[0]);
		return 1;
	}

	int superblock_only = (argc >= 3 &&
			       strcmp(argv[2], "--superblock-only") == 0);

	fd_img = open(argv[1], O_RDONLY);
	if (fd_img < 0) { perror("open"); return 1; }

	/* ---- Superblock ---- */
	{
		int sbret = read_superblock(&sb);

		if (sbret != 0) {
			if (sbret == -1)
				fprintf(stderr,
				    "ERROR: not a UFS filesystem\n");
			/* -2: error already printed by read_superblock */
			close(fd_img);
			return 1;
		}
	}

	printf("\n=== UFS%s (FFS%s) Superblock ===\n",
	       sb.ufs2 ? "2" : "1", sb.ufs2 ? "2" : "1");
	printf("  Magic    : 0x%08x\n", sb.magic);
	printf("  Volume   : %s\n", sb.volname[0] ? sb.volname : "(unnamed)");
	printf("  bsize    : %u\n", sb.bsize);
	printf("  fsize    : %u\n", sb.fsize);
	printf("  frag     : %u\n", sb.frag);
	printf("  ncg      : %u\n", sb.ncg);
	printf("  ipg      : %u\n", sb.ipg);
	printf("  fpg      : %u\n", sb.fpg);
	printf("  inopb    : %u\n", sb.inopb);
	printf("  iblkno   : %u\n", sb.iblkno);
	printf("  nindir   : %u\n", sb.nindir);

	/* In --superblock-only mode, skip inode/directory/file reading */
	if (superblock_only) {
		close(fd_img);
		return 0;
	}

	if (sb.ufs2) {
		/* ---- UFS2 / FFS2 path ---- */
		ufs2_dinode_t root_ino, file_ino;

		printf("\n=== Root Inode (ino %u) [UFS2] ===\n", UFS_ROOTINO);
		if (read_inode2(&sb, UFS_ROOTINO, &root_ino) != 0) {
			fprintf(stderr, "ERROR: cannot read root inode\n");
			return 1;
		}
		printf("  mode      : 0%o\n",  root_ino.di_mode);
		printf("  nlink     : %d\n",   root_ino.di_nlink);
		printf("  size      : %llu\n", (unsigned long long)root_ino.di_size);
		printf("  di_db[0]  : %lld (frag addr)\n", (long long)root_ino.di_db[0]);

		printf("\n=== Root Directory Contents [UFS2] ===\n");
		list_dir2(&sb, &root_ino);

		printf("\n=== File Inode 3 (hello.txt) [UFS2] ===\n");
		if (read_inode2(&sb, 3, &file_ino) != 0) {
			fprintf(stderr, "ERROR: cannot read file inode\n");
			return 1;
		}
		printf("  mode      : 0%o\n",  file_ino.di_mode);
		printf("  size      : %llu\n", (unsigned long long)file_ino.di_size);
		printf("  di_db[0]  : %lld (frag addr)\n", (long long)file_ino.di_db[0]);

		filebuf = malloc(file_ino.di_size + 1);
		if (!filebuf) { fprintf(stderr, "malloc failed\n"); return 1; }
		if (read_file2(&sb, &file_ino, filebuf, file_ino.di_size) != 0) {
			fprintf(stderr, "ERROR: cannot read file data\n");
			free(filebuf); return 1;
		}
		filebuf[file_ino.di_size] = '\0';
		printf("\n=== File Contents ===\n%s\n", filebuf);
		free(filebuf);

	} else {
		/* ---- UFS1 / FFS1 path ---- */
		ufs1_dinode_t root_ino, file_ino;

		printf("\n=== Root Inode (ino %u) [UFS1] ===\n", UFS_ROOTINO);
		if (read_inode1(&sb, UFS_ROOTINO, &root_ino) != 0) {
			fprintf(stderr, "ERROR: cannot read root inode\n");
			return 1;
		}
		printf("  mode   : 0%o\n", root_ino.di_mode);
		printf("  nlink  : %d\n",  root_ino.di_nlink);
		printf("  size   : %llu\n", (unsigned long long)root_ino.di_size);
		printf("  db[0]  : %u (frag addr)\n", root_ino.di_db[0]);

		printf("\n=== Root Directory Contents [UFS1] ===\n");
		list_dir1(&sb, &root_ino);

		printf("\n=== File Inode 3 (hello.txt) [UFS1] ===\n");
		if (read_inode1(&sb, 3, &file_ino) != 0) {
			fprintf(stderr, "ERROR: cannot read file inode\n");
			return 1;
		}
		printf("  mode   : 0%o\n", file_ino.di_mode);
		printf("  size   : %llu\n", (unsigned long long)file_ino.di_size);
		printf("  db[0]  : %u (frag addr)\n", file_ino.di_db[0]);

		filebuf = malloc(file_ino.di_size + 1);
		if (!filebuf) { fprintf(stderr, "malloc failed\n"); return 1; }
		if (read_file1(&sb, &file_ino, filebuf, file_ino.di_size) != 0) {
			fprintf(stderr, "ERROR: cannot read file data\n");
			free(filebuf); return 1;
		}
		filebuf[file_ino.di_size] = '\0';
		printf("\n=== File Contents ===\n%s\n", filebuf);
		free(filebuf);
	}

	close(fd_img);
	printf("=== All checks PASSED ===\n");
	return 0;
}
