#!/usr/bin/env python3
"""
create_test_image.py – Create comprehensive FFS1/FFS2 test images for linux_ufs driver testing.

Based on OpenBSD sys/ufs/ filesystem structures, this script creates disk images
that exercise every file type FFS supports:

  1. Regular file       – content, size, permissions, uid/gid, timestamps
  2. Nested directory   – traversal, dot/dotdot links
  3. Hard link          – two dirents for the same inode (nlink=2)
  4. Inline symlink     – short target stored in the inode block-pointer area
  5. File-backed symlink – long target stored in a data block
  6. Character device   – MKDEV(1,3) ≈ /dev/null
  7. Block device       – MKDEV(8,0) ≈ /dev/sda
  8. FIFO               – named pipe inode
  9. Unix socket        – socket inode
 10. Setuid regular file – mode 04755

Directory tree:
  /                       mode 0755, uid 0,    nlink 3
  /regular.txt            mode 0644, uid 1000, regular file, "Hello FFS!\\n"
  /subdir/                mode 0755, uid 0,    nlink 2
  /subdir/nested.txt      mode 0600, uid 1000, regular file, "Nested file.\\n"
  /inline_link  -> /regular.txt          inline symlink (fits in inode)
  /long_link    -> (125-char target)     file-backed symlink (exceeds inline max)
  /hardlink     (ino == ino of regular.txt)  hard link
  /chardev      major=1, minor=3  (character device)
  /blkdev       major=8, minor=0  (block device)
  /fifo         FIFO
  /socket       Unix socket
  /setuid_prog  mode 04755, regular file, "#!/bin/sh\\necho setuid\\n"

Usage:
  python3 create_test_image.py ffs1 /tmp/test_ffs1.img
  python3 create_test_image.py ffs2 /tmp/test_ffs2.img
"""

import struct
import math
import sys
import time

# ─────────────────────────────────────────────────────────────────────────────
# Shared on-disk constants (matching ufs_fs.h)
# ─────────────────────────────────────────────────────────────────────────────
UFS1_MAGIC = 0x011954
UFS2_MAGIC = 0x19540119

ROOTINO = 2
NDADDR  = 12
NIADDR  = 3

# Linux MKDEV encoding: (major << 20) | minor
# Used to store rdev in di_db[0] for char/block device inodes.
def MKDEV(major, minor):
    return (major << 20) | minor

CHARDEV_RDEV = MKDEV(1, 3)   # like /dev/null
BLKDEV_RDEV  = MKDEV(8, 0)   # like /dev/sda

# Symlink targets
INLINE_LINK_TARGET = b'/regular.txt'          # 12 bytes – always fits inline
# 125-byte target – exceeds both FFS1 (60 B) and FFS2 (120 B) inline limits
LONG_LINK_TARGET   = b'/long/symlink/target/stored/in/data/block' + b'X' * 84

FILE_CONTENT_REGULAR = b'Hello FFS!\n'          # 11 bytes
FILE_CONTENT_NESTED  = b'Nested file.\n'         # 13 bytes
FILE_CONTENT_SETUID  = b'#!/bin/sh\necho setuid\n'  # 21 bytes

assert len(LONG_LINK_TARGET) == 125
assert len(LONG_LINK_TARGET) > 60   # exceeds FFS1 inline limit
assert len(LONG_LINK_TARGET) > 120  # exceeds FFS2 inline limit

# ─────────────────────────────────────────────────────────────────────────────
# Inode number assignments (same for both FFS1 and FFS2)
# ─────────────────────────────────────────────────────────────────────────────
INO_ROOT       = 2
INO_REGULAR    = 3   # /regular.txt  (nlink=2, also referenced by /hardlink)
INO_SUBDIR     = 4   # /subdir/
INO_NESTED     = 5   # /subdir/nested.txt
INO_INLINE_LNK = 6   # /inline_link
INO_LONG_LNK   = 7   # /long_link
INO_CHARDEV    = 8   # /chardev
INO_BLKDEV     = 9   # /blkdev
INO_FIFO       = 10  # /fifo
INO_SOCKET     = 11  # /socket
INO_SETUID     = 12  # /setuid_prog


