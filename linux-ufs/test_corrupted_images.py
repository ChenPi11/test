#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
test_corrupted_images.py – Unit tests for corrupted FFS1/FFS2 images.

Verifies that the linux_ufs driver (via ufs_reader userspace tool) correctly:
  • Rejects (exit 1) images with FATAL superblock corruption:
      - Wrong magic bytes
      - Zero or non-power-of-2 fs_bsize
      - fs_bsize < fs_fsize
      - Zero fs_frag / fs_ncg / fs_ipg
      - All-zeros image (no filesystem)
      - Truncated image (too short to hold a superblock)
  • Warns (exit 0, stdout contains "WARN:") for RECOVERABLE conditions:
      - FFS1: invalid fs_state checksum (clean-state cannot be trusted)
      - FFS1: fs_clean == 0 (filesystem was not cleanly unmounted)
      - FFS2: fs_clean == 0
      - FFS2: FS_UNCLEAN flag set in fs_flags

This test does NOT require root privileges.  It runs ufs_reader in
--superblock-only mode so no inode/directory structures are needed.

Usage:
    python3 test_corrupted_images.py [-v]
    python3 -m pytest linux-ufs/test_corrupted_images.py -v
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

# ─── Paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
READER_SRC  = os.path.join(SCRIPT_DIR, 'ufs_reader.c')
READER_BIN  = os.path.join(SCRIPT_DIR, 'ufs_reader')

# ─── On-disk constants (must match ufs_fs.h / super.c) ───────────────────────
UFS1_MAGIC  = 0x011954
UFS2_MAGIC  = 0x19540119
FS_OKAY     = 0x7c269d38   # FFS1 superblock checksum constant
FS_UNCLEAN  = 0x0001       # fs_flags bit: not cleanly unmounted

SBLOCK_UFS1 = 8192         # byte offset of FFS1 superblock
SBLOCK_UFS2 = 65536        # byte offset of FFS2 superblock
SB_SIZE     = 1376         # superblock is always 1376 bytes

# Offsets WITHIN the 1376-byte raw superblock
SB_OFF_IBLKNO   =   16     # fs_iblkno       (uint32)
SB_OFF_CGOFFSET =   24     # fs_cgoffset      (uint32)
SB_OFF_CGMASK   =   28     # fs_cgmask        (uint32)
SB_OFF_FFS1TIME =   32     # fs_ffs1_time     (uint32, FFS1 only)
SB_OFF_FFS1SIZE =   36     # fs_ffs1_size     (uint32, FFS1 only)
SB_OFF_NCG      =   44     # fs_ncg           (uint32)
SB_OFF_BSIZE    =   48     # fs_bsize         (uint32)
SB_OFF_FSIZE    =   52     # fs_fsize         (uint32)
SB_OFF_FRAG     =   56     # fs_frag          (uint32)
SB_OFF_NINDIR   =  116     # fs_nindir        (uint32)
SB_OFF_INOPB    =  120     # fs_inopb         (uint32)
SB_OFF_IPG      =  184     # fs_ipg           (uint32)
SB_OFF_FPG      =  188     # fs_fpg           (uint32)
SB_OFF_CLEAN    =  209     # fs_clean         (uint8)
SB_OFF_FLAGS    = 1308     # fs_flags         (uint32)
SB_OFF_STATE    = 1352     # fs_state         (uint32, FFS1 checksum)
SB_OFF_MAGIC    = 1372     # fs_magic         (uint32)
SB_OFF_FFS2SIZE = 1080     # fs_size (UFS2)   (int64) – total fragment count

# ─── Default geometry for test superblocks ────────────────────────────────────
# FFS1 (matches create_test_image.py)
FFS1_BSIZE   = 4096
FFS1_FSIZE   = 512
FFS1_FRAG    = FFS1_BSIZE // FFS1_FSIZE   # 8
FFS1_NCG     = 1
FFS1_IPG     = 64
FFS1_FPG     = 128
FFS1_INOPB   = FFS1_BSIZE // 128          # 32
FFS1_NINDIR  = FFS1_BSIZE // 4            # 1024
FFS1_IBLKNO  = 64

