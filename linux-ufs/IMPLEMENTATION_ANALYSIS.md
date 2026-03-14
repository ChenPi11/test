# Feature Implementation Analysis

This document analyses every **not-implemented (❌) or partial (⚠️) feature**
listed in `FEATURES.md` and evaluates:

- **Code Impact** – how invasively the feature would modify the existing driver
  source (~2000 lines across `super.c`, `inode.c`, `dir.c`, `file.c`, `ufs.h`,
  `ufs_fs.h`)
- **Stability Risk** – the probability that the implementation introduces kernel
  crashes, data corruption, or regressions
- **Effort** – approximate lines-of-change (LoC) and person-days
- **Necessity** – whether normal read-only use cases require it

Effort scale: **XS** < 1 day · **S** 1–2 days · **M** 3–7 days · **L** 1–3 weeks · **XL** > 3 weeks

Stability-risk scale: **Low** · **Medium** · **High** (kernel-level risks only; user-visible bugs are "Medium" at worst for a read-only driver)

---

## 1. File Types

### 1-A  Whiteout (`IFWHT`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Linux VFS has no `IFWHT` inode type. The only place it appears is `dir.c`: a whiteout entry currently gets `d_type = DT_WHT` in `readdir` (already done) but cannot be returned as a valid inode from `lookup`. Adding "safe ignore" behaviour (return `ENOENT` from lookup, or expose as an invisible entry) requires ~10 lines in `dir.c`. |
| **Stability risk** | **Low** – the change is entirely in lookup path; no memory allocations or block I/O. |
| **Effort** | **XS** (~10 LoC in `dir.c`) |
| **Necessary?** | **No.** Whiteout entries only appear on union-mounted filesystems (OpenBSD `mount_union`). A standard FFS partition from OpenBSD/FreeBSD/NetBSD will never contain `IFWHT` inodes. Skipping is safe. |

---

## 2. Inode Attributes

### 2-A  `di_birthtime` / `di_birthnsec` surfaced via `statx(STATX_BTIME)`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | `di_birthtime` is **already read** into `ufs_inode_info.i_birthtime` / `i_birthtime_nsec` (FFS2 only). The only missing step is setting `stat->btime` and `stat->result_mask |= STATX_BTIME` in `ufs_getattr()`. ~5 lines in `inode.c`. |
| **Stability risk** | **Low** – pure attribute copy, no I/O, no allocation. |
| **Effort** | **XS** (~5 LoC) |
| **Necessary?** | **Recommended.** It is the only FFS2-specific metadata advantage over FFS1. Any tool doing archive integrity checks (rsync `--checksum`, `cp -a`, backup software) may rely on birthtime. |

### 2-B  BSD `chflags` flags (`di_flags`) enforcement
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Flags are already stored in `ufs_inode_info.i_flags`. Enforcement would require intercepting `inode_permission()` or `security_inode_permission()` to honour `SF_IMMUTABLE`, `UF_NOUNLINK`, etc. This requires a custom `permission` inode operation and understanding of which flag maps to which Linux concept (`S_IMMUTABLE`, `S_APPEND`, `S_NODUMP`). ~80–120 LoC in `inode.c` + new logic in `ufs.h`. |
| **Stability risk** | **Medium** – a bug in `permission()` can make files unexpectedly inaccessible (DoS on the mount). Must be tested carefully for edge cases (root bypass, `CAP_LINUX_IMMUTABLE`). |
| **Effort** | **S** (~100 LoC, 1–2 days) |
| **Necessary?** | **Optional.** For pure data-recovery / migration use, silently ignoring chflags is fine. Only necessary if the driver is used as a security boundary (e.g., mounting untrusted removable media). |

### 2-C  Extended attributes (`di_extb[2]` / `di_extsize` / FFS2 EA blocks)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Medium-sized new subsystem: a new `ea.c` (~200 LoC) to read EA blocks (at most 2 direct block pointers), parse the `extattr` record format (`extattr_header` + namespace byte + name + value), and implement the Linux `xattr` interface (`get_inode_acl`, `generic_getxattr`, `listxattr`). Requires adding `inode_operations.get_inode_acl` and `.listxattr` to all file type vnop tables in `inode.c`. |
| **Stability risk** | **Medium** – new block I/O path; a bug in EA-block parsing (offset arithmetic, record size) can trigger out-of-bounds reads. Must validate `di_extsize` against `2 × fs_bsize` limit. |
| **Effort** | **M** (~300 LoC, 4–5 days) |
| **Necessary?** | **Optional for most users.** FreeBSD uses EA blocks heavily for ACLs and MAC labels; OpenBSD does not use them by default. Only relevant if mounting FreeBSD FFS2 partitions that carry NFSv4 ACLs or MAC labels. |

