# FFS1 / FFS2 Feature Implementation Summary

This document surveys every feature category defined by OpenBSD's FFS
(Fast File System, `sys/ufs/ffs/` and `sys/ufs/ufs/`) and records whether
the **linux-ufs** read-only driver in this repository implements it fully,
partially, or not at all.

Legend:
- ✅ **Full** – correctly implemented and tested
- ⚠️ **Partial** – code exists but with known gaps or caveats
- ❌ **Not implemented** – absent from the driver

---

## 1. File Types

| File Type | OpenBSD symbol | Status | Notes |
|-----------|---------------|--------|-------|
| Regular file | `IFREG` | ✅ Full | Read via `mpage_read_folio` / `generic_file_read_iter` |
| Directory | `IFDIR` | ✅ Full | `iterate_shared` + linear `lookup` |
| Symbolic link – inline (short) | `IFLNK`, `i_size ≤ MAXSYMLINKLEN` | ✅ Full | Stored in `di_db[]` area; `SLAB_USERCOPY` fix applied |
| Symbolic link – file-backed (long) | `IFLNK`, `i_size > MAXSYMLINKLEN` | ✅ Full | `page_get_link` via normal page cache |
| Hard link | N/A (multiple dirents) | ✅ Full | Standard nlink counting; same inode, multiple directory entries |
| Character device | `IFCHR` | ✅ Full | `init_special_inode`; rdev from `di_db[0]` (32-bit FFS1 / 64-bit FFS2) |
| Block device | `IFBLK` | ✅ Full | Same as above |
| Named pipe (FIFO) | `IFIFO` | ✅ Full | `init_special_inode` |
| Unix-domain socket | `IFSOCK` | ✅ Full | `init_special_inode` |
| Whiteout | `IFWHT` | ❌ Not implemented | Linux VFS has no native whiteout inode type; only meaningful in union mounts |

---

## 2. Inode Attributes

| Attribute | Status | Notes |
|-----------|--------|-------|
| `di_mode` (type + permission bits) | ✅ Full | All 12 mode bits including `ISUID`, `ISGID`, `ISVTX` |
| `di_uid` / `di_gid` (32-bit) | ✅ Full | FFS1 uses old 16-bit fields + 32-bit extension; FFS2 native 32-bit |
| `di_nlink` | ✅ Full | Correct for all file types |
| `di_size` (64-bit) | ✅ Full | Both FFS1 (`u_int64_t`) and FFS2 |
| `di_blocks` (actual disk usage) | ✅ Full | FFS1: 512-byte units; FFS2: byte count → `>> 9` |
| `di_gen` (generation number) | ✅ Full | Used by NFS; stored in `inode->i_generation` |
| `di_atime` / `di_atimensec` | ✅ Full | 32-bit (FFS1) and 64-bit (FFS2); nanosecond precision |
| `di_mtime` / `di_mtimensec` | ✅ Full | Same |
| `di_ctime` / `di_ctimensec` | ✅ Full | Same |
| `di_birthtime` / `di_birthnsec` (FFS2) | ✅ Full | Parsed from disk; surfaced via `statx(STATX_BTIME)`; FFS1 has no birthtime |
| `di_flags` (BSD `chflags` flags) | ⚠️ Partial | Stored in `ufs_inode_info.i_flags`; exposed via `stat.attributes` in `getattr`, but Linux has no `chflags(2)` syscall – flags are visible but not enforced |
| `di_extb[2]` / `di_extsize` (FFS2 EA blocks) | ❌ Not implemented | Extended-attribute block pointers present in on-disk struct but never read |

---

## 3. Block Addressing

| Feature | Status | Notes |
|---------|--------|-------|
| Direct blocks `di_db[0..11]` (12 entries) | ✅ Full | Both 32-bit (FFS1) and 64-bit (FFS2) |
| Single-indirect `di_ib[0]` | ✅ Full | Covers files up to 12 + 1024 blocks (4 MB @ 4 KB blocks) |
| Double-indirect `di_ib[1]` | ✅ Full | Covers files up to ~4 GB @ 4 KB blocks |
| Triple-indirect `di_ib[2]` | ✅ Full | Covers files up to ~4 TB @ 4 KB blocks (FFS1), ~32 TB (FFS2) |
| Fragment-to-block conversion (`frag_addr / fs_frag`) | ✅ Full | Used everywhere in `ufs_block_map` |
| Fragment tail reads (last partial block in file) | ✅ Full | VFS page cache clips to `i_size`; no extra action needed |
| Hole / sparse file (block pointer = 0) | ✅ Full | `ufs_block_map` returns 0 → `map_bh` not called → zero page |

