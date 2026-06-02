#ifndef _SELFS_IOCTL_H
#define _SELFS_IOCTL_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/ioctl.h>
#else
#  include <linux/types.h>   /* provides __u32 / __u64 (matches kernel) */
#  include <sys/ioctl.h>
#endif

#define SELFS_IOCTL_MAGIC        'M'

#define SELFS_NAME_LEN           64
#define SELFS_MAX_FILES_IOCTL    4096
#define SELFS_MAX_SECTORS_MAP    64

struct selfs_file_meta {
	char    name[SELFS_NAME_LEN];
	__u32   hash;
	__u32   size_sectors;
	__u64   offset_sector;
};

struct selfs_meta_list {
	__u32                  num_files;
	__u32                  _pad;
	struct selfs_file_meta  files[SELFS_MAX_FILES_IOCTL];
};

struct selfs_sector_map {
	char    name[SELFS_NAME_LEN];
	__u32   num_sectors;
	__u32   _pad;
	__u64   sectors[SELFS_MAX_SECTORS_MAP];
};

#define SELFS_IOCTL_ZERO_ALL     _IO  (SELFS_IOCTL_MAGIC, 1)
#define SELFS_IOCTL_ERASE_FS     _IO  (SELFS_IOCTL_MAGIC, 2)

#define SELFS_IOCTL_GET_META     _IOWR(SELFS_IOCTL_MAGIC, 3, __u64)
#define SELFS_IOCTL_GET_SECTORS  _IOWR(SELFS_IOCTL_MAGIC, 4, __u64)

#endif /* _SELFS_IOCTL_H */
