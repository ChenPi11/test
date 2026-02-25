#!/usr/bin/env python3
"""
create_ufs1.py – create a minimal UFS1 (BSD FFS) filesystem image for testing.

The image layout (all sizes in bytes unless noted):
  offset      0 –   8191  boot block (zeroed)
  offset   8192 –   9567  UFS1 superblock (struct fs, 1376 bytes)
  offset  16384 – 17407   cylinder-group block (partial, 1024 bytes)
  offset  32768 – 36863   inode table (8 × 512-byte frags = 4096 bytes = 1 block)
  offset  40960 – 45055   data: root directory (1 block = 4096 bytes)
  offset  45056 – 49151   data: hello.txt file (1 block = 4096 bytes)

Parameters used:
  bsize  = 4096   block size
  fsize  = 512    fragment size
  frag   = 8      fragments per block
  ncg    = 1      cylinder groups
  ipg    = 64     inodes per group
  fpg    = 128    fragments per group
  inopb  = bsize / 128 = 32   inodes per block
  nindir = bsize / 4 = 1024   indirect pointers per block
  iblkno = 4      first inode-block fragment within CG (after sblock+cgblock)
  dblkno = 12     first data fragment within CG (4+8=12, i.e. iblkno + 1 block)

Fragment address layout:
  frag 0  – reserved / boot
  frag 16 – superblock (offset 8192)
  frag 32 – CG block   (offset 16384)
  frag 64 – inode table block (offset 32768) [iblkno=64 chosen to keep it simple]
  frag 80 – root dir   (offset 40960)
  frag 88 – hello.txt  (offset 45056)
"""

import struct
import math
import os
import sys

# ──────────────────────────────────────────────────────────────
# Constants matching ufs_fs.h
# ──────────────────────────────────────────────────────────────
UFS_MAGIC    = 0x011954
ROOTINO      = 2
NDADDR       = 12
NIADDR       = 3

# Filesystem parameters
BSIZE   = 4096
FSIZE   = 512
FRAG    = BSIZE // FSIZE      # 8
NCG     = 1
IPG     = 64                   # inodes per cylinder group
FPG     = 128                  # fragments per cylinder group
INOPB   = BSIZE // 128         # 32 inodes per block (ufs1_dinode = 128 bytes)
NINDIR  = BSIZE // 4           # 1024 indirect pointers (32-bit)

# Fragment addresses (all within CG 0)
SBLOCK_FRAG  = 8192  // FSIZE   # 16 – superblock
CG_FRAG      = 16384 // FSIZE   # 32 – cylinder-group block
IBLKNO_FRAG  = 32768 // FSIZE   # 64 – inode table start (iblkno field value)
ROOT_DIR_FRAG= 40960 // FSIZE   # 80 – root directory data
HELLO_FRAG   = 45056 // FSIZE   # 88 – hello.txt data

# Image size: enough for all data
IMAGE_SIZE = 49152 + BSIZE

# ──────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────
def frag_to_offset(frag):
    return frag * FSIZE

def write_at(buf, offset, data):
    buf[offset:offset + len(data)] = data


