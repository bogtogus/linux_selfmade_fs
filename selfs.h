/* SPDX-License-Identifier: GPL-2.0 */
/*
 * selfs.h - Common definitions for the selfs kernel module
 *
 * On-disk layout:
 *
 *   sector 0 .. sb_first_offset-1   : (unused, sb_first_offset is usually 0)
 *   sector sb_first_offset          : SuperBlock copy #1
 *   sector r1_start .. r1_end-1     : files region #1
 *   sector sb_second_offset         : SuperBlock copy #2 (duplicate)
 *   sector r2_start .. r2_end-1     : files region #2
 *
 * The integrity of each superblock is protected by CRC32.
 * Files are pre-created at mkfs time, all with the same size in sectors
 * (file_size_sectors), and their names are auto-generated as "f<NNNNN>".
 */

#ifndef _SELFS_H
#define _SELFS_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mutex.h>

#define SELFS_MAGIC              0x4D594653   /* 'M''Y''F''S' */
#define SELFS_NAME               "selfs"
#define SELFS_VERSION            1
#define SELFS_SECTOR_SIZE        512

#define SELFS_ROOT_INO           1
#define SELFS_FILE_INO_START     2

#define SELFS_MAX_NAME           64           /* compile-time upper bound */
#define SELFS_MAX_FILES_LIMIT    4096         /* cap to keep RAM usage sane */

/*
 * On-disk superblock structure. Exactly one sector (512 bytes).
 * All multi-byte integers are little-endian on disk.
 */
struct selfs_super_block_disk {
	__le32  magic;
	__le32  version;
	__le64  total_sectors;
	__le64  sb_first_offset;
	__le64  sb_second_offset;
	__le32  max_name_len;
	__le32  max_file_sectors;     /* parameter M */
	__le32  file_size_sectors;    /* actual size of each file in sectors */
	__le32  num_files;
	__le64  region1_start;
	__le64  region1_end;          /* exclusive */
	__le64  region2_start;
	__le64  region2_end;          /* exclusive */
	__le32  hash;                 /* CRC32 over the entire struct with this field == 0 */
	__u8    reserved[420];        /* pad to 512 bytes */
} __packed;

/* sanity */
_Static_assert(sizeof(struct selfs_super_block_disk) == SELFS_SECTOR_SIZE,
	       "selfs superblock must be exactly one sector");

/* In-memory description of one file */
struct selfs_file_info {
	char    name[SELFS_MAX_NAME];
	u64     offset_sector;   /* absolute sector on the block device */
	u32     size_sectors;
	u32     ino;
};

/* In-memory FS-wide info (sb->s_fs_info) */
struct selfs_sb_info {
	struct selfs_super_block_disk disk_sb;
	struct selfs_file_info       *files;
	u32                          num_files;
	u32                          max_name_len;
	u32                          max_file_sectors;
	u32                          file_size_sectors;
	u64                          sb_first_offset;
	u64                          sb_second_offset;
	struct mutex                 lock;
};

/* Per-inode private info */
struct selfs_inode_info {
	u64               offset_sector;
	u32               size_sectors;
	u32               file_idx;
	struct inode      vfs_inode;
};

static inline struct selfs_inode_info *SELFS_I(struct inode *inode)
{
	return container_of(inode, struct selfs_inode_info, vfs_inode);
}

static inline struct selfs_sb_info *SELFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

#endif /* __KERNEL__ */

#endif /* _SELFS_H */