# ─────────────────────────────────────────────────────────────────────────────
# Helper: write bytes into a bytearray buffer
# ─────────────────────────────────────────────────────────────────────────────
def write_at(buf, offset, data):
    buf[offset:offset + len(data)] = data


# ─────────────────────────────────────────────────────────────────────────────
# Directory entry builder (shared between FFS1 and FFS2)
# ─────────────────────────────────────────────────────────────────────────────
def build_dir_entry(ino, ftype, name):
    """Build a single UFS directory entry (struct direct)."""
    namlen   = len(name)
    min_size = ((8 + namlen + 1) + 3) & ~3
    entry    = bytearray(min_size)
    struct.pack_into('<I', entry, 0, ino)
    struct.pack_into('<H', entry, 4, min_size)
    entry[6] = ftype
    entry[7] = namlen
    entry[8:8 + namlen] = name.encode()
    return bytes(entry)


def build_dir_block(entries, block_size):
    """
    Build a full directory block from a list of (ino, ftype, name) tuples.
    The last entry's d_reclen is stretched to fill the block (BSD convention).
    """
    raw = [build_dir_entry(ino, ft, nm) for ino, ft, nm in entries]
    total_before_last = sum(len(e) for e in raw[:-1])
    assert total_before_last + len(raw[-1]) <= block_size, \
        "directory entries overflow block"
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


# ─────────────────────────────────────────────────────────────────────────────
# FFS1 image builder
# ─────────────────────────────────────────────────────────────────────────────

# FFS1 layout parameters
FFS1_BSIZE  = 4096
FFS1_FSIZE  = 512
FFS1_FRAG   = FFS1_BSIZE // FFS1_FSIZE   # 8
FFS1_NCG    = 1
FFS1_IPG    = 64
FFS1_FPG    = 128    # fragments per CG  → image = 128 × 512 = 65536 bytes
FFS1_INOPB  = FFS1_BSIZE // 128           # 32  (ufs1_dinode = 128 bytes)
FFS1_NINDIR = FFS1_BSIZE // 4             # 1024 (32-bit indirect pointers)

# Fragment addresses within CG 0 (one CG, fpg=128)
FFS1_SBLOCK_FRAG   = 8192  // FFS1_FSIZE   # 16  – superblock
FFS1_IBLKNO_FRAG   = 32768 // FFS1_FSIZE   # 64  – inode table (iblkno field)
FFS1_ROOT_FRAG     = 80                     # root dir  data block (offset 40960)
FFS1_SUBDIR_FRAG   = 88                     # subdir    data block (offset 45056)
FFS1_REGULAR_FRAG  = 96                     # regular.txt            (offset 49152)
FFS1_NESTED_FRAG   = 104                    # nested.txt             (offset 53248)
FFS1_LONGLN_FRAG   = 112                    # long_link target       (offset 57344)
FFS1_SETUID_FRAG   = 120                    # setuid_prog            (offset 61440)
FFS1_IMAGE_SIZE    = FFS1_FPG * FFS1_FSIZE  # 65536 bytes


