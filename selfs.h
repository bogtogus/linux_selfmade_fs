#ifndef _SELFS_H
#define _SELFS_H

#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/types.h>



#define SELFS_NAME          "selfs"
#define SELFS_MAGIC         0x53454C46U   /* 'S','E','L','F' */
#define SELFS_VERSION       1U



#define SELFS_SECTOR_SIZE   512



#define SELFS_MAX_NAME      64


#define SELFS_MAX_FILES_LIMIT   8192U



#define SELFS_ROOT_INO          1UL
#define SELFS_FILE_INO_START    2UL



struct selfs_super_block_disk {
	__le32  magic;              
	__le32  version;            
	__le64  total_sectors;      
	__le64  sb_first_offset;    
	__le64  sb_second_offset;   
	__le32  max_name_len;       
	__le32  max_file_sectors;   
	__le32  file_size_sectors;  
	__le32  num_files;          
	__le64  region1_start;      
	__le64  region1_end;        
	__le64  region2_start;      
	__le64  region2_end;        
	__le32  hash;               

	u8      _reserved[512 - 84];
} __packed;


static_assert(sizeof(struct selfs_super_block_disk) == SELFS_SECTOR_SIZE,
	      "selfs_super_block_disk must be exactly 512 bytes");



struct selfs_file_info {
	char    name[SELFS_MAX_NAME];
	u64     offset_sector;       
	u32     size_sectors;        
	u32     ino;                 
};



struct selfs_sb_info {
	struct selfs_super_block_disk   disk_sb;


	u32                     max_name_len;
	u32                     max_file_sectors;
	u32                     file_size_sectors;
	u64                     sb_first_offset;
	u64                     sb_second_offset;


	u32                     num_files;
	struct selfs_file_info *files;

	struct mutex            lock;   
};



struct selfs_inode_info {
	u64             offset_sector;  
	u32             size_sectors;   
	u32             file_idx;       
	struct inode    vfs_inode;      
};



static inline struct selfs_sb_info *SELFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct selfs_inode_info *SELFS_I(struct inode *inode)
{
	return container_of(inode, struct selfs_inode_info, vfs_inode);
}

#endif /* _SELFS_H */

