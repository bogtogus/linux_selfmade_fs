// SPDX-License-Identifier: GPL-2.0
/*
 * selfs.c - A minimal block-device-backed filesystem for Linux 6.12.
 *
 * Features:
 *   - Two copies of the superblock with CRC32 integrity check.
 *   - All files are pre-created at format time, same size in sectors.
 *   - Only read/write are supported (no create/unlink/rename/...).
 *   - IOCTLs: zero_all, erase_fs, get_meta (hashes), get_sectors.
 *
 * Module parameters:
 *   device           : path to a block device that will be formatted at
 *                      module load (optional - format is also performed
 *                      at mount time if no valid superblock is found).
 *   sb_first_offset  : sector index of the first superblock copy (default 0).
 *   sb_second_offset : sector index of the second superblock copy (default 1024).
 *   max_name_len     : maximum file-name length used by the FS (default 32).
 *   max_file_sectors : M - maximum file size in sectors (default 4).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/parser.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include "selfs.h"
#include "selfs_ioctl.h"

/* ------------------------------------------------------------------------ */
/* Module parameters                                                        */
/* ------------------------------------------------------------------------ */

static char        *device           = "";
static unsigned int sb_first_offset  = 0;
static unsigned int sb_second_offset = 1024;
static unsigned int max_name_len     = 32;
static unsigned int max_file_sectors = 4;

module_param(device, charp, 0444);
MODULE_PARM_DESC(device,
	"Block device path. If non-empty, the FS will be created on it at module load.");
module_param(sb_first_offset, uint, 0444);
MODULE_PARM_DESC(sb_first_offset, "Sector index of the first superblock copy (default 0)");
module_param(sb_second_offset, uint, 0444);
MODULE_PARM_DESC(sb_second_offset, "Sector index of the second superblock copy (default 1024)");
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Maximum file-name length (default 32)");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors, M (default 4)");

/* ------------------------------------------------------------------------ */
/* Forward declarations                                                     */
/* ------------------------------------------------------------------------ */

static const struct super_operations  selfs_super_ops;
static const struct inode_operations  selfs_dir_inode_ops;
static const struct file_operations   selfs_dir_ops;
static const struct file_operations   selfs_file_ops;
static struct file_system_type        selfs_fs_type;

static struct kmem_cache *selfs_inode_cachep;

/* Static holder tag for bdev_file_open_by_path() during module-load format. */
static char selfs_format_holder_tag;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static u32 selfs_sb_compute_hash(struct selfs_super_block_disk *sb)
{
	__le32 saved_hash = sb->hash;
	u32 hash;

	sb->hash = 0;
	hash = crc32(0xFFFFFFFFU, sb, sizeof(*sb));
	sb->hash = saved_hash;
	return hash;
}

/*
 * Compute on-disk file layout for the given parameters.
 *
 *   region 1: (sb1_offset, sb2_offset)         -> [sb1_offset+1, sb2_offset)
 *   region 2: (sb2_offset, total_sectors)      -> [sb2_offset+1, total_sectors)
 *
 * Files cannot span across sb_second_offset.
 */
static int selfs_compute_layout(u64 total_sectors, u64 sb1, u64 sb2,
			       u32 file_size,
			       u64 *r1s, u64 *r1e,
			       u64 *r2s, u64 *r2e,
			       u32 *num_files)
{
	u64 sz1, sz2;
	u32 n1, n2;

	if (sb1 >= sb2)
		return -EINVAL;
	if (sb2 >= total_sectors)
		return -EINVAL;
	if (file_size == 0)
		return -EINVAL;

	*r1s = sb1 + 1;
	*r1e = sb2;
	*r2s = sb2 + 1;
	*r2e = total_sectors;

	sz1 = *r1e - *r1s;
	sz2 = *r2e - *r2s;

	n1 = (u32)(sz1 / file_size);
	n2 = (u32)(sz2 / file_size);

	*num_files = n1 + n2;
	if (*num_files == 0)
		return -EINVAL;

	if (*num_files > SELFS_MAX_FILES_LIMIT)
		*num_files = SELFS_MAX_FILES_LIMIT;

	return 0;
}