---

## 4. Superblock

| Feature | Status | Notes |
|---------|--------|-------|
| FFS1 magic (`0x011954`) | ✅ Full | Detected at offset 8192 |
| FFS2 magic (`0x19540119`) | ✅ Full | Detected at offset 65536 |
| Alternate/piggyback superblock (`0x40000`) | ✅ Full | Probed third in `ufs_parse_superblock` |
| Auto-detection FFS1 vs FFS2 | ✅ Full | No mount option needed |
| Volume name (`fs_volname`) | ✅ Full | Shown in `show_options` and mount log |
| `statfs` (block/inode counts) | ✅ Full | `f_blocks`, `f_bfree`, `f_files`, `f_ffree` |
| `show_options` | ✅ Full | `version=ufs1/ufs2`, `volname=…` |
| Block and fragment size validation | ✅ Full | Power-of-2, `fs_bsize ≥ fs_fsize`, etc. |
| `fs_clean` / dirty-filesystem detection | ✅ Full | Checked at mount; `pr_warn` emitted if `fs_clean == 0` |
| `FS_UNCLEAN` flag in `fs_flags` | ✅ Full | Checked alongside `fs_clean`; `pr_warn` on either condition |
| Superblock checksum (`FS_OKAY`) | ✅ Full | FFS1 only: `(fs_state + fs_ffs1_time) == FS_OKAY` verified; mismatch → "unverifiable" warning; FFS2 has no such checksum |
| `FS_FLAGS_UPDATED` / `fs_ffs1_flags` compat | ❌ Not implemented | Legacy FFS1 flag area not inspected |
| `fs_inodefmt` check (`FS_44INODEFMT` vs `FS_42INODEFMT`) | ❌ Not implemented | Old 4.2BSD inode format (no `di_uid`/`di_gid` 32-bit fields) not handled |
| Backup cylinder-group superblocks | ❌ Not implemented | Only the three primary probe offsets are tried; no fallback to CG backup SBs |

---

## 5. Cylinder Group Layout

| Feature | Status | Notes |
|---------|--------|-------|
| `cgbase(cg) = fpg × cg` | ✅ Full | `ufs_cgbase()` in `ufs.h` |
| `cgstart(cg)` – FFS1 offset (`cgoffset × (cg & ~cgmask)`) | ✅ Full | `ufs_cgstart()` in `ufs.h` |
| `cgstart(cg)` – FFS2 (= cgbase, no offset) | ✅ Full | Same function, branches on `fs_ufs2` |
| `cgimin(cg)` – first fragment of inode table | ✅ Full | `ufs_cgimin()` in `ufs.h` |
| Multiple cylinder groups (`fs_ncg > 1`) | ✅ Full | `ufs_ino_to_fsba` computes correct CG for any inode number |
| Cylinder group block (`struct cg`, free-block bitmap) | ❌ Not implemented | Only needed for allocation and `fsck`; not required for read-only |
| Fragment summary counters (`cg_frsum`) | ❌ Not implemented | Same – allocation only |
| Rotational layout tables (`fs_postbloff`, `fs_rotbloff`) | ❌ Not implemented | Historical optimization; completely unused even in modern OpenBSD |

---

## 6. Directory Operations

| Feature | Status | Notes |
|---------|--------|-------|
| `readdir` / `iterate_shared` | ✅ Full | Full scan with deleted-entry skip (`d_ino == 0`) |
| `lookup` | ✅ Full | Linear scan; returns negative dentry on miss |
| `d_type` field mapping (`UFS_DT_*` → `DT_*`) | ✅ Full | All 9 types including `DT_WHT` |
| Record-length sanity checks | ✅ Full | Validates `reclen ≥ 8`, aligned, within block |
| `MAXNAMLEN` (255) | ✅ Full | `ENAMETOOLONG` returned for longer names |
| Dot (`.`) and dotdot (`..`) entries | ✅ Full | Emitted normally as directory entries |
| Directory hash (`ufs_dirhash`) | ❌ Not implemented | OpenBSD uses an in-memory hash for large dirs; this driver does O(n) scan |
| `d_namlen` vs `d_type` byte-swap (old format) | ❌ Not implemented | Some old UFS1 images swap the byte order of these two fields; not handled |