---

## 3. Block Addressing — fully implemented; no gaps.

---

## 4. Superblock

### 4-A  `fs_clean` / `FS_UNCLEAN` warning at mount
> **✅ IMPLEMENTED** – `ufs_parse_superblock()` in `super.c` emits `pr_warn()` whenever `fs_clean == 0` or `fs_flags & FS_UNCLEAN` is set (FFS1 path: only after the `fs_state` checksum passes; FFS2 path: direct check).

| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | ~8 LoC in `ufs_parse_superblock()`. Zero architectural change. |
| **Stability risk** | **Low** – read-only check; no new I/O or allocations. |
| **Effort** | **XS** (~8 LoC) |
| **Necessary?** | **Strongly recommended.** ✓ Done. |

### 4-B  `FS_UNCLEAN` flag in `fs_flags`
> **✅ IMPLEMENTED** – Covered by 4-A.

| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Included in the same 8-line check as 4-A. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (included in 4-A) |
| **Necessary?** | ✓ Done. |

### 4-C  Superblock checksum (`fs_state` / `FS_OKAY`)
> **✅ IMPLEMENTED** – FFS1: `ufs_parse_superblock()` verifies `(fs_state + fs_ffs1_time) == FS_OKAY` before trusting `fs_clean`. `FS_OKAY = 0x7c269d38`, `FS_ISCLEAN`, `FS_WASCLEAN` added to `ufs_fs.h`. FFS2 has no `fs_state` guard; direct check used instead. Test images (`create_ufs1.py`, `create_test_image.py`) now write a valid `fs_state` checksum.

| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | ~40 LoC in `super.c` + 3 new constants in `ufs_fs.h` + 2-line fix in each Python test-image generator. |
| **Stability risk** | **Low** – adds a mount-time guard; no runtime path changes. |
| **Effort** | **XS** (~45 LoC total) |
| **Necessary?** | **Recommended.** ✓ Done. |

### 4-D  `FS_FLAGS_UPDATED` / `fs_ffs1_flags` compat
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | `FS_FLAGS_UPDATED` is a flag indicating that `fs_flags` (rather than the old `fs_ffs1_flags` alias) should be trusted. Handling it requires ~20 lines in `ufs_parse_superblock()` to pick the right flags field. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~20 LoC) |
| **Necessary?** | **Low priority.** Only relevant for very old FFS1 images written before the flags area was standardised (pre-2000 era). Modern `newfs` always sets `FS_FLAGS_UPDATED`. |

### 4-E  `fs_inodefmt` check (`FS_44INODEFMT` vs `FS_42INODEFMT`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | The old 4.2BSD inode format has no `di_uid`/`di_gid` 32-bit extension fields — uid/gid are packed differently. Supporting it requires a parallel inode-read path in `inode.c` (`ufs_iget`), adding ~40 lines of conditionals. |
| **Stability risk** | **Low** – the new branch is only entered for ancient images; no impact on modern FFS1/FFS2. |
| **Effort** | **S** (~50 LoC, 0.5 days) |
| **Necessary?** | **Very low priority.** 4.2BSD FFS images are pre-1993. Any modern OpenBSD `newfs(8)` produces `FS_44INODEFMT`. Practically zero real-world relevance. |

### 4-F  Backup cylinder-group superblock fallback
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | If all three primary probe offsets (8192, 65536, 262144) fail, iterate through CG superblocks at `fsbtodb(fs, cgsblock(fs, n))` for each `n`. This requires a two-pass approach: first attempt primary probes, then try CG backups using a heuristic block-size guess. ~80–120 LoC in `super.c`. |
| **Stability risk** | **Medium** – the fallback heuristic could read the wrong block and misinterpret its contents as a valid superblock (false-positive magic). Must be gated on strict validation of all superblock fields. |
| **Effort** | **S** (~100 LoC, 1 day) |
| **Necessary?** | **Recommended** for a production-grade driver. Primarily useful for data recovery scenarios where the primary superblock is corrupted. |