/*
 * Build the in-memory file-info array from the (already populated) disk
 * superblock fields of sbi.
 */
static int selfs_build_file_table(struct selfs_sb_info *sbi)
{
	u64 r1s = le64_to_cpu(sbi->disk_sb.region1_start);
	u64 r1e = le64_to_cpu(sbi->disk_sb.region1_end);
	u64 r2s = le64_to_cpu(sbi->disk_sb.region2_start);
	u64 r2e = le64_to_cpu(sbi->disk_sb.region2_end);
	u32 fsize = sbi->file_size_sectors;
	u32 n1 = (u32)((r1e - r1s) / fsize);
	u32 n2 = (u32)((r2e - r2s) / fsize);
	u32 total = n1 + n2;
	u32 i, idx = 0;

	if (total > SELFS_MAX_FILES_LIMIT)
		total = SELFS_MAX_FILES_LIMIT;
	sbi->num_files = total;

	sbi->files = kvmalloc(total * sizeof(*sbi->files), GFP_KERNEL);
	if (!sbi->files)
		return -ENOMEM;

	for (i = 0; i < n1 && idx < total; i++, idx++) {
		struct selfs_file_info *fi = &sbi->files[idx];

		snprintf(fi->name, SELFS_MAX_NAME, "f%05u", idx);
		/* Honor the user's max_name_len limit. */
		if (sbi->max_name_len < SELFS_MAX_NAME)
			fi->name[sbi->max_name_len - 1] = '\0';
		fi->offset_sector = r1s + (u64)i * fsize;
		fi->size_sectors  = fsize;
		fi->ino           = SELFS_FILE_INO_START + idx;
	}
	for (i = 0; i < n2 && idx < total; i++, idx++) {
		struct selfs_file_info *fi = &sbi->files[idx];

		snprintf(fi->name, SELFS_MAX_NAME, "f%05u", idx);
		if (sbi->max_name_len < SELFS_MAX_NAME)
			fi->name[sbi->max_name_len - 1] = '\0';
		fi->offset_sector = r2s + (u64)i * fsize;
		fi->size_sectors  = fsize;
		fi->ino           = SELFS_FILE_INO_START + idx;
	}

	return 0;
}

/* Read a single sector via buffer-head cache. */
static int selfs_read_sector(struct super_block *sb, u64 sector, void *buf)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh) {
		pr_err("selfs: sb_bread(%llu) failed\n", sector);
		return -EIO;
	}
	memcpy(buf, bh->b_data, SELFS_SECTOR_SIZE);
	brelse(bh);
	return 0;
}

