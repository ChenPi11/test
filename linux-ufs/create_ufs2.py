#!/usr/bin/env python3
"""
create_ufs2.py – create a minimal UFS2 (OpenBSD/FreeBSD FFS2) filesystem
image for testing the linux_ufs.ko kernel driver.

Key differences from UFS1 (create_ufs1.py):
  • Magic   = 0x19540119  (UFS2_MAGIC / FS_UFS2_MAGIC)
  • Superblock at byte offset 65536  (not 8192)
  • ufs2_dinode is 256 bytes  (not 128); inopb = bsize/256
  • di_db[] / di_ib[] are 64-bit  (not 32-bit)
  • di_blocks is in bytes  (not 512-byte sectors)
  • cgstart(cg) = cgbase(cg)  (no rotational-layout fudge for UFS2)
  • iblkno must be placed AFTER the superblock+CG-descriptor area

Image layout (bsize=4096, fsize=512, frag=8):
  offset       0 –  65535   boot / reserved area
  offset   65536 –  66911   UFS2 superblock  (1376 bytes, magic at +1372)
  offset   81920 –  86015   inode table block (iblkno=160, 4096 bytes,
                             holds 16 × 256-byte dinodes)
  offset   86016 –  90111   root directory data   (frag 168)
  offset   90112 –  94207   hello.txt data        (frag 176)

  Total image size: 131072 bytes (128 KiB)

Parameters:
  bsize   = 4096   block size
  fsize   = 512    fragment size
  frag    = 8      fragments per block
  ncg     = 1      cylinder groups
  ipg     = 64     inodes per group
  fpg     = 512    fragments per group  (fpg * fsize = 256 KiB per CG)
  inopb   = bsize / 256 = 16   (ufs2_dinode = 256 bytes)
  nindir  = bsize / 8   = 512  (64-bit pointers)
  iblkno  = 160  (first inode-block frag within CG 0; after superblock area)
"""

import struct
import math
import sys
import time

# ──────────────────────────────────────────────────────────────
# On-disk constants (matching ufs_fs.h)
# ──────────────────────────────────────────────────────────────
UFS2_MAGIC = 0x19540119
ROOTINO    = 2
NDADDR     = 12
NIADDR     = 3

# Filesystem parameters
BSIZE   = 4096
FSIZE   = 512
FRAG    = BSIZE // FSIZE        # 8
NCG     = 1
IPG     = 64
FPG     = 512                   # fragments per CG
INOPB   = BSIZE // 256          # 16  (ufs2_dinode = 256 bytes)
NINDIR  = BSIZE // 8            # 512 (64-bit indirect pointers)

# Fragment addresses within CG 0
# Superblock at offset 65536 = frag 128; occupies frags 128..143 (8192 bytes)
# CG descriptor follows at frag 144..147 (nominal), inode table at 160
SBLOCK_FRAG   = 65536 // FSIZE  # 128
IBLKNO_FRAG   = 160             # iblkno value; inode table start
ROOT_DIR_FRAG = IBLKNO_FRAG + FRAG          # 168 – root directory data
HELLO_FRAG    = ROOT_DIR_FRAG   + FRAG      # 176 – hello.txt data

IMAGE_SIZE = 131072             # 128 KiB

# ──────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────
def frag_to_offset(frag):
    return frag * FSIZE

def write_at(buf, offset, data):
    buf[offset:offset + len(data)] = data