def ffs1_build_superblock(now):
    sb = bytearray(1376)
    def w32(off, val): struct.pack_into('<I', sb, off, val & 0xFFFFFFFF)
    def w32s(off, val): struct.pack_into('<i', sb, off, val)

    w32(8,   FFS1_SBLOCK_FRAG)             # fs_sblkno
    w32(16,  FFS1_IBLKNO_FRAG)             # fs_iblkno
    w32(24,  0)                             # fs_cgoffset (UFS1 layout offset)
    w32(28,  0xFFFFFFFF)                    # fs_cgmask
    w32s(36, FFS1_FPG * FFS1_NCG)          # fs_ffs1_size (total frags)
    w32(44,  FFS1_NCG)                      # fs_ncg
    w32(48,  FFS1_BSIZE)                    # fs_bsize
    w32(52,  FFS1_FSIZE)                    # fs_fsize
    w32(56,  FFS1_FRAG)                     # fs_frag
    w32(60,  5)                             # fs_minfree
    w32(96,  int(math.log2(FFS1_FRAG)))     # fs_fragshift = 3
    w32(100, int(math.log2(FFS1_FSIZE // 512)))  # fs_fsbtodb = 0
    w32(116, FFS1_NINDIR)                   # fs_nindir
    w32(120, FFS1_INOPB)                    # fs_inopb
    w32(184, FFS1_IPG)                      # fs_ipg
    w32(188, FFS1_FPG)                      # fs_fpg

    # UFS1 cylinder summary
    nifree = FFS1_IPG * FFS1_NCG - (INO_SETUID + 1)
    w32(192, 2)                             # fs_cs_ndir (root + subdir)
    w32(200, max(0, nifree))                # fs_cs_nifree

    sb[209] = 1                             # fs_clean = FS_ISCLEAN

    volname = b'TestFFS1\x00'
    sb[680:680 + len(volname)] = volname
    w32(1372, UFS1_MAGIC)
    return bytes(sb)


def ffs1_build_dinode(mode, nlink, uid, gid, size, atime, mtime, ctime,
                      db=None, blocks=0, gen=1,
                      inline_data=None, rdev=None):
    """
    Build a UFS1 on-disk inode (128 bytes).

    inline_data  – bytes to write directly into the di_db/di_ib area
                   (used for inline symlinks and devices); supersedes db[].
    rdev         – device number stored in di_db[0] (for char/block devices).
    db           – list of fragment addresses for di_db[0..11].
    """
    inode = bytearray(128)
    def w16(off, val): struct.pack_into('<H', inode, off, val & 0xFFFF)
    def w32(off, val): struct.pack_into('<I', inode, off, val & 0xFFFFFFFF)
    def w64(off, val): struct.pack_into('<Q', inode, off, val & 0xFFFFFFFFFFFFFFFF)

    w16(0,  mode)
    w16(2,  nlink)
    w64(8,  size)
    w32(16, atime);  w32(20, 0)
    w32(24, mtime);  w32(28, 0)
    w32(32, ctime);  w32(36, 0)
    w32(104, blocks)
    w32(108, gen)
    w32(112, uid)
    w32(116, gid)

    # di_db[0..11] starts at offset 40 in the UFS1 inode.
    if inline_data is not None:
        # Inline symlink: target bytes go directly into the block-pointer area.
        inode[40:40 + len(inline_data)] = inline_data
    elif rdev is not None:
        # Char/block device: rdev is stored in di_db[0].
        w32(40, rdev)
    elif db:
        for i, frag in enumerate(db[:NDADDR]):
            w32(40 + i * 4, frag)

    return bytes(inode)


def create_ffs1_image(path):
    buf = bytearray(FFS1_IMAGE_SIZE)
    now = int(time.time())

    # ── Superblock ────────────────────────────────────────────────────────────
    write_at(buf, FFS1_SBLOCK_FRAG * FFS1_FSIZE, ffs1_build_superblock(now))

    # ── Helper: write an inode at its correct offset in the inode table ───────
    def put_inode(ino, inode_bytes):
        # All our inodes (2-12) are in inode block 0 (frags FFS1_IBLKNO_FRAG..)
        off = FFS1_IBLKNO_FRAG * FFS1_FSIZE + ino * 128
        write_at(buf, off, inode_bytes)

    # ── Inodes ────────────────────────────────────────────────────────────────

    # ino 2: root directory
    put_inode(INO_ROOT, ffs1_build_dinode(
        mode=0o040755, nlink=3, uid=0, gid=0,
        size=FFS1_BSIZE, atime=now, mtime=now, ctime=now,
        db=[FFS1_ROOT_FRAG], blocks=FFS1_BSIZE // 512))

    # ino 3: regular.txt  (nlink=2 because /hardlink also points here)
    put_inode(INO_REGULAR, ffs1_build_dinode(
        mode=0o100644, nlink=2, uid=1000, gid=1000,
        size=len(FILE_CONTENT_REGULAR), atime=now, mtime=now, ctime=now,
        db=[FFS1_REGULAR_FRAG], blocks=FFS1_BSIZE // 512))

    # ino 4: subdir
    put_inode(INO_SUBDIR, ffs1_build_dinode(
        mode=0o040755, nlink=2, uid=0, gid=0,
        size=FFS1_BSIZE, atime=now, mtime=now, ctime=now,
        db=[FFS1_SUBDIR_FRAG], blocks=FFS1_BSIZE // 512))

    # ino 5: subdir/nested.txt
    put_inode(INO_NESTED, ffs1_build_dinode(
        mode=0o100600, nlink=1, uid=1000, gid=1000,
        size=len(FILE_CONTENT_NESTED), atime=now, mtime=now, ctime=now,
        db=[FFS1_NESTED_FRAG], blocks=FFS1_BSIZE // 512))

    # ino 6: inline_link (short symlink stored inline in inode)
    put_inode(INO_INLINE_LNK, ffs1_build_dinode(
        mode=0o120777, nlink=1, uid=0, gid=0,
        size=len(INLINE_LINK_TARGET), atime=now, mtime=now, ctime=now,
        inline_data=INLINE_LINK_TARGET, blocks=0))

    # ino 7: long_link (long symlink stored in data block)
    put_inode(INO_LONG_LNK, ffs1_build_dinode(
        mode=0o120777, nlink=1, uid=0, gid=0,
        size=len(LONG_LINK_TARGET), atime=now, mtime=now, ctime=now,
        db=[FFS1_LONGLN_FRAG], blocks=FFS1_BSIZE // 512))

    # ino 8: character device
    put_inode(INO_CHARDEV, ffs1_build_dinode(
        mode=0o020644, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        rdev=CHARDEV_RDEV, blocks=0))

    # ino 9: block device
    put_inode(INO_BLKDEV, ffs1_build_dinode(
        mode=0o060640, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        rdev=BLKDEV_RDEV, blocks=0))

    # ino 10: FIFO
    put_inode(INO_FIFO, ffs1_build_dinode(
        mode=0o010644, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        blocks=0))

    # ino 11: Unix socket
    put_inode(INO_SOCKET, ffs1_build_dinode(
        mode=0o140755, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        blocks=0))

    # ino 12: setuid regular file (mode 04755)
    put_inode(INO_SETUID, ffs1_build_dinode(
        mode=0o104755, nlink=1, uid=0, gid=0,
        size=len(FILE_CONTENT_SETUID), atime=now, mtime=now, ctime=now,
        db=[FFS1_SETUID_FRAG], blocks=FFS1_BSIZE // 512))

    # ── Directory data blocks ─────────────────────────────────────────────────

    # Root directory
    #   DT values: DIR=4, REG=8, LNK=10, CHR=2, BLK=6, FIFO=1, SOCK=12
    root_dir = build_dir_block([
        (INO_ROOT,       4,  '.'),
        (INO_ROOT,       4,  '..'),
        (INO_REGULAR,    8,  'regular.txt'),
        (INO_SUBDIR,     4,  'subdir'),
        (INO_INLINE_LNK, 10, 'inline_link'),
        (INO_LONG_LNK,   10, 'long_link'),
        (INO_REGULAR,    8,  'hardlink'),       # hard link → same ino as regular.txt
        (INO_CHARDEV,    2,  'chardev'),
        (INO_BLKDEV,     6,  'blkdev'),
        (INO_FIFO,       1,  'fifo'),
        (INO_SOCKET,     12, 'socket'),
        (INO_SETUID,     8,  'setuid_prog'),
    ], FFS1_BSIZE)
    write_at(buf, FFS1_ROOT_FRAG * FFS1_FSIZE, root_dir)

    # subdir/
    subdir = build_dir_block([
        (INO_SUBDIR,  4, '.'),
        (INO_ROOT,    4, '..'),
        (INO_NESTED,  8, 'nested.txt'),
    ], FFS1_BSIZE)
    write_at(buf, FFS1_SUBDIR_FRAG * FFS1_FSIZE, subdir)

    # ── File data blocks ──────────────────────────────────────────────────────
    write_at(buf, FFS1_REGULAR_FRAG * FFS1_FSIZE, FILE_CONTENT_REGULAR)
    write_at(buf, FFS1_NESTED_FRAG  * FFS1_FSIZE, FILE_CONTENT_NESTED)
    write_at(buf, FFS1_LONGLN_FRAG  * FFS1_FSIZE, LONG_LINK_TARGET)
    write_at(buf, FFS1_SETUID_FRAG  * FFS1_FSIZE, FILE_CONTENT_SETUID)

    with open(path, 'wb') as f:
        f.write(buf)

    print(f"Created FFS1 test image: {path}  ({FFS1_IMAGE_SIZE} bytes)")
    print(f"  superblock  @ frag {FFS1_SBLOCK_FRAG}  (offset {FFS1_SBLOCK_FRAG*FFS1_FSIZE})")
    print(f"  inode table @ frag {FFS1_IBLKNO_FRAG}  (offset {FFS1_IBLKNO_FRAG*FFS1_FSIZE})")
    print(f"  root dir    @ frag {FFS1_ROOT_FRAG}  (offset {FFS1_ROOT_FRAG*FFS1_FSIZE})")
    print(f"  inline_link target (inline in ino {INO_INLINE_LNK}): {INLINE_LINK_TARGET.decode()}")
    print(f"  long_link   @ frag {FFS1_LONGLN_FRAG}  target len={len(LONG_LINK_TARGET)}")


# ─────────────────────────────────────────────────────────────────────────────
# FFS2 image builder
# ─────────────────────────────────────────────────────────────────────────────

FFS2_BSIZE  = 4096
FFS2_FSIZE  = 512
FFS2_FRAG   = FFS2_BSIZE // FFS2_FSIZE    # 8
FFS2_NCG    = 1
FFS2_IPG    = 64
FFS2_FPG    = 512    # fragments per CG  → image = 512 × 512 = 262144 bytes
FFS2_INOPB  = FFS2_BSIZE // 256           # 16  (ufs2_dinode = 256 bytes)
FFS2_NINDIR = FFS2_BSIZE // 8             # 512 (64-bit indirect pointers)

FFS2_SBLOCK_FRAG   = 65536 // FFS2_FSIZE  # 128 – superblock
FFS2_IBLKNO_FRAG   = 160                  # 160 – inode table start
FFS2_ROOT_FRAG     = 168                  # root dir data block
FFS2_SUBDIR_FRAG   = 176                  # subdir data block
FFS2_REGULAR_FRAG  = 184                  # regular.txt
FFS2_NESTED_FRAG   = 192                  # nested.txt
FFS2_LONGLN_FRAG   = 200                  # long_link target
FFS2_SETUID_FRAG   = 208                  # setuid_prog
FFS2_IMAGE_SIZE    = FFS2_FPG * FFS2_FSIZE  # 262144 bytes


def ffs2_build_superblock(now):
    sb = bytearray(1376)
    def w32(off, val):  struct.pack_into('<I', sb, off, val & 0xFFFFFFFF)
    def w32s(off, val): struct.pack_into('<i', sb, off, val)
    def w64(off, val):  struct.pack_into('<Q', sb, off, val & 0xFFFFFFFFFFFFFFFF)

    w32(8,   FFS2_SBLOCK_FRAG)
    w32(16,  FFS2_IBLKNO_FRAG)
    w32(24,  0)                             # fs_cgoffset (unused for FFS2)
    w32(28,  0xFFFFFFFF)                    # fs_cgmask   (unused for FFS2)
    w32(44,  FFS2_NCG)
    w32(48,  FFS2_BSIZE)
    w32(52,  FFS2_FSIZE)
    w32(56,  FFS2_FRAG)
    w32(60,  5)
    w32(96,  int(math.log2(FFS2_FRAG)))
    w32(100, int(math.log2(FFS2_FSIZE // 512)))
    w32(116, FFS2_NINDIR)
    w32(120, FFS2_INOPB)
    w32(184, FFS2_IPG)
    w32(188, FFS2_FPG)

    # UFS2 cylinder summary (offsets 1008 and 1024)
    nifree = FFS2_IPG * FFS2_NCG - (INO_SETUID + 1)
    w64(1008, 2)                            # fs_ufs2_cs_ndir
    w64(1024, max(0, nifree))               # fs_ufs2_cs_nifree
    w64(1080, FFS2_FPG * FFS2_NCG)          # fs_size (total fragments)

    sb[209] = 1                             # fs_clean

    volname = b'TestFFS2\x00'
    sb[680:680 + len(volname)] = volname
    w32(1372, UFS2_MAGIC)
    return bytes(sb)


def ffs2_build_dinode(mode, nlink, uid, gid, size, atime, mtime, ctime,
                      db=None, blocks_bytes=0, gen=1,
                      inline_data=None, rdev=None):
    """
    Build a UFS2 on-disk inode (256 bytes).

    inline_data  – bytes written into the di_db area (inline symlinks).
    rdev         – device number stored in di_db[0] (char/block devices).
    db           – list of fragment addresses for di_db[0..11].
    blocks_bytes – di_blocks value (bytes actually held; FFS2 uses bytes).
    """
    inode = bytearray(256)
    def w16(off, val):  struct.pack_into('<H', inode, off, val & 0xFFFF)
    def w32(off, val):  struct.pack_into('<I', inode, off, val & 0xFFFFFFFF)
    def w64(off, val):  struct.pack_into('<Q', inode, off, val & 0xFFFFFFFFFFFFFFFF)
    def w64s(off, val): struct.pack_into('<q', inode, off, val)

    w16(0,  mode)
    w16(2,  nlink)
    w32(4,  uid)
    w32(8,  gid)
    w32(12, FFS2_BSIZE)         # di_blksize
    w64(16, size)               # di_size
    w64(24, blocks_bytes)       # di_blocks (bytes)
    w64s(32, atime)             # di_atime
    w64s(40, mtime)             # di_mtime
    w64s(48, ctime)             # di_ctime
    w64s(56, ctime)             # di_birthtime
    w32(80, gen)

    # di_db[0..11] starts at offset 112 in the UFS2 inode.
    if inline_data is not None:
        inode[112:112 + len(inline_data)] = inline_data
    elif rdev is not None:
        w64(112, rdev)
    elif db:
        for i, frag in enumerate(db[:NDADDR]):
            w64(112 + i * 8, frag)

    return bytes(inode)


def create_ffs2_image(path):
    buf = bytearray(FFS2_IMAGE_SIZE)
    now = int(time.time())

    write_at(buf, FFS2_SBLOCK_FRAG * FFS2_FSIZE, ffs2_build_superblock(now))

    def put_inode(ino, inode_bytes):
        off = FFS2_IBLKNO_FRAG * FFS2_FSIZE + ino * 256
        write_at(buf, off, inode_bytes)

    # ino 2: root directory
    put_inode(INO_ROOT, ffs2_build_dinode(
        mode=0o040755, nlink=3, uid=0, gid=0,
        size=FFS2_BSIZE, atime=now, mtime=now, ctime=now,
        db=[FFS2_ROOT_FRAG], blocks_bytes=FFS2_BSIZE))

    # ino 3: regular.txt
    put_inode(INO_REGULAR, ffs2_build_dinode(
        mode=0o100644, nlink=2, uid=1000, gid=1000,
        size=len(FILE_CONTENT_REGULAR), atime=now, mtime=now, ctime=now,
        db=[FFS2_REGULAR_FRAG], blocks_bytes=FFS2_BSIZE))

    # ino 4: subdir
    put_inode(INO_SUBDIR, ffs2_build_dinode(
        mode=0o040755, nlink=2, uid=0, gid=0,
        size=FFS2_BSIZE, atime=now, mtime=now, ctime=now,
        db=[FFS2_SUBDIR_FRAG], blocks_bytes=FFS2_BSIZE))

    # ino 5: subdir/nested.txt
    put_inode(INO_NESTED, ffs2_build_dinode(
        mode=0o100600, nlink=1, uid=1000, gid=1000,
        size=len(FILE_CONTENT_NESTED), atime=now, mtime=now, ctime=now,
        db=[FFS2_NESTED_FRAG], blocks_bytes=FFS2_BSIZE))

    # ino 6: inline_link (short symlink, stored inline)
    put_inode(INO_INLINE_LNK, ffs2_build_dinode(
        mode=0o120777, nlink=1, uid=0, gid=0,
        size=len(INLINE_LINK_TARGET), atime=now, mtime=now, ctime=now,
        inline_data=INLINE_LINK_TARGET, blocks_bytes=0))

    # ino 7: long_link (long symlink, data block)
    put_inode(INO_LONG_LNK, ffs2_build_dinode(
        mode=0o120777, nlink=1, uid=0, gid=0,
        size=len(LONG_LINK_TARGET), atime=now, mtime=now, ctime=now,
        db=[FFS2_LONGLN_FRAG], blocks_bytes=FFS2_BSIZE))

    # ino 8: character device
    put_inode(INO_CHARDEV, ffs2_build_dinode(
        mode=0o020644, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        rdev=CHARDEV_RDEV, blocks_bytes=0))

    # ino 9: block device
    put_inode(INO_BLKDEV, ffs2_build_dinode(
        mode=0o060640, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        rdev=BLKDEV_RDEV, blocks_bytes=0))

    # ino 10: FIFO
    put_inode(INO_FIFO, ffs2_build_dinode(
        mode=0o010644, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        blocks_bytes=0))

    # ino 11: Unix socket
    put_inode(INO_SOCKET, ffs2_build_dinode(
        mode=0o140755, nlink=1, uid=0, gid=0,
        size=0, atime=now, mtime=now, ctime=now,
        blocks_bytes=0))

    # ino 12: setuid regular file
    put_inode(INO_SETUID, ffs2_build_dinode(
        mode=0o104755, nlink=1, uid=0, gid=0,
        size=len(FILE_CONTENT_SETUID), atime=now, mtime=now, ctime=now,
        db=[FFS2_SETUID_FRAG], blocks_bytes=FFS2_BSIZE))

    # ── Directory data blocks ─────────────────────────────────────────────────
    root_dir = build_dir_block([
        (INO_ROOT,       4,  '.'),
        (INO_ROOT,       4,  '..'),
        (INO_REGULAR,    8,  'regular.txt'),
        (INO_SUBDIR,     4,  'subdir'),
        (INO_INLINE_LNK, 10, 'inline_link'),
        (INO_LONG_LNK,   10, 'long_link'),
        (INO_REGULAR,    8,  'hardlink'),
        (INO_CHARDEV,    2,  'chardev'),
        (INO_BLKDEV,     6,  'blkdev'),
        (INO_FIFO,       1,  'fifo'),
        (INO_SOCKET,     12, 'socket'),
        (INO_SETUID,     8,  'setuid_prog'),
    ], FFS2_BSIZE)
    write_at(buf, FFS2_ROOT_FRAG * FFS2_FSIZE, root_dir)

    subdir = build_dir_block([
        (INO_SUBDIR,  4, '.'),
        (INO_ROOT,    4, '..'),
        (INO_NESTED,  8, 'nested.txt'),
    ], FFS2_BSIZE)
    write_at(buf, FFS2_SUBDIR_FRAG * FFS2_FSIZE, subdir)

    # ── File data blocks ──────────────────────────────────────────────────────
    write_at(buf, FFS2_REGULAR_FRAG * FFS2_FSIZE, FILE_CONTENT_REGULAR)
    write_at(buf, FFS2_NESTED_FRAG  * FFS2_FSIZE, FILE_CONTENT_NESTED)
    write_at(buf, FFS2_LONGLN_FRAG  * FFS2_FSIZE, LONG_LINK_TARGET)
    write_at(buf, FFS2_SETUID_FRAG  * FFS2_FSIZE, FILE_CONTENT_SETUID)

    with open(path, 'wb') as f:
        f.write(buf)

    print(f"Created FFS2 test image: {path}  ({FFS2_IMAGE_SIZE} bytes)")
    print(f"  superblock  @ frag {FFS2_SBLOCK_FRAG}  (offset {FFS2_SBLOCK_FRAG*FFS2_FSIZE})")
    print(f"  inode table @ frag {FFS2_IBLKNO_FRAG}  (offset {FFS2_IBLKNO_FRAG*FFS2_FSIZE})")
    print(f"  root dir    @ frag {FFS2_ROOT_FRAG}  (offset {FFS2_ROOT_FRAG*FFS2_FSIZE})")
    print(f"  inline_link target (inline in ino {INO_INLINE_LNK}): {INLINE_LINK_TARGET.decode()}")
    print(f"  long_link   @ frag {FFS2_LONGLN_FRAG}  target len={len(LONG_LINK_TARGET)}")


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('ffs1', 'ffs2'):
        print(f"Usage: {sys.argv[0]} ffs1|ffs2 <output-image>")
        sys.exit(1)
    version, out = sys.argv[1], sys.argv[2]
    if version == 'ffs1':
        create_ffs1_image(out)
    else:
        create_ffs2_image(out)