/* Synchronous write of a full sector via the buffer-head cache. */
static int selfs_write_sector(struct super_block *sb, u64 sector, const void *buf)
{
	struct buffer_head *bh;

	bh = sb_getblk(sb, sector);
	if (!bh) {
		pr_err("selfs: sb_getblk(%llu) failed\n", sector);
		return -EIO;
	}
	lock_buffer(bh);
	memcpy(bh->b_data, buf, SELFS_SECTOR_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Superblock I/O                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Read a superblock copy from the given sector, verify magic and CRC.
 * Returns 0 on success.
 */
static int selfs_read_and_verify_sb(struct super_block *sb, u64 sector,
				   struct selfs_super_block_disk *out)
{
	int ret;
	u32 stored, computed;

	ret = selfs_read_sector(sb, sector, out);
	if (ret)
		return ret;

	if (le32_to_cpu(out->magic) != SELFS_MAGIC) {
		pr_info("selfs: SB at sector %llu: bad magic 0x%x\n",
			sector, le32_to_cpu(out->magic));
		return -EINVAL;
	}

	stored   = le32_to_cpu(out->hash);
	computed = selfs_sb_compute_hash(out);
	if (stored != computed) {
		pr_warn("selfs: SB at sector %llu: hash mismatch (stored=0x%08x, computed=0x%08x)\n",
			sector, stored, computed);
		return -EINVAL;
	}

	if (le32_to_cpu(out->version) != SELFS_VERSION) {
		pr_warn("selfs: SB at sector %llu: unsupported version %u\n",
			sector, le32_to_cpu(out->version));
		return -EINVAL;
	}

	return 0;
}

/* Build a fresh superblock structure from current module parameters. */
static int selfs_build_fresh_sb(struct selfs_super_block_disk *out, u64 total_sectors)
{
	u64 r1s, r1e, r2s, r2e;
	u32 num_files;
	int ret;

	ret = selfs_compute_layout(total_sectors, sb_first_offset, sb_second_offset,
				  max_file_sectors,
				  &r1s, &r1e, &r2s, &r2e, &num_files);
	if (ret)
		return ret;

	memset(out, 0, sizeof(*out));
	out->magic             = cpu_to_le32(SELFS_MAGIC);
	out->version           = cpu_to_le32(SELFS_VERSION);
	out->total_sectors     = cpu_to_le64(total_sectors);
	out->sb_first_offset   = cpu_to_le64(sb_first_offset);
	out->sb_second_offset  = cpu_to_le64(sb_second_offset);
	out->max_name_len      = cpu_to_le32(max_name_len);
	out->max_file_sectors  = cpu_to_le32(max_file_sectors);
	out->file_size_sectors = cpu_to_le32(max_file_sectors); /* all files use full M */
	out->num_files         = cpu_to_le32(num_files);
	out->region1_start     = cpu_to_le64(r1s);
	out->region1_end       = cpu_to_le64(r1e);
	out->region2_start     = cpu_to_le64(r2s);
	out->region2_end       = cpu_to_le64(r2e);
	out->hash              = 0;
	out->hash              = cpu_to_le32(selfs_sb_compute_hash(out));
	return 0;
}

/*
 * Format the device that is already opened as sb->s_bdev (called from
 * fill_super when no valid superblock is found on disk).
 */
static int selfs_format_at_mount(struct super_block *sb)
{
	struct selfs_super_block_disk *new_sb;
	u64 total_sectors = bdev_nr_sectors(sb->s_bdev);
	int ret;

	new_sb = kzalloc(sizeof(*new_sb), GFP_KERNEL);
	if (!new_sb)
		return -ENOMEM;

	ret = selfs_build_fresh_sb(new_sb, total_sectors);
	if (ret) {
		pr_err("selfs: invalid parameters for layout\n");
		kfree(new_sb);
		return ret;
	}

	pr_info("selfs: formatting bdev (total=%llu sectors, num_files=%u, file_size=%u)\n",
		total_sectors, le32_to_cpu(new_sb->num_files),
		le32_to_cpu(new_sb->file_size_sectors));

	ret = selfs_write_sector(sb, sb_first_offset, new_sb);
	if (ret)
		goto out;
	ret = selfs_write_sector(sb, sb_second_offset, new_sb);
out:
	kfree(new_sb);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Inode handling                                                           */
/* ------------------------------------------------------------------------ */

static struct inode *selfs_alloc_inode(struct super_block *sb)
{
	struct selfs_inode_info *mi;

	mi = kmem_cache_alloc(selfs_inode_cachep, GFP_KERNEL);
	if (!mi)
		return NULL;
	mi->offset_sector = 0;
	mi->size_sectors  = 0;
	mi->file_idx      = 0;
	return &mi->vfs_inode;
}

static void selfs_free_inode(struct inode *inode)
{
	kmem_cache_free(selfs_inode_cachep, SELFS_I(inode));
}

static void selfs_inode_init_once(void *obj)
{
	struct selfs_inode_info *mi = obj;

	inode_init_once(&mi->vfs_inode);
}

static struct inode *selfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	struct selfs_inode_info *mi;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	mi = SELFS_I(inode);
	simple_inode_init_ts(inode);
	inode->i_ino = ino;
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);

	if (ino == SELFS_ROOT_INO) {
		inode->i_mode = S_IFDIR | 0755;
		inode->i_op   = &selfs_dir_inode_ops;
		inode->i_fop  = &selfs_dir_ops;
		inode->i_size = 0;
		set_nlink(inode, 2);
	} else {
		u32 idx = (u32)(ino - SELFS_FILE_INO_START);
		struct selfs_file_info *fi;

		if (idx >= sbi->num_files) {
			iget_failed(inode);
			return ERR_PTR(-ENOENT);
		}
		fi = &sbi->files[idx];

		inode->i_mode = S_IFREG | 0644;
		inode->i_op   = &simple_dir_inode_operations; /* fine for regulars too */
		inode->i_fop  = &selfs_file_ops;
		inode->i_size = (loff_t)fi->size_sectors * SELFS_SECTOR_SIZE;
		set_nlink(inode, 1);

		mi->offset_sector = fi->offset_sector;
		mi->size_sectors  = fi->size_sectors;
		mi->file_idx      = idx;
	}

	unlock_new_inode(inode);
	return inode;
}

/* ------------------------------------------------------------------------ */
/* Directory operations                                                     */
/* ------------------------------------------------------------------------ */

static struct dentry *selfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	const char *name = dentry->d_name.name;
	u32 nlen = dentry->d_name.len;
	u32 i;

	if (nlen >= SELFS_MAX_NAME)
		return ERR_PTR(-ENAMETOOLONG);

	for (i = 0; i < sbi->num_files; i++) {
		struct selfs_file_info *fi = &sbi->files[i];

		if (strlen(fi->name) == nlen &&
		    memcmp(fi->name, name, nlen) == 0) {
			struct inode *inode = selfs_iget(sb, fi->ino);

			if (IS_ERR(inode))
				return ERR_CAST(inode);
			return d_splice_alias(inode, dentry);
		}
	}

	/* not found -> negative dentry */
	return d_splice_alias(NULL, dentry);
}