# FFS2
FFS2_BSIZE   = 32768
FFS2_FSIZE   = 4096
FFS2_FRAG    = FFS2_BSIZE // FFS2_FSIZE   # 8
FFS2_NCG     = 1
FFS2_IPG     = 64
FFS2_FPG     = 64
FFS2_INOPB   = FFS2_BSIZE // 256          # 128
FFS2_NINDIR  = FFS2_BSIZE // 8            # 4096

# Image sizes (large enough to hold superblock + some padding)
FFS1_IMG_SIZE  = max(SBLOCK_UFS1 + SB_SIZE + 4096, 65536)
FFS2_IMG_SIZE  = SBLOCK_UFS2 + SB_SIZE + 65536

# ─── System page size (matches kernel PAGE_SIZE used in super.c) ──────────────
# This determines which fsize values are accepted vs. rejected by the driver.
# Typical values: 4096 (x86_64), 16384 (ARM64 Debian kernel), 65536 (some ARM64).
LINUX_PAGE_SIZE = os.sysconf('SC_PAGE_SIZE')


# ─── Low-level helpers ────────────────────────────────────────────────────────

def _u32(v: int) -> bytes:
    return struct.pack('<I', v & 0xFFFFFFFF)


def _u8(v: int) -> bytes:
    return bytes([v & 0xFF])


def _set32(buf: bytearray, off: int, v: int) -> None:
    buf[off:off + 4] = _u32(v)


def _set8(buf: bytearray, off: int, v: int) -> None:
    buf[off] = v & 0xFF


# ─── Minimal superblock builders ──────────────────────────────────────────────

def _make_ffs1_sb(now: int = 0,
                  magic:   int = UFS1_MAGIC,
                  bsize:   int = FFS1_BSIZE,
                  fsize:   int = FFS1_FSIZE,
                  frag:    int = FFS1_FRAG,
                  ncg:     int = FFS1_NCG,
                  ipg:     int = FFS1_IPG,
                  fpg:     int = FFS1_FPG,
                  inopb:   int = FFS1_INOPB,
                  nindir:  int = FFS1_NINDIR,
                  clean:   int = 0x01,
                  fs_state: int | None = None,
                  fs_flags: int = 0) -> bytes:
    """Build a 1376-byte FFS1 superblock with the given parameters."""
    if now == 0:
        now = int(time.time()) & 0xFFFFFFFF
    if fs_state is None:
        fs_state = (FS_OKAY - now) & 0xFFFFFFFF

    sb = bytearray(SB_SIZE)
    _set32(sb, SB_OFF_IBLKNO,   FFS1_IBLKNO)
    _set32(sb, SB_OFF_CGOFFSET, 0)
    _set32(sb, SB_OFF_CGMASK,   0xFFFFFFFF)
    _set32(sb, SB_OFF_FFS1TIME, now)
    _set32(sb, SB_OFF_FFS1SIZE, fpg * ncg)
    _set32(sb, SB_OFF_NCG,      ncg)
    _set32(sb, SB_OFF_BSIZE,    bsize)
    _set32(sb, SB_OFF_FSIZE,    fsize)
    _set32(sb, SB_OFF_FRAG,     frag)
    _set32(sb, SB_OFF_NINDIR,   nindir)
    _set32(sb, SB_OFF_INOPB,    inopb)
    _set32(sb, SB_OFF_IPG,      ipg)
    _set32(sb, SB_OFF_FPG,      fpg)
    _set8(sb,  SB_OFF_CLEAN,    clean)
    _set32(sb, SB_OFF_STATE,    fs_state)
    _set32(sb, SB_OFF_FLAGS,    fs_flags)
    _set32(sb, SB_OFF_MAGIC,    magic)
    return bytes(sb)