# ──────────────────────────────────────────────────────────────
# Build the UFS2 superblock (struct ufs_super_block, 1376 bytes)
# ──────────────────────────────────────────────────────────────
# Only the fields read by the kernel driver are set; the rest are 0.
# See ufs_fs.h for the precise byte offsets.
def build_superblock():
    sb = bytearray(1376)

    def w32(off, val):
        struct.pack_into('<I', sb, off, val & 0xFFFFFFFF)

    def w32s(off, val):
        struct.pack_into('<i', sb, off, val)

    def w64(off, val):
        struct.pack_into('<Q', sb, off, val & 0xFFFFFFFFFFFFFFFF)

    # Basic layout
    w32(8,   SBLOCK_FRAG)   # fs_sblkno
    w32(16,  IBLKNO_FRAG)   # fs_iblkno (frag offset within CG)
    w32(24,  0)             # fs_cgoffset  (unused for UFS2)
    w32(28,  0xFFFFFFFF)    # fs_cgmask    (unused for UFS2)
    w32(44,  NCG)           # fs_ncg
    w32(48,  BSIZE)         # fs_bsize
    w32(52,  FSIZE)         # fs_fsize
    w32(56,  FRAG)          # fs_frag
    w32(60,  5)             # fs_minfree (5 %)
    w32(96,  int(math.log2(FRAG)))          # fs_fragshift = 3
    w32(100, int(math.log2(FSIZE // 512)))  # fs_fsbtodb  = 0
    w32(116, NINDIR)        # fs_nindir
    w32(120, INOPB)         # fs_inopb
    w32(184, IPG)           # fs_ipg
    w32(188, FPG)           # fs_fpg

    # UFS2 cylinder summary lives in the 64-bit csum_total area
    # (offsets 1008–1071 in struct ufs_super_block from ufs_fs.h)
    ndir   = 1
    nifree = IPG * NCG - 3          # ino 0,1 reserved; ino 2=root, 3=hello
    w64(1008, ndir)                  # fs_ufs2_cs_ndir
    # fs_ufs2_cs_nbfree / nffree left 0 for a minimal test image
    w64(1024, nifree)                # fs_ufs2_cs_nifree

    # UFS2 total block count (fs_size at offset 1080)
    w64(1080, FPG * NCG)             # fs_size (total fragments)

    # Clean flag at offset 209
    sb[209] = 1     # fs_clean = FS_ISCLEAN

    # Volume name at offset 680
    volname = b'TestVolFFS2\x00'
    sb[680:680 + len(volname)] = volname

    # UFS2 magic at offset 1372
    w32(1372, UFS2_MAGIC)

    return bytes(sb)


# ──────────────────────────────────────────────────────────────
# Build a UFS2 on-disk inode (256 bytes)
# ──────────────────────────────────────────────────────────────
# Layout matches struct ufs2_dinode in ufs_fs.h.
def build_ufs2_dinode(mode, nlink, uid, gid, size, atime, mtime, ctime,
                      birthtime=None, db=None, blocks_bytes=0, gen=1):
    """
    mode        – file type + permissions
    db          – list of up to NDADDR direct block fragment addresses
    blocks_bytes – di_blocks value (bytes actually held; FFS2 uses bytes)
    """
    if db is None:
        db = []
    if birthtime is None:
        birthtime = ctime

    inode = bytearray(256)

    def w16(off, val):
        struct.pack_into('<H', inode, off, val & 0xFFFF)
    def w32(off, val):
        struct.pack_into('<I', inode, off, val & 0xFFFFFFFF)
    def w64(off, val):
        struct.pack_into('<Q', inode, off, val & 0xFFFFFFFFFFFFFFFF)
    def w64s(off, val):
        struct.pack_into('<q', inode, off, val)

    w16(0,   mode)          # di_mode
    w16(2,   nlink)         # di_nlink
    w32(4,   uid)           # di_uid
    w32(8,   gid)           # di_gid
    w32(12,  BSIZE)         # di_blksize
    w64(16,  size)          # di_size
    w64(24,  blocks_bytes)  # di_blocks (bytes)
    w64s(32, atime)         # di_atime
    w64s(40, mtime)         # di_mtime
    w64s(48, ctime)         # di_ctime
    w64s(56, birthtime)     # di_birthtime (FFS2 creation time)
    w32(64,  0)             # di_mtimensec
    w32(68,  0)             # di_atimensec
    w32(72,  0)             # di_ctimensec
    w32(76,  0)             # di_birthnsec
    w32(80,  gen)           # di_gen

    # di_db[0..11] at offset 112 (each 8 bytes, 64-bit)
    for i, frag in enumerate(db[:NDADDR]):
        w64(112 + i * 8, frag)

    return bytes(inode)


# ──────────────────────────────────────────────────────────────
# Build a directory block (identical format to UFS1 – dir entries
# use 32-bit inodes regardless of UFS version)
# ──────────────────────────────────────────────────────────────
def build_dir_entry(ino, ftype, name):
    namlen   = len(name)
    min_size = ((8 + namlen + 1) + 3) & ~3
    entry    = bytearray(min_size)
    struct.pack_into('<I', entry, 0, ino)
    struct.pack_into('<H', entry, 4, min_size)
    entry[6] = ftype
    entry[7] = namlen
    entry[8:8 + namlen] = name.encode()
    return bytes(entry)


def build_dir_block(entries, block_size=BSIZE):
    raw  = [build_dir_entry(ino, ft, nm) for ino, ft, nm in entries]
    total_before_last = sum(len(e) for e in raw[:-1])
    last_reclen = block_size - total_before_last
    last = bytearray(raw[-1])
    struct.pack_into('<H', last, 4, last_reclen)
    raw[-1] = bytes(last)

    block = bytearray(block_size)
    off = 0
    for e in raw:
        block[off:off + len(e)] = e
        off += len(e)
    return bytes(block)


# ──────────────────────────────────────────────────────────────
# Main image builder
# ──────────────────────────────────────────────────────────────
def create_ufs2_image(path):
    buf = bytearray(IMAGE_SIZE)
    now = int(time.time())

    # ── UFS2 superblock at offset 65536 ──────────────────────
    sb_data = build_superblock()
    write_at(buf, frag_to_offset(SBLOCK_FRAG), sb_data)

    # ── Inode table at offset 81920 (frag 160) ───────────────
    # Inode 0: unused (zeroed)
    # Inode 1: unused (zeroed)
    # Inode 2: root directory
    # Inode 3: hello.txt

    hello_content = b'Hello from FFS2 (UFS2)!\nThis is a test file on an OpenBSD Fast File System 2.\n'

    # Inode 2 – root directory
    root_inode = build_ufs2_dinode(
        mode         = 0o040755,
        nlink        = 3,
        uid=0, gid=0,
        size         = BSIZE,
        atime=now, mtime=now, ctime=now,
        db           = [ROOT_DIR_FRAG],
        blocks_bytes = BSIZE,           # bytes actually held
    )

    # Inode 3 – hello.txt
    hello_inode = build_ufs2_dinode(
        mode         = 0o0100644,
        nlink        = 1,
        uid=0, gid=0,
        size         = len(hello_content),
        atime=now, mtime=now, ctime=now,
        db           = [HELLO_FRAG],
        blocks_bytes = BSIZE,
    )

    # Each UFS2 dinode is 256 bytes; INOPB=16 dinodes per 4096-byte block.
    inode_table_offset = frag_to_offset(IBLKNO_FRAG)
    write_at(buf, inode_table_offset + 2 * 256, root_inode)   # inode 2
    write_at(buf, inode_table_offset + 3 * 256, hello_inode)  # inode 3

    # ── Root directory data at frag 168 ──────────────────────
    dir_block = build_dir_block([
        (2, 4, '.'),
        (2, 4, '..'),
        (3, 8, 'hello.txt'),
    ])
    write_at(buf, frag_to_offset(ROOT_DIR_FRAG), dir_block)

    # ── hello.txt data at frag 176 ───────────────────────────
    write_at(buf, frag_to_offset(HELLO_FRAG), hello_content)

    with open(path, 'wb') as f:
        f.write(buf)

    print(f"Created UFS2/FFS2 image: {path} ({IMAGE_SIZE} bytes)")
    print(f"  Superblock at offset {frag_to_offset(SBLOCK_FRAG)} (frag {SBLOCK_FRAG})")
    print(f"  Inode table at offset {frag_to_offset(IBLKNO_FRAG)} (frag {IBLKNO_FRAG})")
    print(f"  Root dir at offset {frag_to_offset(ROOT_DIR_FRAG)} (frag {ROOT_DIR_FRAG})")
    print(f"  hello.txt at offset {frag_to_offset(HELLO_FRAG)} (frag {HELLO_FRAG})")


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/test.ufs2'
    create_ufs2_image(out)