static int selfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct selfs_sb_info *sbi = SELFS_SB(sb);

	if (!dir_emit_dots(file, ctx))
		return 0;

	/* ctx->pos == 0 -> "."  (handled by dir_emit_dots)
	 * ctx->pos == 1 -> ".." (handled by dir_emit_dots)
	 * ctx->pos == 2 + idx -> sbi->files[idx]
	 */
	while ((u32)(ctx->pos - 2) < sbi->num_files) {
		u32 idx = (u32)(ctx->pos - 2);
		struct selfs_file_info *fi = &sbi->files[idx];

		if (!dir_emit(ctx, fi->name, strlen(fi->name),
			      fi->ino, DT_REG))
			break;
		ctx->pos++;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* File operations (read / write)                                           */
/* ------------------------------------------------------------------------ */

static ssize_t selfs_file_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct selfs_inode_info *mi = SELFS_I(inode);
	struct super_block *sb = inode->i_sb;
	loff_t pos = *ppos;
	loff_t file_size = (loff_t)mi->size_sectors * SELFS_SECTOR_SIZE;
	size_t copied = 0;

	if (pos < 0)
		return -EINVAL;
	if (pos >= file_size)
		return 0;
	if ((loff_t)count > file_size - pos)
		count = (size_t)(file_size - pos);

	while (copied < count) {
		u64 sec_in_file = (u64)(pos + copied) / SELFS_SECTOR_SIZE;
		size_t off_in_sec = (size_t)((pos + copied) % SELFS_SECTOR_SIZE);
		u64 disk_sec = mi->offset_sector + sec_in_file;
		struct buffer_head *bh;
		size_t can = SELFS_SECTOR_SIZE - off_in_sec;
		size_t to_copy = min(can, count - copied);

		bh = sb_bread(sb, disk_sec);
		if (!bh)
			return copied ? (ssize_t)copied : -EIO;

		if (copy_to_user(buf + copied, bh->b_data + off_in_sec, to_copy)) {
			brelse(bh);
			return copied ? (ssize_t)copied : -EFAULT;
		}
		brelse(bh);
		copied += to_copy;
	}

	*ppos = pos + copied;
	return (ssize_t)copied;
}