def _make_ffs2_sb(magic:  int = UFS2_MAGIC,
                  bsize:  int = FFS2_BSIZE,
                  fsize:  int = FFS2_FSIZE,
                  frag:   int = FFS2_FRAG,
                  ncg:    int = FFS2_NCG,
                  ipg:    int = FFS2_IPG,
                  fpg:    int = FFS2_FPG,
                  inopb:  int = FFS2_INOPB,
                  nindir: int = FFS2_NINDIR,
                  clean:  int = 0x01,
                  fs_flags: int = 0) -> bytes:
    """Build a 1376-byte FFS2 superblock with the given parameters."""
    sb = bytearray(SB_SIZE)
    _set32(sb, SB_OFF_NCG,    ncg)
    _set32(sb, SB_OFF_BSIZE,  bsize)
    _set32(sb, SB_OFF_FSIZE,  fsize)
    _set32(sb, SB_OFF_FRAG,   frag)
    _set32(sb, SB_OFF_NINDIR, nindir)
    _set32(sb, SB_OFF_INOPB,  inopb)
    _set32(sb, SB_OFF_IPG,    ipg)
    _set32(sb, SB_OFF_FPG,    fpg)
    _set8(sb,  SB_OFF_CLEAN,  clean)
    _set32(sb, SB_OFF_FLAGS,  fs_flags)
    _set32(sb, SB_OFF_MAGIC,  magic)
    # UFS2: total fragment count at offset 1080 (int64)
    struct.pack_into('<q', sb, SB_OFF_FFS2SIZE, fpg * ncg)
    return bytes(sb)


def _wrap_ffs1(sb_bytes: bytes) -> bytes:
    """Embed sb_bytes at SBLOCK_UFS1 in a zero-filled FFS1 image."""
    img = bytearray(FFS1_IMG_SIZE)
    img[SBLOCK_UFS1:SBLOCK_UFS1 + SB_SIZE] = sb_bytes
    return bytes(img)


def _wrap_ffs2(sb_bytes: bytes) -> bytes:
    """Embed sb_bytes at SBLOCK_UFS2 in a zero-filled FFS2 image."""
    img = bytearray(FFS2_IMG_SIZE)
    img[SBLOCK_UFS2:SBLOCK_UFS2 + SB_SIZE] = sb_bytes
    return bytes(img)


def _write_tmp(data: bytes) -> str:
    """Write data to a temporary file; caller must delete it."""
    fd, path = tempfile.mkstemp(suffix='.img', prefix='ufs_corrupt_')
    os.write(fd, data)
    os.close(fd)
    return path


# ─── ufs_reader runner ────────────────────────────────────────────────────────

def _ensure_reader_built() -> None:
    """Compile ufs_reader if the binary is missing or stale."""
    needs_build = (
        not os.path.exists(READER_BIN) or
        os.path.getmtime(READER_BIN) < os.path.getmtime(READER_SRC)
    )
    if needs_build:
        result = subprocess.run(
            ['gcc', '-O2', '-o', READER_BIN, READER_SRC],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f'Failed to build ufs_reader:\n{result.stderr}'
            )


def _run_reader(img_path: str) -> tuple[int, str, str]:
    """
    Run ``ufs_reader <img_path> --superblock-only``.
    Returns (returncode, stdout, stderr).
    """
    result = subprocess.run(
        [READER_BIN, img_path, '--superblock-only'],
        capture_output=True, text=True,
    )
    return result.returncode, result.stdout, result.stderr


# ─── Base test class ──────────────────────────────────────────────────────────

class _Base(unittest.TestCase):
    """Base class: builds ufs_reader once per test run."""

    @classmethod
    def setUpClass(cls) -> None:
        _ensure_reader_built()

    def _check_fatal(self, img_data: bytes, expected_msg: str,
                     label: str = '') -> None:
        """Assert ufs_reader exits non-zero and stderr contains expected_msg."""
        path = _write_tmp(img_data)
        try:
            rc, out, err = _run_reader(path)
            combined = out + err
            self.assertNotEqual(
                rc, 0,
                msg=f'{label}: expected non-zero exit but got 0\n'
                    f'stdout={out!r}\nstderr={err!r}',
            )
            self.assertIn(
                expected_msg, combined,
                msg=f'{label}: expected {expected_msg!r} in output\n'
                    f'stdout={out!r}\nstderr={err!r}',
            )
        finally:
            os.unlink(path)

    def _check_warn(self, img_data: bytes, expected_warn: str,
                    label: str = '') -> None:
        """Assert ufs_reader exits 0 and stdout contains a WARN: line."""
        path = _write_tmp(img_data)
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(
                rc, 0,
                msg=f'{label}: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}',
            )
            self.assertIn(
                'WARN:', out,
                msg=f'{label}: expected "WARN:" in stdout\n'
                    f'stdout={out!r}\nstderr={err!r}',
            )
            self.assertIn(
                expected_warn, out,
                msg=f'{label}: expected {expected_warn!r} in stdout\n'
                    f'stdout={out!r}',
            )
        finally:
            os.unlink(path)