---

## 5. Cylinder Group Layout

### 5-A  Cylinder group block (`struct cg`, free-block bitmap)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | CG blocks are only needed for block/inode allocation and `statfs` free-count accuracy. `statfs` already uses `fs_cstotal` (a pre-summed counter in the superblock), so no CG reads are needed for correct `statfs`. Full CG support would require defining `struct cg` in `ufs_fs.h` (~80 lines) and reading them during allocation. |
| **Stability risk** | **Low** for read-only; **High** if write allocation is ever added (CG locking is complex). |
| **Effort** | **L** (needed only as part of write support; standalone: **XS** to parse, **XL** to use correctly for allocation) |
| **Necessary?** | **No** for read-only. `statfs` is already correct. Only needed if write support is added. |

### 5-B  Fragment summary counters (`cg_frsum`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Same as 5-A — allocation only. |
| **Stability risk** | **N/A** for read-only. |
| **Effort** | Included in write-support effort. |
| **Necessary?** | **No** for read-only. |

### 5-C  Rotational layout tables (`fs_postbloff`, `fs_rotbloff`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | These are hints for disk-rotation-aware block placement that were removed from modern OpenBSD `newfs` around 2000. No kernel driver (not even OpenBSD's current `ffs_balloc`) uses them in practice. |
| **Stability risk** | **N/A** |
| **Effort** | **XS** if at all |
| **Necessary?** | **No.** Historical artefact; even OpenBSD ignores them. |

---

## 6. Directory Operations

### 6-A  Directory hash (`ufs_dirhash`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | OpenBSD's `dirhash` maintains an in-memory hash table mapping filename→offset for directories above a size threshold (`ufs_mindirhashsize`). Adding it requires a new `dirhash.c` (~500 LoC), a pointer in `ufs_inode_info`, and integration into `dir.c` `lookup` and `readdir`. |
| **Stability risk** | **Medium** – in-memory hash allocation on every large-directory open; must handle memory-pressure correctly with `shrinker`. A hash corruption bug could silently miss files. |
| **Effort** | **L** (~500 LoC + shrinker, ~1 week) |
| **Necessary?** | **No** for correctness. Only a performance feature for directories with thousands of entries. The current O(n) scan is functionally correct. |

### 6-B  Old-format `d_namlen`/`d_type` byte-swap
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Some old SunOS/Solaris UFS images and very old BSD images used a big-endian directory format even on little-endian machines, swapping `d_namlen` and `d_type`. Detecting and handling this requires a flag in `ufs_sb_info` plus ~15 lines of conditional swapping in `dir.c`. The detection heuristic (check `d_type` range after initial entry read) is straightforward. |
| **Stability risk** | **Low** – only affects the direction of a byte swap; a misdetection produces garbage filenames (visible immediately), not a crash. |
| **Effort** | **XS** (~20 LoC) |
| **Necessary?** | **Low priority.** Not applicable to any modern OpenBSD/FreeBSD/NetBSD filesystem. Relevant only for very old UFS1 images from SunOS. |

---

## 7. Mount Options

### 7-A  `noatime`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | ~30 LoC: add `fs_context_operations.parse_param()`, parse `noatime` option, set/clear `SB_NOATIME` on `sb->s_flags`. For a read-only driver this has zero functional effect (atime is never updated anyway), but it is good practice for forward compatibility. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~30 LoC) |
| **Necessary?** | **No** for read-only; useful if write support is added later. |

### 7-B  `nodev` / `noexec` / `nosuid`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | These are pure VFS-layer flags (`SB_NODEV`, `SB_NOEXEC`, `SB_NOSUID`) that the kernel VFS already handles if set on `sb->s_flags`. The driver only needs to parse them from mount options (~20 LoC added to `parse_param`). |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~20 LoC, can be combined with 7-A) |
| **Necessary?** | **Optional.** Useful for security-sensitive deployments (mounting untrusted removable media). Users can already achieve this via `mount -o noexec,nosuid` at the VFS level in most cases. |

### 7-C  NFS export support
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Requires implementing `export_operations` with at least `fh_to_dentry`, `fh_to_parent`, `get_parent`, and `get_name`. These call `iget_locked`/`ufs_iget` by inode number, which the driver already supports. ~120 LoC in a new `export.c`. |
| **Stability risk** | **Medium** – NFS export paths involve more complex VFS interactions (filehandle revalidation, `d_splice_alias`, inode resurrection after eviction). A bug here can cause NFS stale-filehandle errors or inode double-frees. |
| **Effort** | **M** (~150 LoC, 2–3 days) |
| **Necessary?** | **Optional.** Required only if the user wants to re-export a BSD FFS volume over NFS from a Linux server. |