static ssize_t selfs_file_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct selfs_inode_info *mi = SELFS_I(inode);
	struct super_block *sb = inode->i_sb;
	loff_t pos;
	loff_t file_size = (loff_t)mi->size_sectors * SELFS_SECTOR_SIZE;
	size_t copied = 0;

	if (file->f_flags & O_APPEND)
		pos = file_size;
	else
		pos = *ppos;

	if (pos < 0)
		return -EINVAL;
	if (pos >= file_size)
		return -ENOSPC;
	if ((loff_t)count > file_size - pos)
		count = (size_t)(file_size - pos);

	while (copied < count) {
		u64 sec_in_file = (u64)(pos + copied) / SELFS_SECTOR_SIZE;
		size_t off_in_sec = (size_t)((pos + copied) % SELFS_SECTOR_SIZE);
		u64 disk_sec = mi->offset_sector + sec_in_file;
		struct buffer_head *bh;
		size_t can = SELFS_SECTOR_SIZE - off_in_sec;
		size_t to_copy = min(can, count - copied);

		/* Full-sector overwrite -> no need to read first. */
		if (off_in_sec == 0 && to_copy == SELFS_SECTOR_SIZE)
			bh = sb_getblk(sb, disk_sec);
		else
			bh = sb_bread(sb, disk_sec);
		if (!bh)
			return copied ? (ssize_t)copied : -EIO;

		lock_buffer(bh);
		if (copy_from_user(bh->b_data + off_in_sec, buf + copied, to_copy)) {
			unlock_buffer(bh);
			brelse(bh);
			return copied ? (ssize_t)copied : -EFAULT;
		}
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		copied += to_copy;
	}

	*ppos = pos + copied;
	return (ssize_t)copied;
}

/* ------------------------------------------------------------------------ */
/* IOCTL handlers                                                           */
/* ------------------------------------------------------------------------ */

static long selfs_ioctl_zero_all(struct super_block *sb)
{
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	void *zeros;
	u32 i;
	u64 s;
	int ret = 0;

	zeros = kzalloc(SELFS_SECTOR_SIZE, GFP_KERNEL);
	if (!zeros)
		return -ENOMEM;

	mutex_lock(&sbi->lock);
	for (i = 0; i < sbi->num_files; i++) {
		struct selfs_file_info *fi = &sbi->files[i];

		for (s = 0; s < fi->size_sectors; s++) {
			ret = selfs_write_sector(sb, fi->offset_sector + s, zeros);
			if (ret)
				goto out;
		}
	}
	pr_info("selfs: ioctl ZERO_ALL: zeroed %u files\n", sbi->num_files);
out:
	mutex_unlock(&sbi->lock);
	kfree(zeros);
	return ret;
}

static long selfs_ioctl_erase_fs(struct super_block *sb)
{
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	void *zeros;
	int ret;

	/* First zero all file data. */
	ret = selfs_ioctl_zero_all(sb);
	if (ret)
		return ret;

	zeros = kzalloc(SELFS_SECTOR_SIZE, GFP_KERNEL);
	if (!zeros)
		return -ENOMEM;

	mutex_lock(&sbi->lock);
	ret = selfs_write_sector(sb, sbi->sb_first_offset, zeros);
	if (ret)
		goto out;
	ret = selfs_write_sector(sb, sbi->sb_second_offset, zeros);
	if (ret)
		goto out;
	pr_info("selfs: ioctl ERASE_FS: both superblocks zeroed\n");
out:
	mutex_unlock(&sbi->lock);
	kfree(zeros);
	return ret;
}

static long selfs_ioctl_get_meta(struct super_block *sb,
				struct selfs_meta_list __user *uarg)
{
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	struct selfs_meta_list *meta;
	void *file_buf;
	size_t file_bytes;
	u32 i, s;
	int ret = 0;