# ──────────────────────────────────────────────────────────────
# Build the superblock (struct ufs_super_block, 1376 bytes)
# ──────────────────────────────────────────────────────────────
# We only set fields our driver actually reads; the rest stay 0.
# The struct layout is validated in ufs_fs.h comments.
def build_superblock():
    sb = bytearray(1376)

    def w32(off, val):
        struct.pack_into('<I', sb, off, val & 0xFFFFFFFF)

    def w32s(off, val):
        struct.pack_into('<i', sb, off, val)

    def w64(off, val):
        struct.pack_into('<Q', sb, off, val & 0xFFFFFFFFFFFFFFFF)

    # Basic layout fields (offsets from ufs_fs.h)
    w32(8,   SBLOCK_FRAG)          # fs_sblkno
    w32(16,  IBLKNO_FRAG)          # fs_iblkno (frag addr of inode table in CG)
    w32(24,  0)                    # fs_cgoffset  (UFS1: CG layout offset)
    w32(28,  0xFFFFFFFF)           # fs_cgmask    (UFS1: CG layout mask)
    w32s(36, FPG * NCG)            # fs_ffs1_size (total frags)
    w32(44,  NCG)                  # fs_ncg
    w32(48,  BSIZE)                # fs_bsize
    w32(52,  FSIZE)                # fs_fsize
    w32(56,  FRAG)                 # fs_frag
    w32(60,  5)                    # fs_minfree (5 %)
    w32(96,  int(math.log2(FRAG))) # fs_fragshift = log2(frag) = 3
    w32(100, int(math.log2(FSIZE // 512)))  # fs_fsbtodb: frag→512-B-sector shift
                                            # FSIZE=512 → shift=0
    w32(116, NINDIR)               # fs_nindir
    w32(120, INOPB)                # fs_inopb
    w32(184, IPG)                  # fs_ipg
    w32(188, FPG)                  # fs_fpg

    # Cylinder summary (UFS1)
    ndir   = 1    # only the root directory
    nifree = IPG * NCG - 3  # inode 0,1 reserved + root=2 + hello.txt=3
    w32(192, ndir)                 # fs_cs_ndir
    w32(200, nifree)               # fs_cs_nifree

    # Clean flag
    sb[209] = 1  # fs_clean = FS_ISCLEAN

    # Volume name at offset 680
    volname = b'TestVol\x00'
    sb[680:680 + len(volname)] = volname

    # UFS1 magic at offset 1372
    w32(1372, UFS_MAGIC)

    return bytes(sb)


# ──────────────────────────────────────────────────────────────
# Build a UFS1 on-disk inode (128 bytes)
# ──────────────────────────────────────────────────────────────
def build_ufs1_dinode(mode, nlink, uid, gid, size, atime, mtime, ctime,
                      db=None, blocks=0, gen=1):
    """
    mode   – file type + permissions (e.g. 0o040755 for directory)
    db     – list of up to NDADDR direct block fragment addresses (or None)
    blocks – number of 512-byte disk blocks held
    """
    if db is None:
        db = []

    inode = bytearray(128)

    def w16(off, val):
        struct.pack_into('<H', inode, off, val & 0xFFFF)

    def w32(off, val):
        struct.pack_into('<I', inode, off, val & 0xFFFFFFFF)

    def w64(off, val):
        struct.pack_into('<Q', inode, off, val & 0xFFFFFFFFFFFFFFFF)

    w16(0,   mode)          # di_mode
    w16(2,   nlink)         # di_nlink
    # di_ouid / di_ogid at offset 4/6 (16-bit compat fields, set to 0)
    w64(8,   size)          # di_size
    w32(16,  atime)         # di_atime
    w32(20,  0)             # di_atimensec
    w32(24,  mtime)         # di_mtime
    w32(28,  0)             # di_mtimensec
    w32(32,  ctime)         # di_ctime
    w32(36,  0)             # di_ctimensec

    # di_db[0..11] at offset 40
    for i, frag in enumerate(db[:NDADDR]):
        w32(40 + i * 4, frag)

    w32(104, blocks)        # di_blocks (512-byte units)
    w32(108, gen)           # di_gen
    w32(112, uid)           # di_uid (32-bit)
    w32(116, gid)           # di_gid (32-bit)

    return bytes(inode)


# ──────────────────────────────────────────────────────────────
# Build a directory block
# ──────────────────────────────────────────────────────────────
def build_dir_entry(ino, ftype, name):
    """Build a single directory entry (struct ufs_direct)."""
    namlen = len(name)
    # Minimum record size: 8 (header) + namlen + 1 (NUL), padded to 4
    min_size = ((8 + namlen + 1) + 3) & ~3
    entry = bytearray(min_size)
    struct.pack_into('<I', entry, 0, ino)     # d_ino
    struct.pack_into('<H', entry, 4, min_size)  # d_reclen
    entry[6] = ftype                           # d_type
    entry[7] = namlen                          # d_namlen
    entry[8:8 + namlen] = name.encode()       # d_name
    return bytes(entry)


def build_dir_block(entries, block_size=BSIZE):
    """
    Build a full directory block (block_size bytes) from a list of
    (ino, ftype, name) tuples.  The last entry's d_reclen is stretched to
    fill the remainder of the block (UFS directory convention).
    """
    raw_entries = [build_dir_entry(ino, ft, nm) for ino, ft, nm in entries]
    total = sum(len(e) for e in raw_entries)
    assert total <= block_size, "directory entries overflow block"

    # The last entry's reclen must extend to the end of the block.
    # total_before_last = offset where the last entry starts.
    total_before_last = sum(len(e) for e in raw_entries[:-1])
    last_reclen = block_size - total_before_last   # from entry start to block end
    last = bytearray(raw_entries[-1])
    struct.pack_into('<H', last, 4, last_reclen)
    raw_entries[-1] = bytes(last)

    block = bytearray(block_size)
    off = 0
    for e in raw_entries:
        block[off:off + len(e)] = e
        off += len(e)
    return bytes(block)


# ──────────────────────────────────────────────────────────────
# Main image builder
# ──────────────────────────────────────────────────────────────
def create_ufs1_image(path):
    buf = bytearray(IMAGE_SIZE)

    # ── Superblock at offset 8192 ────────────────────────────
    sb_data = build_superblock()
    write_at(buf, frag_to_offset(SBLOCK_FRAG), sb_data)

    # ── Inode table at offset 32768 ─────────────────────────
    # Inode 0: unused (all-zero)
    # Inode 1: unused (historically bad blocks, keep zeroed)
    # Inode 2: root directory
    # Inode 3: hello.txt

    import time
    now = int(time.time())

    # Inode 2 – root directory
    # Blocks held: 1 block = BSIZE/512 = 8 sectors
    root_inode = build_ufs1_dinode(
        mode   = 0o040755,          # directory, rwxr-xr-x
        nlink  = 3,                 # '.' + '..' (self) + 'hello.txt' entry
        uid=0, gid=0,
        size   = BSIZE,             # one full data block
        atime=now, mtime=now, ctime=now,
        db     = [ROOT_DIR_FRAG],   # fragment address of directory data
        blocks = BSIZE // 512,
    )
    # Inode 3 – hello.txt
    hello_content = b'Hello from UFS1!\nThis is a test file on a BSD Fast File System.\n'
    hello_inode = build_ufs1_dinode(
        mode   = 0o0100644,         # regular file, rw-r--r--
        nlink  = 1,
        uid=0, gid=0,
        size   = len(hello_content),
        atime=now, mtime=now, ctime=now,
        db     = [HELLO_FRAG],
        blocks = BSIZE // 512,
    )

    # Write inode table: each inode is 128 bytes, INOPB=32 per 4096-byte block
    inode_table_offset = frag_to_offset(IBLKNO_FRAG)
    write_at(buf, inode_table_offset + 2 * 128, root_inode)   # inode 2
    write_at(buf, inode_table_offset + 3 * 128, hello_inode)  # inode 3

    # ── Root directory data at offset 40960 ─────────────────
    # DT values: DT_DIR=4, DT_REG=8
    dir_block = build_dir_block([
        (2, 4, '.'),
        (2, 4, '..'),
        (3, 8, 'hello.txt'),
    ])
    write_at(buf, frag_to_offset(ROOT_DIR_FRAG), dir_block)

    # ── hello.txt data at offset 45056 ──────────────────────
    write_at(buf, frag_to_offset(HELLO_FRAG), hello_content)

    with open(path, 'wb') as f:
        f.write(buf)

    print(f"Created UFS1 image: {path} ({IMAGE_SIZE} bytes)")
    print(f"  Superblock at offset {frag_to_offset(SBLOCK_FRAG)} (frag {SBLOCK_FRAG})")
    print(f"  Inode table at offset {frag_to_offset(IBLKNO_FRAG)} (frag {IBLKNO_FRAG})")
    print(f"  Root dir at offset {frag_to_offset(ROOT_DIR_FRAG)} (frag {ROOT_DIR_FRAG})")
    print(f"  hello.txt at offset {frag_to_offset(HELLO_FRAG)} (frag {HELLO_FRAG})")


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/test.ufs'
    create_ufs1_image(out)