### 7-D  UID / GID remapping (`uid=` / `gid=` options)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Store remapping offsets in `ufs_sb_info`; apply them in `ufs_iget` when filling `inode->i_uid` / `inode->i_gid`. ~40 LoC. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~40 LoC) |
| **Necessary?** | **Optional.** Useful when mounting an OpenBSD disk whose uid/gid numbering differs from the Linux host. |

---

## 8. File I/O

### 8-A  `statx` `STATX_BTIME` (covered in 2-A above)

### 8-B  Write / `write_iter`, `fsync`, `fallocate`, `truncate`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Requires a complete new write subsystem: block allocation (`ffs_alloc`), fragment reallocation (`ffs_realloccg`), inode allocation (`ffs_valloc`), directory entry insertion/removal, superblock/CG writeback, and transaction ordering (or at least crash-safe ordering). This is essentially a full filesystem driver rewrite — an estimated **10–20× the size of the current codebase**. |
| **Stability risk** | **High** – write bugs in a kernel filesystem driver can corrupt user data silently. Requires an extensive test suite, fsck verification after every write test. |
| **Effort** | **XL** (several person-months, 3000–8000 new LoC) |
| **Necessary?** | **Only if the goal is a fully writable FFS driver.** For read-only data access or migration, this is not needed. Note that Linux already ships `fs/ufs/` which has partial write support for older UFS1; a better strategy would be to extend that driver rather than starting from scratch here. |

### 8-C  `ioctl`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | No standard UFS ioctls are documented for Linux. The only relevant one would be a "get filesystem UUID / label" ioctl, which is already served by `show_options`. ~10 LoC stub returning `ENOTTY`. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~10 LoC) |
| **Necessary?** | **No.** |

### 8-D  Advisory locks (`fcntl F_SETLK` / `flock`)
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | When `file_operations.lock` and `.flock` are both `NULL`, `vfs_lock_file()` in the VFS automatically falls through to `posix_lock_file()`, providing both POSIX record-lock (`fcntl F_SETLK`) and BSD `flock(2)` semantics. No code change is needed; the VFS default is already correct. The `generic_file_lock` helper that used to wrap this was removed from modern kernels (≥ 6.8) precisely because leaving these fields NULL achieves the same result. |
| **Stability risk** | **Low** — no driver code involved. |
| **Effort** | **XS** (0 LoC — already handled by VFS default) |
| **Necessary?** | **Already working.** Confirmed by VFS `vfs_lock_file()` fallback path. |

---

## 10. fsck / Filesystem Health

### 10-A  Warn on unclean filesystem (covered in 4-A)

### 10-B  `fs_fscktime` field
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Read `fs_fscktime` (a 4-byte timestamp in the superblock) and include it in a mount-time log message alongside the `fs_clean` warning. ~5 LoC addition to the 4-A check. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~5 LoC) |
| **Necessary?** | **Optional** – useful for diagnostics only. |

### 10-C  `fs_pendingblocks` / `fs_pendinginodes`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | These FFS2 fields are updated by the kernel during soft-update write operations. For a read-only driver they are informational only. Could be logged at mount or included in `statfs` output. ~10 LoC. |
| **Stability risk** | **Low** |
| **Effort** | **XS** (~10 LoC) |
| **Necessary?** | **No.** Only meaningful when the driver performs soft-update writes. |

### 10-D  Backup superblock recovery (covered in 4-F)

---

## 11. Quotas

### 11-A / 11-B  User and group quotas
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Quota files (`.quota.user`, `.quota.group`) are regular files on the FFS partition. Implementing `quotactl` requires: (1) locating and reading quota files, (2) implementing `vfs_quota_op` callbacks, (3) wiring up `sb->dq_op` and `sb->s_qcop`. The Linux quota subsystem provides common infrastructure but still requires ~300 LoC of FFS-specific glue. |
| **Stability risk** | **Medium** – quota accounting involves inode hooks (`dquot_transfer`) that run in the write path; for read-only they are mostly no-ops but their initialisation must not fail. |
| **Effort** | **M** (~300 LoC, 3–5 days for read-only quota reporting) |
| **Necessary?** | **No.** Quotas are never enforced on a read-only mount; the quota files can be read as plain files if needed. |