	meta = kvzalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta)
		return -ENOMEM;

	file_bytes = (size_t)sbi->file_size_sectors * SELFS_SECTOR_SIZE;
	file_buf = kvmalloc(file_bytes, GFP_KERNEL);
	if (!file_buf) {
		kvfree(meta);
		return -ENOMEM;
	}

	mutex_lock(&sbi->lock);
	meta->num_files = min(sbi->num_files, (u32)SELFS_MAX_FILES_IOCTL);

	for (i = 0; i < meta->num_files; i++) {
		struct selfs_file_info *fi = &sbi->files[i];
		struct selfs_file_meta *fm = &meta->files[i];

		strscpy(fm->name, fi->name, SELFS_NAME_LEN);
		fm->offset_sector = fi->offset_sector;
		fm->size_sectors  = fi->size_sectors;

		for (s = 0; s < fi->size_sectors; s++) {
			ret = selfs_read_sector(sb, fi->offset_sector + s,
					       (u8 *)file_buf + s * SELFS_SECTOR_SIZE);
			if (ret)
				goto out;
		}
		fm->hash = crc32(0xFFFFFFFFU, file_buf, file_bytes);
	}
out:
	mutex_unlock(&sbi->lock);

	if (!ret && copy_to_user(uarg, meta, sizeof(*meta)))
		ret = -EFAULT;

	kvfree(file_buf);
	kvfree(meta);
	return ret;
}

static long selfs_ioctl_get_sectors(struct super_block *sb,
				   struct selfs_sector_map __user *uarg)
{
	struct selfs_sb_info *sbi = SELFS_SB(sb);
	struct selfs_sector_map *map;
	u32 i, s;
	int ret = -ENOENT;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	if (copy_from_user(map, uarg, sizeof(*map))) {
		ret = -EFAULT;
		goto out;
	}
	map->name[SELFS_NAME_LEN - 1] = '\0';

	mutex_lock(&sbi->lock);
	for (i = 0; i < sbi->num_files; i++) {
		struct selfs_file_info *fi = &sbi->files[i];

		if (strncmp(fi->name, map->name, SELFS_NAME_LEN) != 0)
			continue;

		map->num_sectors = min((u32)fi->size_sectors,
				       (u32)SELFS_MAX_SECTORS_MAP);
		for (s = 0; s < map->num_sectors; s++)
			map->sectors[s] = fi->offset_sector + s;
		ret = 0;
		break;
	}
	mutex_unlock(&sbi->lock);

	if (ret == 0 && copy_to_user(uarg, map, sizeof(*map)))
		ret = -EFAULT;
out:
	kfree(map);
	return ret;
}