# ─── Fatal-corruption tests ───────────────────────────────────────────────────

class TestFatalCorruption(_Base):
    """
    Images that must cause ufs_reader to exit non-zero.
    These mirror the checks in super.c that return -EINVAL and refuse to mount.
    """

    # ── Wrong magic ──────────────────────────────────────────────────────────

    def test_ffs1_bad_magic(self):
        """FFS1 image with zeroed magic → 'not a UFS filesystem'."""
        sb = _make_ffs1_sb(magic=0x00000000)
        self._check_fatal(_wrap_ffs1(sb), 'not a UFS filesystem',
                          label='ffs1_bad_magic')

    def test_ffs2_bad_magic(self):
        """FFS2 image with zeroed magic → 'not a UFS filesystem'."""
        sb = _make_ffs2_sb(magic=0x00000000)
        self._check_fatal(_wrap_ffs2(sb), 'not a UFS filesystem',
                          label='ffs2_bad_magic')

    def test_ffs1_random_magic(self):
        """FFS1 image with garbage magic value → 'not a UFS filesystem'."""
        sb = _make_ffs1_sb(magic=0xDEADBEEF)
        self._check_fatal(_wrap_ffs1(sb), 'not a UFS filesystem',
                          label='ffs1_random_magic')

    # ── Zero fs_bsize ────────────────────────────────────────────────────────

    def test_ffs1_zero_bsize(self):
        """FFS1 with fs_bsize=0 → sanity check failure."""
        sb = _make_ffs1_sb(bsize=0)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_zero_bsize')

    # ── Non-power-of-2 fs_bsize ──────────────────────────────────────────────

    def test_ffs1_nonpow2_bsize(self):
        """FFS1 with fs_bsize=3000 (not a power of 2) → sanity check failure."""
        sb = _make_ffs1_sb(bsize=3000)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_nonpow2_bsize')

    def test_ffs2_nonpow2_bsize(self):
        """FFS2 with fs_bsize=12000 (not a power of 2) → sanity check failure."""
        sb = _make_ffs2_sb(bsize=12000)
        self._check_fatal(_wrap_ffs2(sb), 'sanity check failed',
                          label='ffs2_nonpow2_bsize')

    # ── fs_bsize < fs_fsize ──────────────────────────────────────────────────

    def test_ffs1_bsize_lt_fsize(self):
        """FFS1 with bsize=512, fsize=4096 (bsize < fsize) → sanity check failure."""
        sb = _make_ffs1_sb(bsize=512, fsize=4096, frag=1)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_bsize_lt_fsize')

    # ── Zero geometry fields ─────────────────────────────────────────────────

    def test_ffs1_zero_frag(self):
        """FFS1 with fs_frag=0 → sanity check failure."""
        sb = _make_ffs1_sb(frag=0)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_zero_frag')

    def test_ffs1_zero_ncg(self):
        """FFS1 with fs_ncg=0 → sanity check failure."""
        sb = _make_ffs1_sb(ncg=0)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_zero_ncg')

    def test_ffs1_zero_ipg(self):
        """FFS1 with fs_ipg=0 → sanity check failure."""
        sb = _make_ffs1_sb(ipg=0)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_zero_ipg')

    def test_ffs2_zero_inopb(self):
        """FFS2 with fs_inopb=0 → sanity check failure."""
        sb = _make_ffs2_sb(inopb=0)
        self._check_fatal(_wrap_ffs2(sb), 'sanity check failed',
                          label='ffs2_zero_inopb')

    # ── Degenerate images ────────────────────────────────────────────────────

    def test_all_zeros(self):
        """Completely zeroed image → 'not a UFS filesystem'."""
        self._check_fatal(bytes(FFS2_IMG_SIZE), 'not a UFS filesystem',
                          label='all_zeros')

    def test_truncated_image(self):
        """Image of only 100 bytes → cannot read superblock → failure."""
        self._check_fatal(bytes(100), 'not a UFS filesystem',
                          label='truncated')

    def test_all_0xff(self):
        """Image filled with 0xFF bytes (no valid magic) → failure."""
        self._check_fatal(bytes([0xFF] * FFS1_IMG_SIZE), 'not a UFS filesystem',
                          label='all_0xff')