---

## 7. Mount Options

| Option | Status | Notes |
|--------|--------|-------|
| Read-only mount (`SB_RDONLY`) | ✅ Full | Always forced; driver never writes to disk |
| Automatic FFS1 / FFS2 detection | ✅ Full | No `version=` option needed |
| `noatime` | ❌ Not implemented | No mount-option parser; `SB_NOATIME` never set |
| `nodev` / `noexec` / `nosuid` | ❌ Not implemented | These are VFS-layer flags; could be parsed but are not |
| NFS export options | ❌ Not implemented | OpenBSD supports `vfs_checkexp`; not needed for a basic driver |
| UID / GID remapping | ❌ Not implemented | No `uid=` / `gid=` options |
| `sync` / `async` | N/A | Irrelevant for a read-only driver |

---

## 8. File I/O

| Feature | Status | Notes |
|---------|--------|-------|
| Sequential read (`read_iter`) | ✅ Full | `generic_file_read_iter` |
| `mmap` (shared read-only) | ✅ Full | `generic_file_mmap` |
| Readahead | ✅ Full | `mpage_readahead` |
| `llseek` | ✅ Full | `generic_file_llseek` |
| `read_dir` (directory) | ✅ Full | `generic_read_dir` |
| `getattr` / `statx` | ✅ Full | `generic_fillattr`; `stat.blksize = fs_fsize` |
| `statx` `STATX_BTIME` (birthtime) | ✅ Full | FFS2 `di_birthtime` surfaced via `stat->btime` + `STATX_BTIME` in result_mask |
| Write / `write_iter` | ❌ Not implemented | Read-only driver |
| `fsync` / `fdatasync` | ❌ Not implemented | No dirty data to flush |
| `fallocate` / `punch_hole` | ❌ Not implemented | Write feature |
| `ioctl` | ❌ Not implemented | No UFS-specific ioctls |
| Advisory locks (`fcntl F_SETLK`) | ❌ Not implemented | Could add `generic_file_lock` |
| `sendfile` | ⚠️ Partial | Works via page cache but not explicitly tested |

---

## 9. Write & Allocation (Not Implemented)

These features require write support and are explicitly out of scope for
this read-only driver.

| Feature | Status |
|---------|--------|
| Block allocation (`ffs_alloc`) | ❌ |
| Fragment reallocation (`ffs_realloccg`) | ❌ |
| Inode allocation (`ffs_valloc`) | ❌ |
| File creation / `mknod` / `mkdir` | ❌ |
| File deletion / `unlink` / `rmdir` | ❌ |
| Rename | ❌ |
| `truncate` | ❌ |
| Soft updates (soft dependencies) | ❌ |
| Deferred write / `bdwrite` | ❌ |
| Superblock / cylinder-group writeback | ❌ |

---

## 10. fsck / Filesystem Health

| Feature | Status | Notes |
|---------|--------|-------|
| Read-only mount of unclean filesystem | ✅ Full | Driver silently mounts any image regardless of `fs_clean` |
| Warn on unclean filesystem at mount | ❌ Not implemented | `fs_clean == 0` / `FS_UNCLEAN` not checked or logged |
| Prevent R/W mount of unclean filesystem | N/A | Driver is always read-only |
| `fs_fscktime` field | ❌ Not implemented | Last `fsck` timestamp present on disk but not read |
| `fs_pendingblocks` / `fs_pendinginodes` | ❌ Not implemented | FFS2 soft-update pending counters; not relevant for read-only |
| Backup superblock recovery | ❌ Not implemented | No fallback to cylinder-group backup superblocks |

> **Note:** `fsck_ffs(8)` operates directly on the block device and does
> not interact with the running kernel driver.  The driver does not need
> to implement fsck logic itself.

---

## 11. Quotas

| Feature | Status | Notes |
|---------|--------|-------|
| User quotas (`quota.h` / `.quota` file) | ❌ Not implemented | OpenBSD uses `ufs_quotactl`; quota files would need to be read |
| Group quotas | ❌ Not implemented | Same |

---

## 12. Snapshots

