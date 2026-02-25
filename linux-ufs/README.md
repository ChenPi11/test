# Linux UFS Driver (`linux_ufs.ko`)

A **read-only** Linux kernel module that mounts **UFS1 and UFS2 (BSD Fast File System)** disk images as created by OpenBSD, FreeBSD, and NetBSD `newfs(8)`.

Inspired by the OpenBSD source at [`sys/ufs/`](https://github.com/openbsd/src/tree/master/sys/ufs).

---

## Files

| File | Description |
|------|-------------|
| `ufs_fs.h`       | On-disk format: superblock, UFS1/UFS2 dinodes, directory entries |
| `ufs.h`          | In-memory structures, helper macros, function prototypes |
| `super.c`        | `fill_super`, `statfs`, module init/exit |
| `inode.c`        | Inode reading, block mapping, page cache |
| `dir.c`          | `readdir` and `lookup` for directories |
| `file.c`         | Regular file operations (delegates to generic VFS helpers) |
| `Makefile`       | Out-of-tree kernel module build rules |
| `create_ufs1.py` | Python script that creates a minimal UFS1 test image |
| `ufs_reader.c`   | Userspace tool to validate a UFS image without loading the kernel module |

---

## Architecture

### On-disk format

The driver implements the BSD Fast File System (FFS) on-disk layout:

```
Byte offset   Content
-----------   -------
0 – 8191      Boot block (unused)
8192 –        UFS1 superblock (struct ufs_super_block, 1376 bytes)
              fs_magic at offset 1372 = 0x011954 (UFS1) or 0x19540119 (UFS2)
65536 –       UFS2 superblock (alternative location)

Per cylinder group:
  cgbase  = fpg × cg_number                   (in fragment units)
  cgstart = cgbase + cgoffset × (cg & ~cgmask) (UFS1), or cgbase (UFS2)
  cgimin  = cgstart + iblkno                   (inode table start)
  data blocks follow
```

### Block-size strategy

Linux requires `sb->s_blocksize ≥ 1024`. UFS fragment sizes can be as small
as 512 bytes. The driver therefore sets:

```
sb->s_blocksize = fs_bsize  (e.g. 4096 or 8192)
```

Fragment addresses from inode pointers (`di_db[]`, `di_ib[]`) are converted to
Linux block numbers by dividing by `fs_frag`:

```
linux_block = fragment_address / fs_frag
```

### Address calculation (mirrors OpenBSD macros)

```c
cgbase(cg)  = fs_fpg × cg
cgstart(cg) = cgbase(cg) + fs_cgoffset × (cg & ~fs_cgmask)  // UFS1
cgimin(cg)  = cgstart(cg) + fs_iblkno
fsba(ino)   = cgimin(cg) + (ino_in_cg / fs_inopb) × fs_frag
fsbo(ino)   = ino_in_cg % fs_inopb
block_num   = fsba(ino) / fs_frag
```

---

## Building

### Prerequisites

```bash
sudo apt-get install -y linux-headers-$(uname -r) build-essential
```

### Compile

```bash
cd linux-ufs
make
```

The output is `linux_ufs.ko`.

---

## Testing

### 1. Validate image structure (no root required)

```bash
# Create a test UFS1 image
python3 create_ufs1.py /tmp/test.ufs

# Build the userspace reader
gcc -O2 -o /tmp/ufs_reader ufs_reader.c

# Run validation – no root, no kernel module needed
/tmp/ufs_reader /tmp/test.ufs
```

Expected output includes:
```
=== UFS Superblock ===
  Version  : UFS1
  Magic    : 0x00011954
  Volume   : TestVol
  bsize    : 4096
  ...

=== Root Directory Contents ===
  ino=2    type=4 name=.
  ino=2    type=4 name=..
  ino=3    type=8 name=hello.txt

=== File Contents ===
Hello from UFS1!
...
=== All checks PASSED ===
```

### 2. Load and mount (requires root)

```bash
# Load the kernel module
sudo insmod linux_ufs.ko

# Verify registration
cat /proc/filesystems | grep ufs2bsd

# Create a test image
python3 create_ufs1.py /tmp/test.ufs

# Set up a loop device
sudo losetup /dev/loop0 /tmp/test.ufs

# Mount read-only
sudo mkdir -p /mnt/ufstest
sudo mount -t ufs2bsd -o ro /dev/loop0 /mnt/ufstest

# Browse the filesystem
ls -la /mnt/ufstest/
cat /mnt/ufstest/hello.txt
df -h /mnt/ufstest

# Unmount and clean up
sudo umount /mnt/ufstest
sudo losetup -d /dev/loop0
sudo rmmod linux_ufs
```

### 3. Test with a real BSD disk image (OpenBSD/FreeBSD)

If you have an OpenBSD VM or install medium:

```bash
# Extract the image (e.g. from QEMU block device)
qemu-img convert -f qcow2 openbsd.qcow2 -O raw openbsd.raw

# Mount the UFS partition (skip MBR partitions as needed)
sudo losetup -P /dev/loop0 openbsd.raw
sudo mount -t ufs2bsd -o ro /dev/loop0p3 /mnt/ufstest
```

### 4. Test with `newfs` from freebsd-tools (Debian/Ubuntu)

```bash
sudo apt-get install -y freebsd-ufs-fuse  # if available, or use a FreeBSD container
```

Or use FreeBSD in Docker:

```bash
# Build a real UFS image using FreeBSD tools
docker run --rm --privileged freebsd/freebsd-docker-image sh -c "
  dd if=/dev/zero of=/tmp/disk.img bs=1M count=64
  mdconfig -f /tmp/disk.img -t vnode
  newfs /dev/md0
  mount /dev/md0 /mnt
  echo 'hello from newfs' > /mnt/test.txt
  umount /mnt
  mdconfig -d -u 0
" && \
sudo losetup /dev/loop0 /tmp/disk.img && \
sudo mount -t ufs2bsd -o ro /dev/loop0 /mnt/ufstest && \
cat /mnt/ufstest/test.txt
```

---

## Design notes

### Read-only

The driver is intentionally **read-only** (`SB_RDONLY`). Write support requires
implementing fragment allocation, inode updates, journal replay (for soft
updates), and is left as a future exercise.

### Direct + single indirect blocks

`ufs_block_map()` handles the first 12 direct block pointers (`di_db[0..11]`)
and one level of indirection (`di_ib[0]`).  This supports files up to:

```
12 × fs_bsize + (fs_bsize/4) × fs_bsize  (UFS1, 32-bit pointers)
```

For `fs_bsize = 8192`: max ≈ 8 MB + 16 GB ≈ 16 GB.

### Inline symlinks

Short symbolic links (≤ 60 bytes for UFS1, ≤ 120 bytes for UFS2) are stored
directly in the inode's `di_db[]` field array and returned without any I/O.

### Filesystem type name

The module registers as `ufs2bsd` to avoid conflicting with the existing Linux
UFS driver (`ufs`) already in the kernel.

---

## Comparison with OpenBSD UFS

| OpenBSD | Linux driver |
|---------|--------------|
| `ffs_vfsops.c` | `super.c` |
| `ffs_inode.c` | `inode.c` |
| `ufs_lookup.c` | `dir.c` |
| `ufs_vnops.c` | `file.c` |
| `struct vfsops` | `struct super_operations` |
| `struct vnodeops` | `struct inode_operations` + `struct file_operations` |
| `vnode` | `struct inode` + `struct file` |
| `VOP_LOOKUP` | `inode_operations.lookup` |
| `VOP_READDIR` | `file_operations.iterate_shared` |
| `VOP_READ` | `address_space_operations.read_folio` via `mpage` |
| `bread()` | `sb_bread()` |
| `brelse()` | `brelse()` |

---

## License

GPL v2 — see `SPDX-License-Identifier: GPL-2.0` in each source file.
