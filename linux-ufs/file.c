// SPDX-License-Identifier: GPL-2.0
/*
 * Linux UFS (Fast File System) driver – file operations
 * Inspired by OpenBSD sys/ufs/ufs/ufs_vnops.c
 *
 * Regular file operations delegate entirely to the generic Linux VFS
 * helpers; the real work (block mapping) is done in inode.c via
 * ufs_get_block / ufs_aops.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ufs.h"
#include "ufs_fs.h"

/*
 * All file operation functions are already defined in inode.c as part of:
 *   ufs_file_inode_ops
 *   ufs_file_operations
 *   ufs_aops
 *   ufs_symlink_inode_ops
 *
 * This file exists to keep the build structure consistent with a typical
 * multi-file Linux filesystem driver (compare ext2, minix, etc.) and may
 * be extended with write support in the future.
 *
 * Read-only UFS driver – no write operations are implemented.
 */