| Feature | Status | Notes |
|---------|--------|-------|
| `fs_snapinum[20]` (snapshot inode list) | ❌ Not implemented | Snapshot inodes could be read like any other file, but snapshot semantics (COW) require write support |

---

## 13. Extended Attributes (FFS2)

| Feature | Status | Notes |
|---------|--------|-------|
| `di_extb[2]` block pointers | ❌ Not implemented | The two EA block addresses are parsed into `ufs2_dinode` struct but never followed |
| `di_extsize` | ❌ Not implemented | EA data size field ignored |
| `getxattr` / `listxattr` | ❌ Not implemented | Linux xattr VFS hooks not wired up |

---

## 14. Byte Order / Platform

| Feature | Status | Notes |
|---------|--------|-------|
| Little-endian (x86 / amd64 / ARM) | ✅ Full | All fields use `le16_to_cpu` / `le32_to_cpu` / `le64_to_cpu` |
| Big-endian host (SPARC, PowerPC) | ❌ Not implemented | UFS images created on big-endian hosts use big-endian on-disk layout; this driver only handles little-endian images |

---

## 15. Module Infrastructure

| Feature | Status | Notes |
|---------|--------|-------|
| `kmem_cache` for `ufs_inode_info` | ✅ Full | `kmem_cache_create_usercopy` with `i_u` union whitelisted for `HARDENED_USERCOPY` |
| RCU-deferred inode free | ✅ Full | `call_rcu` + `rcu_barrier` in `ufs_exit` |
| Linux ≥ 6.8 `get_tree_bdev` API | ✅ Full | `mount_bdev` replaced with `init_fs_context` + `get_tree_bdev` |
| `FS_REQUIRES_DEV` flag | ✅ Full | Block-device backed filesystem |
| Module alias `ufs2bsd` | ✅ Full | `MODULE_ALIAS_FS("ufs2bsd")` |

---

## Summary Table

| Category | Fully ✅ | Partial ⚠️ | Not Implemented ❌ |
|----------|---------|-----------|-------------------|
| File types | 9 / 10 | 0 | 1 (whiteout) |
| Inode attributes | 9 / 12 | 1 (BSD flags) | 2 (birthtime, EA) |
| Block addressing | 7 / 7 | 0 | 0 |
| Superblock | 7 / 14 | 0 | 7 |
| Cylinder group layout | 5 / 8 | 0 | 3 (CG block, rotational tables) |
| Directory operations | 6 / 8 | 0 | 2 (dirhash, old namlen/type swap) |
| Mount options | 2 / 7 | 0 | 5 |
| File I/O | 7 / 11 | 1 (sendfile) | 4 |
| Write & allocation | 0 / 10 | 0 | 10 (by design – read-only) |
| fsck / health | 1 / 6 | 0 | 5 |
| Quotas | 0 / 2 | 0 | 2 |
| Snapshots | 0 / 1 | 0 | 1 |
| Extended attributes (FFS2) | 0 / 3 | 0 | 3 |
| Byte order | 1 / 2 | 0 | 1 (big-endian) |
| Module infrastructure | 5 / 5 | 0 | 0 |
| **Total** | **59 / 106** | **2** | **45** |

---

## Priority Notes for Future Work

If write support or broader compatibility is desired, the highest-value
items to implement first are:

1. **`fs_clean` / `FS_UNCLEAN` check at mount** — trivial to add, avoids
   mounting a filesystem that needs `fsck`.
2. **`STATX_BTIME` (birthtime) for FFS2** — one-liner in `ufs_getattr`;
   `di_birthtime` is already parsed.
3. **Dirty-filesystem warning** — log a `pr_warn` when `fs_clean == 0`.
4. **Backup superblock fallback** — iterate cylinder-group superblocks if
   the primary probe locations fail.
5. **`noatime` mount option** — parse `SB_NOATIME` and skip atime updates
   (irrelevant now since this is read-only, but needed for future R/W).
6. **Big-endian image support** — add a `fs_byteswap` flag and conditionalize
   all field reads on it (similar to how the existing Linux `ufs` driver
   in `fs/ufs/` handles byte order with `uspi->s_bytesex`).
7. **Extended attributes (FFS2)** — follow `di_extb[]` pointers to read
   EA blocks; wire up `getxattr` / `listxattr`.