---

## 12. Snapshots

### 12-A  Snapshot inode support
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | Snapshot inodes (listed in `fs_snapinum[]`) are regular files whose data represents a point-in-time copy of the filesystem. A read-only driver could expose them as regular files without implementing COW semantics — ~30 LoC to iterate `fs_snapinum` and mark those inodes. Full snapshot semantics (consistent reads across COW) require write support. |
| **Stability risk** | **Low** for read-only exposure; **High** for full COW implementation |
| **Effort** | **XS** for expose-as-file (~30 LoC); **XL** for full COW |
| **Necessary?** | **No.** Snapshots are an advanced feature not needed for basic data access. |

---

## 13. Extended Attributes (FFS2)

### 13-A / 13-B / 13-C  EA block pointers, `di_extsize`, `getxattr`/`listxattr`
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | See 2-C above — a new `ea.c` (~300 LoC) plus hooks in `inode.c` vnop tables. |
| **Stability risk** | **Medium** |
| **Effort** | **M** (~300 LoC, 4–5 days) |
| **Necessary?** | **Optional** – needed only for FreeBSD FFS2 images carrying ACLs/MAC labels. |

---

## 14. Byte Order / Platform

### 14-A  Big-endian image support
| Dimension | Assessment |
|-----------|-----------|
| **Code impact** | UFS big-endian on-disk layout is used by SPARC and PowerPC BSDs. The existing `fs/ufs/` driver in the Linux kernel tree handles this via a `uspi->s_bytesex` flag and accessor macros (`SCLASS(bytesex, field)` etc.). Porting that approach to this driver requires: (1) a `fs_byteswap` flag in `ufs_sb_info`, (2) replacing every `le32_to_cpu`/`le64_to_cpu` call with conditional byte-swap accessors — affecting roughly 60–80 call sites across all source files. |
| **Stability risk** | **Medium** – a missed swap site causes silent misreads (wrong block numbers, corrupted timestamps). Must test on a big-endian image. |
| **Effort** | **M** (~60–80 call-site changes + accessor infrastructure, 3–4 days) |
| **Necessary?** | **Low priority.** The vast majority of FFS images in the wild today are from amd64/arm64 OpenBSD or FreeBSD installations (little-endian). SPARC/PowerPC BSDs are niche. If needed, it is better to point users at the in-tree `fs/ufs/` driver which already handles both byte orders. |

---

## Master Summary Table

Each row represents one gap.  
**Effort**: XS < 1 day · S 1–2 days · M 3–7 days · L 1–3 weeks · XL > 3 weeks  
**Stability risk**: Low / Medium / High  
**Recommendation**: ★★★ Must-have · ★★☆ Recommended · ★☆☆ Optional · ☆☆☆ Not needed