# ─── Warning-only tests ───────────────────────────────────────────────────────

class TestWarningConditions(_Base):
    """
    Images with 'soft' problems that the kernel driver mounts with a warning
    but does not refuse.  ufs_reader must exit 0 and print a WARN: line.
    These mirror the pr_warn() calls in super.c ufs_parse_superblock().
    """

    # ── FFS1 superblock checksum ─────────────────────────────────────────────

    def test_ffs1_bad_fs_state(self):
        """
        FFS1 with wrong fs_state checksum.
        (fs_state + fs_ffs1_time) != FS_OKAY → WARN: checksum invalid.
        The filesystem can still be read; the operator should run fsck_ffs.
        """
        now = int(time.time()) & 0xFFFFFFFF
        bad_state = (FS_OKAY - now + 1) & 0xFFFFFFFF  # off by one
        sb = _make_ffs1_sb(now=now, fs_state=bad_state, clean=1)
        self._check_warn(_wrap_ffs1(sb), 'checksum',
                         label='ffs1_bad_fs_state')

    def test_ffs1_zero_fs_state(self):
        """FFS1 with fs_state=0 (e.g., written by tool that ignores checksum)."""
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, fs_state=0, clean=1)
        self._check_warn(_wrap_ffs1(sb), 'checksum',
                         label='ffs1_zero_fs_state')

    # ── Dirty filesystem flag (FFS1) ─────────────────────────────────────────

    def test_ffs1_dirty_fs(self):
        """
        FFS1 with fs_clean=0 (unclean unmount).
        Kernel mounts read-only with a warning; ufs_reader must do likewise.
        """
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, clean=0x00)  # fs_clean = 0
        self._check_warn(_wrap_ffs1(sb), 'cleanly',
                         label='ffs1_dirty_fs')

    def test_ffs1_dirty_fs_both_warn_msgs(self):
        """FFS1 with both bad checksum AND fs_clean=0 → two WARN: lines."""
        now = int(time.time()) & 0xFFFFFFFF
        bad_state = (FS_OKAY - now + 0x100) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, fs_state=bad_state, clean=0x00)
        self._check_warn(_wrap_ffs1(sb), 'checksum',
                         label='ffs1_bad_checksum_and_dirty')
        self._check_warn(_wrap_ffs1(sb), 'cleanly',
                         label='ffs1_bad_checksum_and_dirty')

    # ── Dirty filesystem flag (FFS2) ─────────────────────────────────────────

    def test_ffs2_dirty_fs(self):
        """FFS2 with fs_clean=0 → WARN: filesystem not cleanly unmounted."""
        sb = _make_ffs2_sb(clean=0x00)
        self._check_warn(_wrap_ffs2(sb), 'cleanly',
                         label='ffs2_dirty_fs')

    def test_ffs2_unclean_flag(self):
        """FFS2 with FS_UNCLEAN bit set in fs_flags → WARN: not cleanly unmounted."""
        sb = _make_ffs2_sb(clean=0x01, fs_flags=FS_UNCLEAN)
        self._check_warn(_wrap_ffs2(sb), 'cleanly',
                         label='ffs2_unclean_flag')

    def test_ffs2_dirty_and_unclean_flag(self):
        """FFS2 with both fs_clean=0 AND FS_UNCLEAN → single WARN: line."""
        sb = _make_ffs2_sb(clean=0x00, fs_flags=FS_UNCLEAN)
        self._check_warn(_wrap_ffs2(sb), 'cleanly',
                         label='ffs2_dirty_and_unclean_flag')


# ─── Valid-baseline sanity tests ──────────────────────────────────────────────

