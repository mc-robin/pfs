#ifndef __LINUX_PFS_H
#define __LINUX_PFS_H

#include	"pfs_fs.h"

#define PFS_DEPTH	5	
#define PFS_ALLOC_INODE	0	
#define PFS_ALLOC_BLOCK	1	

struct pfs_sb_info{
	int64_t	*s_ifree; 	
	int64_t	*s_bfree; 	
	struct mutex s_lock;
	struct buffer_head	*s_sbh;
	struct buffer_head	*s_ibh;
	struct buffer_head	*s_bbh;
	struct pfs_super_block	*s_spb; 
};

struct pfs_inode_info{
	int64_t	i_ino;
	int64_t	i_addr[PFS_NADDR];
	struct inode 	vfs_inode;
};

struct pfs_dir_hash_info{ 
	int64_t	*p;	
	int64_t	off;	
	struct buffer_head *bh; 
};

static inline struct pfs_sb_info *
PFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct pfs_inode_info *
PFS_I(struct inode *inode)
{
	return list_entry(inode, struct pfs_inode_info, vfs_inode);
}

static inline int64_t
pfs_get_de_offset(struct pfs_dir_entry *de)
{
	return le64_to_cpu(de->d_next);
}

static inline int16_t
pfs_get_reclen(int8_t len)
{
	return len < PFS_DIR_RECLEN ? sizeof(struct pfs_dir_entry) : sizeof(struct pfs_dir_entry) + 1 + len; 
}

static inline int16_t
pfs_get_de_size(struct pfs_dir_entry *de)
{
	return le16_to_cpu(de->d_reclen);
}

static inline char *
pfs_get_de_name(struct pfs_dir_entry *de)
{
	return de->d_len < PFS_DIR_RECLEN ? de->d_name : (char *)de + sizeof(*de); 
}

static inline int
pfs_match(const void *qstr, const void *de)
{
        if(((struct qstr *)qstr)->len != ((struct pfs_dir_entry *)de)->d_len) 
                return 0;
        return !memcmp(((struct qstr *)qstr)->name, pfs_get_de_name((struct pfs_dir_entry *)de), ((struct qstr *)qstr)->len);
}

static inline int
pfs_find_empty_entry(const void *qstr, const void *de)
{
        return pfs_get_de_size((struct pfs_dir_entry *)de) >= pfs_get_reclen(((struct qstr *)qstr)->len);
}

static inline void
pfs_add_hdentry(struct pfs_dir_hash_info *hdp, int64_t *p, int64_t off, struct buffer_head *bh)
{
        hdp->p = p;
        hdp->bh = bh;
        hdp->off = off;
}

extern int64_t	pfs_alloc_zero(struct super_block *sb);
extern int64_t	pfs_alloc(struct super_block *sb, int type);
extern int	pfs_free(struct super_block *sb, int64_t dno, int type);
extern int	pfs_clear_block(struct super_block *sb, int64_t dno, int size);

extern int	pfs_empty_dir(struct inode *dir);
extern int	pfs_make_empty(struct inode *inode);
extern int	pfs_add_link(struct dentry *dentry, struct inode *inode);
extern int64_t	pfs_inode_by_name(struct inode *dir, const struct qstr *qstr);
extern int	pfs_delete_entry(struct inode *dir, struct pfs_dir_entry *de, struct buffer_head *bh,
        		struct pfs_dir_hash_info *hdp, struct pfs_dir_hash_info *hdp1);
extern struct pfs_dir_entry *pfs_find_entry(struct inode *dir, const struct qstr *qstr, int (*test)(const void *, const void *),
       	struct pfs_dir_hash_info *hdp, struct pfs_dir_hash_info *hdp1);

extern void	pfs_evict_inode(struct inode *inode);
extern void	pfs_truncate_blocks(struct inode *inode);
extern void	pfs_set_inode(struct inode *inode, dev_t rdev);
extern int	pfs_free_inode(struct inode *inode);
extern int	pfs_truncate(struct inode *inode, int64_t size);
extern int	pfs_write_inode(struct inode *inode, struct writeback_control *wbc);
extern int64_t	pfs_get_block_number(struct inode *inode, sector_t block, int create);
extern struct inode *pfs_iget(struct super_block *sb, int64_t ino);
extern struct inode *pfs_new_inode(struct inode *dir, umode_t mode);

extern const struct address_space_operations pfs_aops;
extern const struct file_operations pfs_dir_operations;
extern const struct file_operations pfs_file_operations;
extern const struct inode_operations pfs_dir_inode_operations;
extern const struct inode_operations pfs_file_inode_operations;

#endif