| # | Feature | Category | Effort | Stability Risk | Code Impact Summary | Recommendation |
|---|---------|----------|--------|---------------|---------------------|---------------|
| 1 | `fs_clean` / `FS_UNCLEAN` warning at mount | Superblock | **XS** (~8 LoC) | Low | Single check in `ufs_fill_super()` | ★★★ |
| 2 | `statx STATX_BTIME` (FFS2 birthtime) | Inode attr | **XS** (~5 LoC) | Low | 2 lines in `ufs_getattr()` | ★★★ |
| 3 | `flock` / advisory locks | File I/O | **XS** (0 LoC – VFS default) | Low | VFS `vfs_lock_file()` falls through to `posix_lock_file()` when `.lock = NULL` | ★★★ Already done |
| 4 | `fs_state` superblock checksum (FFS1) | Superblock | **XS** (~15 LoC) | Low | 15 lines in `ufs_parse_superblock()` | ★★☆ |
| 5 | Backup cylinder-group superblocks | Superblock | **S** (~100 LoC) | Medium | New fallback loop in `super.c` | ★★☆ |
| 6 | `noatime`, `nodev`, `noexec`, `nosuid` options | Mount opts | **XS** (~50 LoC) | Low | `parse_param` callback + flag mapping | ★★☆ |
| 7 | BSD `chflags` flag enforcement | Inode attr | **S** (~100 LoC) | Medium | Custom `permission()` iop in `inode.c` | ★☆☆ |
| 8 | `FS_FLAGS_UPDATED` / `fs_ffs1_flags` compat | Superblock | **XS** (~20 LoC) | Low | Conditional flag-field selection | ★☆☆ |
| 9 | UID / GID remapping options | Mount opts | **XS** (~40 LoC) | Low | `parse_param` + offset in `ufs_sb_info` | ★☆☆ |
| 10 | NFS export support | Mount opts | **M** (~150 LoC) | Medium | New `export.c`, `export_operations` | ★☆☆ |
| 11 | Extended attributes / `getxattr` (FFS2) | EA | **M** (~300 LoC) | Medium | New `ea.c` + hooks in `inode.c` | ★☆☆ |
| 12 | Big-endian image support | Byte order | **M** (~60–80 sites) | Medium | Accessor macro refactor throughout | ★☆☆ |
| 13 | Old `d_namlen`/`d_type` byte-swap | Directory | **XS** (~20 LoC) | Low | Detect + conditional swap in `dir.c` | ★☆☆ |
| 14 | `fs_inodefmt` 4.2BSD compat | Superblock | **S** (~50 LoC) | Low | Parallel inode-read branch in `inode.c` | ★☆☆ |
| 15 | `fs_fscktime` logging | fsck/health | **XS** (~5 LoC) | Low | Log field in mount-time warning | ★☆☆ |
| 16 | `fs_pendingblocks`/`fs_pendinginodes` (FFS2) | fsck/health | **XS** (~10 LoC) | Low | Informational log at mount | ☆☆☆ |
| 17 | Whiteout (`IFWHT`) safe ignore | File types | **XS** (~10 LoC) | Low | Return `ENOENT` from lookup | ☆☆☆ |
| 18 | Directory hash (`ufs_dirhash`) | Directory | **L** (~500 LoC) | Medium | New `dirhash.c` + shrinker | ☆☆☆ |
| 19 | CG block / fragment bitmap | CG layout | **L** (write path) | High | Only for allocation; N/A read-only | ☆☆☆ |
| 20 | Rotational layout tables | CG layout | N/A | N/A | Historical no-op | ☆☆☆ |
| 21 | Snapshot inode exposure | Snapshots | **XS** (~30 LoC) | Low | Iterate `fs_snapinum[]` | ☆☆☆ |
| 22 | Quota `quotactl` / `dquot` | Quotas | **M** (~300 LoC) | Medium | `dquot` init + `s_qcop` | ☆☆☆ |
| 23 | `ioctl` stub | File I/O | **XS** (~10 LoC) | Low | Return `ENOTTY` | ☆☆☆ |
| 24 | **Write support** (full) | Write | **XL** (3–8 k LoC) | **High** | New subsystem; entire new write path | ☆☆☆ |

---

## Recommended Implementation Order (read-only driver)

The following three items provide the highest value per line of code and
should be implemented next:

### Tier 1 — Trivial, high value (do now, ~30 LoC total)

1. **`fs_clean` / `FS_UNCLEAN` warning** (#1) — prevents silent mount of
   dirty filesystem; standard practice in all Linux filesystem drivers.
2. **`STATX_BTIME` birthtime** (#2) — FFS2-only metadata; already parsed,
   just needs to be surfaced.
3. **`flock` advisory lock** (#3) — one line; prevents application errors.

### Tier 2 — Worthwhile, moderate effort (1–2 days, ~200 LoC total)

4. **`fs_state` superblock checksum** (#4) — validates FFS1 image integrity.
5. **Mount options** (#6, `noatime`/`nodev`/`noexec`/`nosuid`) — useful for
   security-conscious deployments.
6. **Backup superblock fallback** (#5) — important for data recovery.

### Tier 3 — Situational (implement only if needed)

7. **Big-endian support** (#12) — if SPARC/PowerPC images must be read.
8. **Extended attributes** (#11) — if FreeBSD FFS2 images with ACLs must
   be accessed.
9. **NFS export** (#10) — if Linux must re-export BSD FFS over NFS.

### Not recommended (read-only scope)

- **Write support** (#24) — Use the in-tree `fs/ufs/` driver or consider
  FUSE-based implementations instead.
- **Directory hash** (#18) — Performance only; not a correctness issue.
- **Quotas** (#22), **Snapshots** (#21) — Complex; zero benefit for
  read-only data access.