class TestValidBaselines(_Base):
    """
    Confirm that the valid test images succeed (exit 0, no WARN lines).
    These act as a sanity-check for the test infrastructure itself.
    """

    def _check_clean(self, img_data: bytes, label: str = '') -> None:
        """Assert ufs_reader exits 0 and prints no WARN: lines."""
        path = _write_tmp(img_data)
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(
                rc, 0,
                msg=f'{label}: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}',
            )
            self.assertNotIn(
                'WARN:', out,
                msg=f'{label}: unexpected WARN in output:\n{out}',
            )
            self.assertIn(
                'Found superblock', out,
                msg=f'{label}: expected "Found superblock" in output\n{out}',
            )
        finally:
            os.unlink(path)

    def test_valid_ffs1(self):
        """A well-formed FFS1 superblock with correct checksum → exit 0, no WARN."""
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now)
        self._check_clean(_wrap_ffs1(sb), label='valid_ffs1')

    def test_valid_ffs2(self):
        """A well-formed FFS2 superblock with clean status → exit 0, no WARN."""
        sb = _make_ffs2_sb()
        self._check_clean(_wrap_ffs2(sb), label='valid_ffs2')

    def test_valid_ffs1_with_volname(self):
        """FFS1 with a volume name still passes without warnings."""
        now = int(time.time()) & 0xFFFFFFFF
        sb_bytes = bytearray(_make_ffs1_sb(now=now))
        volname = b'TestVolume\x00'
        sb_bytes[680:680 + len(volname)] = volname
        self._check_clean(_wrap_ffs1(bytes(sb_bytes)), label='valid_ffs1_volname')


# ─── Block-size tests ─────────────────────────────────────────────────────────