static long selfs_unlocked_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;

	if (_IOC_TYPE(cmd) != SELFS_IOCTL_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case SELFS_IOCTL_ZERO_ALL:
		return selfs_ioctl_zero_all(sb);
	case SELFS_IOCTL_ERASE_FS:
		return selfs_ioctl_erase_fs(sb);
	case SELFS_IOCTL_GET_META:
		return selfs_ioctl_get_meta(sb,
				(struct selfs_meta_list __user *)arg);
	case SELFS_IOCTL_GET_SECTORS:
		return selfs_ioctl_get_sectors(sb,
				(struct selfs_sector_map __user *)arg);
	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------------ */
/* Operation tables                                                         */
/* ------------------------------------------------------------------------ */

static void selfs_put_super(struct super_block *sb)
{
	struct selfs_sb_info *sbi = SELFS_SB(sb);

	if (sbi) {
		if (sbi->files)
			kvfree(sbi->files);
		mutex_destroy(&sbi->lock);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
	pr_info("selfs: unmounted\n");
}

static const struct super_operations selfs_super_ops = {
	.alloc_inode = selfs_alloc_inode,
	.free_inode  = selfs_free_inode,
	.put_super   = selfs_put_super,
	.statfs      = simple_statfs,
	.drop_inode  = generic_delete_inode,
};

static const struct inode_operations selfs_dir_inode_ops = {
	.lookup = selfs_lookup,
};

static const struct file_operations selfs_dir_ops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = selfs_iterate_shared,
	.llseek         = default_llseek,
	.unlocked_ioctl = selfs_unlocked_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static const struct file_operations selfs_file_ops = {
	.owner          = THIS_MODULE,
	.read           = selfs_file_read,
	.write          = selfs_file_write,
	.llseek         = default_llseek,
	.unlocked_ioctl = selfs_unlocked_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ------------------------------------------------------------------------ */
/* fill_super / fs_context                                                  */
/* ------------------------------------------------------------------------ */

static int selfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct selfs_sb_info *sbi;
	struct inode *root;
	int ret;
	bool need_format = false;

	if (!sb_set_blocksize(sb, SELFS_SECTOR_SIZE)) {
		pr_err("selfs: block device does not support %d-byte blocks\n",
		       SELFS_SECTOR_SIZE);
		return -EINVAL;
	}

	sb->s_magic    = SELFS_MAGIC;
	sb->s_op       = &selfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	mutex_init(&sbi->lock);
	sb->s_fs_info = sbi;

	/* Try to read either superblock copy. */
	ret = selfs_read_and_verify_sb(sb, sb_first_offset, &sbi->disk_sb);
	if (ret) {
		pr_info("selfs: primary SB invalid, trying backup at sector %u\n",
			sb_second_offset);
		ret = selfs_read_and_verify_sb(sb, sb_second_offset, &sbi->disk_sb);
		if (ret) {
			pr_info("selfs: no valid superblock found, will format\n");
			need_format = true;
		} else {
			/* Backup is good - restore the primary. */
			pr_info("selfs: restoring primary SB from backup\n");
			ret = selfs_write_sector(sb, sb_first_offset, &sbi->disk_sb);
			if (ret)
				goto fail;
		}
	} else {
		/* Primary is good - make sure the backup matches. */
		struct selfs_super_block_disk tmp;

		ret = selfs_read_and_verify_sb(sb, sb_second_offset, &tmp);
		if (ret) {
			pr_info("selfs: rewriting backup SB from primary\n");
			ret = selfs_write_sector(sb, sb_second_offset, &sbi->disk_sb);
			if (ret)
				goto fail;
		}
	}

	if (need_format) {
		ret = selfs_format_at_mount(sb);
		if (ret)
			goto fail;
		ret = selfs_read_and_verify_sb(sb, sb_first_offset, &sbi->disk_sb);
		if (ret)
			goto fail;
	}

	/* Populate in-memory fields from the disk SB. */
	sbi->max_name_len      = le32_to_cpu(sbi->disk_sb.max_name_len);
	sbi->max_file_sectors  = le32_to_cpu(sbi->disk_sb.max_file_sectors);
	sbi->file_size_sectors = le32_to_cpu(sbi->disk_sb.file_size_sectors);
	sbi->sb_first_offset   = le64_to_cpu(sbi->disk_sb.sb_first_offset);
	sbi->sb_second_offset  = le64_to_cpu(sbi->disk_sb.sb_second_offset);
	if (sbi->max_name_len == 0 || sbi->max_name_len > SELFS_MAX_NAME)
		sbi->max_name_len = SELFS_MAX_NAME;

	ret = selfs_build_file_table(sbi);
	if (ret)
		goto fail;

	pr_info("selfs: mounted, %u files, file_size=%u sectors, sb1@%llu sb2@%llu\n",
		sbi->num_files, sbi->file_size_sectors,
		sbi->sb_first_offset, sbi->sb_second_offset);

	root = selfs_iget(sb, SELFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto fail;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail;
	}

	return 0;

fail:
	if (sbi->files)
		kvfree(sbi->files);
	mutex_destroy(&sbi->lock);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

static int selfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, selfs_fill_super);
}

static void selfs_free_fc(struct fs_context *fc)
{
	/* Nothing to free on our side. */
}

static const struct fs_context_operations selfs_context_ops = {
	.get_tree = selfs_get_tree,
	.free     = selfs_free_fc,
};

static int selfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &selfs_context_ops;
	return 0;
}

static struct file_system_type selfs_fs_type = {
	.owner            = THIS_MODULE,
	.name             = SELFS_NAME,
	.init_fs_context  = selfs_init_fs_context,
	.kill_sb          = kill_block_super,
	.fs_flags         = FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS(SELFS_NAME);

/* ------------------------------------------------------------------------ */
/* Optional: format the device at module load                               */
/* ------------------------------------------------------------------------ */

static int selfs_format_device_at_load(const char *path)
{
	struct file *bdev_file;
	struct block_device *bdev;
	struct selfs_super_block_disk *new_sb;
	u64 total_sectors;
	loff_t pos;
	ssize_t written;
	int ret;

	bdev_file = bdev_file_open_by_path(path,
					   BLK_OPEN_READ | BLK_OPEN_WRITE,
					   &selfs_format_holder_tag, NULL);
	if (IS_ERR(bdev_file)) {
		pr_err("selfs: cannot open %s: %ld\n", path, PTR_ERR(bdev_file));
		return PTR_ERR(bdev_file);
	}

	bdev = file_bdev(bdev_file);
	total_sectors = bdev_nr_sectors(bdev);

	new_sb = kzalloc(sizeof(*new_sb), GFP_KERNEL);
	if (!new_sb) {
		ret = -ENOMEM;
		goto out_close;
	}

	ret = selfs_build_fresh_sb(new_sb, total_sectors);
	if (ret) {
		pr_err("selfs: invalid layout parameters at load: %d\n", ret);
		goto out_free;
	}

	pr_info("selfs: load-time format on %s, total=%llu sectors, num_files=%u\n",
		path, total_sectors, le32_to_cpu(new_sb->num_files));

	pos = (loff_t)sb_first_offset * SELFS_SECTOR_SIZE;
	written = kernel_write(bdev_file, new_sb, sizeof(*new_sb), &pos);
	if (written != sizeof(*new_sb)) {
		pr_err("selfs: write primary SB failed: %zd\n", written);
		ret = (written < 0) ? (int)written : -EIO;
		goto out_free;
	}

	pos = (loff_t)sb_second_offset * SELFS_SECTOR_SIZE;
	written = kernel_write(bdev_file, new_sb, sizeof(*new_sb), &pos);
	if (written != sizeof(*new_sb)) {
		pr_err("selfs: write backup SB failed: %zd\n", written);
		ret = (written < 0) ? (int)written : -EIO;
		goto out_free;
	}

	vfs_fsync(bdev_file, 0);
	ret = 0;

out_free:
	kfree(new_sb);
out_close:
	fput(bdev_file);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Module init / exit                                                       */
/* ------------------------------------------------------------------------ */

static int __init selfs_init(void)
{
	int ret;

	selfs_inode_cachep = kmem_cache_create("selfs_inode_cache",
					      sizeof(struct selfs_inode_info),
					      0,
					      SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					      selfs_inode_init_once);
	if (!selfs_inode_cachep)
		return -ENOMEM;

	ret = register_filesystem(&selfs_fs_type);
	if (ret) {
		pr_err("selfs: register_filesystem failed: %d\n", ret);
		kmem_cache_destroy(selfs_inode_cachep);
		return ret;
	}

	pr_info("selfs: module loaded (sb1=%u, sb2=%u, name_len=%u, M=%u, device=\"%s\")\n",
		sb_first_offset, sb_second_offset, max_name_len, max_file_sectors,
		device ? device : "");

	if (device && device[0] != '\0') {
		ret = selfs_format_device_at_load(device);
		if (ret) {
			pr_warn("selfs: load-time format of \"%s\" failed: %d (the FS can still be created at mount time)\n",
				device, ret);
			/* not fatal - mount can still format the device */
		}
	}

	return 0;
}

static void __exit selfs_exit(void)
{
	unregister_filesystem(&selfs_fs_type);
	/* Make sure RCU callbacks finish before destroying the cache. */
	rcu_barrier();
	kmem_cache_destroy(selfs_inode_cachep);
	pr_info("selfs: module unloaded\n");
}

module_init(selfs_init);
module_exit(selfs_exit);

MODULE_AUTHOR("selfs project");
MODULE_DESCRIPTION("A minimal block-device filesystem with redundant superblocks");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