class TestBlockSize(_Base):
    """
    Tests for block-size and fragment-size related checks.

    The Linux UFS driver enforces two constraints that BSD does not:
      1. fs_fsize must be <= PAGE_SIZE  (fragments must fit in a page)
      2. fs_bsize may exceed PAGE_SIZE; the driver uses
         fs_io_bsize = min(fs_bsize, PAGE_SIZE) and adjusts block-
         mapping arithmetic accordingly.

    These tests use the actual system PAGE_SIZE (LINUX_PAGE_SIZE) so they
    are correct regardless of architecture (4 KiB, 16 KiB, 64 KiB, …).

    Concurrent I/O analysis
    ───────────────────────
    The min(fs_bsize, PAGE_SIZE) approach does NOT introduce concurrent-I/O
    issues:

    • sbi->fs_io_bsize is computed once in ufs_fill_super() and is read-only
      for the lifetime of the mount.  No synchronisation needed.

    • ufs_block_map() is a stateless computation over the inode's read-only
      block-pointer array (ui->i_u) plus calls to ufs_read_indir() which
      each do an independent sb_bread().

    • sb_bread() uses Linux's buffer-cache locking (BH_Lock) to serialise
      concurrent reads of the same physical block.  Multiple concurrent
      callers for the same or different blocks are handled correctly without
      any additional locking in the UFS driver.

    • When fs_io_bsize < fs_bsize, a single FFS block maps to
      (fs_bsize / fs_io_bsize) consecutive I/O blocks.  Two threads reading
      different pages of the same FFS block will call sb_bread() for
      *different* block numbers; the buffer-cache handles each independently.

    Conclusion: the implementation is concurrent-safe.
    """

    # ── Helpers ──────────────────────────────────────────────────────────────

    @staticmethod
    def _ffs1_large_fsize_params():
        """Return (fsize, bsize, frag, nindir, inopb) for fsize = PAGE_SIZE*2."""
        fsize  = LINUX_PAGE_SIZE * 2          # exceeds page size → rejected
        bsize  = fsize * 8                    # frag = 8, always valid ratio
        frag   = bsize // fsize               # = 8
        nindir = bsize // 4                   # UFS1 uses 32-bit pointers
        inopb  = bsize // 128                 # UFS1 dinode is 128 bytes
        return fsize, bsize, frag, nindir, inopb

    @staticmethod
    def _ffs2_large_fsize_params():
        """Return (fsize, bsize, frag, nindir, inopb) for fsize = PAGE_SIZE*2."""
        fsize  = LINUX_PAGE_SIZE * 2
        bsize  = fsize * 8
        frag   = bsize // fsize               # = 8
        nindir = bsize // 8                   # UFS2 uses 64-bit pointers
        inopb  = bsize // 256                 # UFS2 dinode is 256 bytes
        return fsize, bsize, frag, nindir, inopb

    @staticmethod
    def _ffs1_large_bsize_params():
        """Return (bsize, fsize, frag, nindir, inopb) for bsize = PAGE_SIZE*4."""
        bsize  = LINUX_PAGE_SIZE * 4          # exceeds page size → uses io_bsize
        fsize  = 512                          # always <= PAGE_SIZE; valid
        frag   = bsize // fsize
        nindir = bsize // 4
        inopb  = bsize // 128
        return bsize, fsize, frag, nindir, inopb

    @staticmethod
    def _ffs2_large_bsize_params():
        """Return (bsize, fsize, frag, nindir, inopb) for bsize = PAGE_SIZE*4."""
        bsize  = LINUX_PAGE_SIZE * 4
        fsize  = max(512, LINUX_PAGE_SIZE // 4)  # <= PAGE_SIZE; power-of-2
        frag   = bsize // fsize
        nindir = bsize // 8
        inopb  = bsize // 256
        return bsize, fsize, frag, nindir, inopb

    # ── fs_fsize > PAGE_SIZE → fatal ─────────────────────────────────────────

    def test_ffs1_fsize_gt_page_size(self):
        """
        FFS1 with fs_fsize = PAGE_SIZE*2 → 'fragment size ... exceeds page size'.
        Mirrors super.c: sbi->fs_fsize > PAGE_SIZE → -EINVAL.
        """
        fsize, bsize, frag, nindir, inopb = self._ffs1_large_fsize_params()
        sb = _make_ffs1_sb(bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS1 + SB_SIZE + bsize * 2, 65536)
        img = bytearray(img_size)
        img[SBLOCK_UFS1:SBLOCK_UFS1 + SB_SIZE] = sb
        self._check_fatal(bytes(img), 'fragment size',
                          label='ffs1_fsize_gt_page_size')

    def test_ffs2_fsize_gt_page_size(self):
        """
        FFS2 with fs_fsize = PAGE_SIZE*2 → 'fragment size ... exceeds page size'.
        """
        fsize, bsize, frag, nindir, inopb = self._ffs2_large_fsize_params()
        sb = _make_ffs2_sb(bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS2 + SB_SIZE + bsize * 2, 131072)
        img = bytearray(img_size)
        img[SBLOCK_UFS2:SBLOCK_UFS2 + SB_SIZE] = sb
        self._check_fatal(bytes(img), 'fragment size',
                          label='ffs2_fsize_gt_page_size')

    # ── fs_fsize not a power of 2 → fatal sanity check ───────────────────────

    def test_ffs1_fsize_nonpow2(self):
        """
        FFS1 with fs_fsize=3000 (not power of 2) → sanity check failed.
        Mirrors IS_POW2(sb->fsize) check in ufs_reader / is_power_of_2 in super.c.
        """
        sb = _make_ffs1_sb(bsize=8192, fsize=3000, frag=2)
        self._check_fatal(_wrap_ffs1(sb), 'sanity check failed',
                          label='ffs1_fsize_nonpow2')

    def test_ffs2_fsize_nonpow2(self):
        """FFS2 with fs_fsize=6000 (not power of 2) → sanity check failed."""
        sb = _make_ffs2_sb(bsize=32768, fsize=6000, frag=5)
        self._check_fatal(_wrap_ffs2(sb), 'sanity check failed',
                          label='ffs2_fsize_nonpow2')

    # ── fs_bsize > PAGE_SIZE with fs_fsize <= PAGE_SIZE → valid ──────────────

    def test_ffs1_large_bsize_valid(self):
        """
        FFS1 with fs_bsize = PAGE_SIZE*4, fs_fsize = 512.
        The driver uses io_bsize = min(bsize, PAGE_SIZE) = PAGE_SIZE.
        Must succeed with no errors or warnings.
        """
        bsize, fsize, frag, nindir, inopb = self._ffs1_large_bsize_params()
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS1 + SB_SIZE + bsize, 131072)
        img = bytearray(img_size)
        img[SBLOCK_UFS1:SBLOCK_UFS1 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs1_large_bsize_valid: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertNotIn('ERROR', err,
                msg=f'ffs1_large_bsize_valid: unexpected ERROR\n{err}')
            self.assertIn('Found superblock', out,
                msg=f'ffs1_large_bsize_valid: expected "Found superblock"\n{out}')
        finally:
            os.unlink(path)

    def test_ffs2_large_bsize_valid(self):
        """
        FFS2 with fs_bsize = PAGE_SIZE*4, fs_fsize <= PAGE_SIZE.
        Must succeed: driver uses io_bsize = min(bsize, PAGE_SIZE).
        """
        bsize, fsize, frag, nindir, inopb = self._ffs2_large_bsize_params()
        sb = _make_ffs2_sb(bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS2 + SB_SIZE + bsize, 131072)
        img = bytearray(img_size)
        img[SBLOCK_UFS2:SBLOCK_UFS2 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs2_large_bsize_valid: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertNotIn('ERROR', err,
                msg=f'ffs2_large_bsize_valid: unexpected ERROR\n{err}')
            self.assertIn('Found superblock', out,
                msg=f'ffs2_large_bsize_valid: expected "Found superblock"\n{out}')
        finally:
            os.unlink(path)

    # ── fs_bsize = PAGE_SIZE (exact boundary) → valid ────────────────────────

    def test_ffs1_bsize_equals_page_size(self):
        """
        FFS1 with fs_bsize exactly equal to PAGE_SIZE.
        io_bsize = min(PAGE_SIZE, PAGE_SIZE) = PAGE_SIZE → standard path.
        """
        bsize  = LINUX_PAGE_SIZE
        fsize  = max(512, LINUX_PAGE_SIZE // 8)
        frag   = bsize // fsize
        nindir = bsize // 4
        inopb  = bsize // 128
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS1 + SB_SIZE + bsize, 65536)
        img = bytearray(img_size)
        img[SBLOCK_UFS1:SBLOCK_UFS1 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs1_bsize_eq_page: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertIn('Found superblock', out)
        finally:
            os.unlink(path)

    def test_ffs2_bsize_equals_page_size(self):
        """FFS2 with fs_bsize exactly equal to PAGE_SIZE → valid."""
        bsize  = LINUX_PAGE_SIZE
        fsize  = max(512, LINUX_PAGE_SIZE // 8)
        frag   = bsize // fsize
        nindir = bsize // 8
        inopb  = bsize // 256
        if inopb == 0:
            self.skipTest('PAGE_SIZE too small for UFS2 (inopb would be 0)')
        sb = _make_ffs2_sb(bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS2 + SB_SIZE + bsize, 131072)
        img = bytearray(img_size)
        img[SBLOCK_UFS2:SBLOCK_UFS2 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs2_bsize_eq_page: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertIn('Found superblock', out)
        finally:
            os.unlink(path)

    # ── fs_fsize = PAGE_SIZE (exact boundary) → valid ────────────────────────

    def test_ffs1_fsize_equals_page_size(self):
        """
        FFS1 with fs_fsize exactly equal to PAGE_SIZE.
        fs_fsize == PAGE_SIZE is the largest accepted fragment size.
        """
        fsize  = LINUX_PAGE_SIZE
        bsize  = fsize * 8
        frag   = 8
        nindir = bsize // 4
        inopb  = bsize // 128
        now = int(time.time()) & 0xFFFFFFFF
        sb = _make_ffs1_sb(now=now, bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS1 + SB_SIZE + bsize, 65536)
        img = bytearray(img_size)
        img[SBLOCK_UFS1:SBLOCK_UFS1 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs1_fsize_eq_page: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertIn('Found superblock', out)
        finally:
            os.unlink(path)

    def test_ffs2_fsize_equals_page_size(self):
        """FFS2 with fs_fsize exactly equal to PAGE_SIZE → valid."""
        fsize  = LINUX_PAGE_SIZE
        bsize  = fsize * 8
        frag   = 8
        nindir = bsize // 8
        inopb  = bsize // 256
        sb = _make_ffs2_sb(bsize=bsize, fsize=fsize, frag=frag,
                           nindir=nindir, inopb=inopb)
        img_size = max(SBLOCK_UFS2 + SB_SIZE + bsize, 131072)
        img = bytearray(img_size)
        img[SBLOCK_UFS2:SBLOCK_UFS2 + SB_SIZE] = sb
        path = _write_tmp(bytes(img))
        try:
            rc, out, err = _run_reader(path)
            self.assertEqual(rc, 0,
                msg=f'ffs2_fsize_eq_page: expected exit 0 but got {rc}\n'
                    f'stdout={out!r}\nstderr={err!r}')
            self.assertIn('Found superblock', out)
        finally:
            os.unlink(path)


# ─── Entry point ─────────────────────────────────────────────────────────────

if __name__ == '__main__':
    unittest.main(verbosity=2)
