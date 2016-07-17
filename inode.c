#include	<linux/fs.h> 
#include	<linux/namei.h>
#include	<linux/version.h>
#include	<linux/writeback.h>
#include	<linux/buffer_head.h>
#include	<linux/mpage.h>
#include	"pfs.h"

typedef struct{
	int64_t	*p;
	int64_t	key;
	struct buffer_head *bh;
}Indirect;

static inline int
pfs_depth(int x)
{
	if(x < (int)PFS_D_BLOCK) 
		return 1;
	if((x -= (int)PFS_D_BLOCK) < (int)PFS_IND_BLOCK) 
		return 2;
	return (x - (int)PFS_IND_BLOCK) / PFS_DIND_BLOCK + 3; 
}

static inline void
pfs_add_chain(Indirect *p, struct buffer_head *bh, sector_t *v)
{
	p->bh = bh;
	p->key = le64_to_cpu(*(p->p = v)); 
}

static inline void
pfs_free_chain(Indirect *from, Indirect *to)
{
	while(from > to){
		brelse(from->bh);
		from--;	
	}
}

static int
pfs_test(struct inode *inode, void *data)
{
	return PFS_I(inode)->i_ino == *((int64_t *)data);
}

static int
pfs_set(struct inode *inode, void *data)
{
	inode->i_ino = *((uint32_t *)data);
	PFS_I(inode)->i_ino = *((int64_t *)data);
	return 0;	
}

static int
pfs_atomic_alloc(struct inode *inode, Indirect *p)
{
	int err = 0;
	int64_t	dno;
	struct super_block *sb = inode->i_sb;
	struct pfs_sb_info *sbi = PFS_SB(sb);

	mutex_lock(&sbi->s_lock);
	if(!(dno = pfs_alloc(sb, PFS_ALLOC_BLOCK))){ 
		err = -1;
		goto out;
	}
	if(p->bh){ 
		p->key = dno;
		*(p->p) = cpu_to_le64(dno); 
		mark_buffer_dirty_inode(p->bh, inode);
	}else
		p->key = *(p->p) = dno; 
	inode->i_blocks++; 
	inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode); 
out:
	mutex_unlock(&sbi->s_lock);
	return err;
}

static int
pfs_atomic_free(struct inode *inode, Indirect *p)
{
	int err = 0;
	struct super_block *sb = inode->i_sb;
	struct pfs_sb_info *sbi = PFS_SB(sb);
	
	mutex_lock(&sbi->s_lock);
	if(pfs_free(sb, p->key, PFS_ALLOC_BLOCK)){
		err = -1;
		goto out;
	}
	p->key = *(p->p) = 0; 
	if(p->bh)
		mark_buffer_dirty_inode(p->bh, inode);
	inode->i_blocks--; 
        inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
out:
	mutex_unlock(&sbi->s_lock);
	return 0;
}

static int
pfs_bmap_free(struct inode *inode, Indirect *q, int64_t *offset, int depth, int whole)
{
	if(--depth){
		int	i;
		Indirect chain;
		struct buffer_head *bh;

		if(!q->key) 
			return 0;
		if(!(bh = sb_bread(inode->i_sb, q->key / PFS_STRS_PER_BLOCK))) 
			return -1;
		if(whole){ 
			for(i = 0; i < PFS_INBLOCKS; i++){
				pfs_add_chain(&chain, bh, (int64_t *)bh->b_data + i);
				pfs_bmap_free(inode, &chain, NULL, depth, 1);
			}
			bforget(bh);
			pfs_atomic_free(inode, q);
		}else{
			for(i = offset[0]; i < PFS_INBLOCKS; i++){
				pfs_add_chain(&chain, bh, (int64_t *)bh->b_data + i);
				pfs_bmap_free(inode, &chain, i == offset[0] ? offset + 1 : NULL, depth, i == offset[0] ? 0 : 1);
			}	
			brelse(bh);
		}
	}else{
		if(!q->key)
			return 0;
		if(whole)
			return pfs_atomic_free(inode, q);
	}
	return 0;
}

static int64_t
pfs_bmap_alloc(struct inode *inode, int64_t *offset, int depth)
{
	int64_t	tm;
	Indirect chain[PFS_DEPTH], *q = chain;

	pfs_add_chain(q, NULL, PFS_I(inode)->i_addr + *offset);
        if(!(tm = q->key) && pfs_atomic_alloc(inode, q))
                goto no_block;
        while(--depth){
                struct buffer_head      *bh;

                if(!(bh = sb_bread(inode->i_sb, q->key / PFS_STRS_PER_BLOCK)))
                        goto no_block;
                if(!tm)
                        memset(bh->b_data, 0, PFS_BLOCKSIZ);
                pfs_add_chain(++q, bh, (int64_t *)bh->b_data + *++offset);
                if(!(tm = q->key) && pfs_atomic_alloc(inode, q))
                        goto no_block;
        }
        pfs_free_chain(q, chain);
        return q->key;
no_block:
        pfs_free_chain(q, chain);
        return 0;
}

static int64_t
pfs_bmap(struct inode *inode, int64_t *offset, int depth)
{
	Indirect chain[PFS_DEPTH], *q = chain;

	pfs_add_chain(q, NULL, PFS_I(inode)->i_addr + *offset);
	if(!q->key)
		goto no_block;
	while(--depth){
		struct buffer_head	*bh;

		if(!(bh = sb_bread(inode->i_sb, q->key / PFS_STRS_PER_BLOCK)))
			goto no_block;
		pfs_add_chain(++q, bh, (int64_t *)bh->b_data + *++offset);
		if(!q->key)
			goto no_block;
	}
	pfs_free_chain(q, chain);
	return q->key;
no_block:
	pfs_free_chain(q, chain);
	return 0;
}

static int
pfs_block_to_path(struct inode *inode, sector_t block, int64_t *offsets)
{
	int	n = 0; 

	if(block < 0 || block > PFS_MAXBLOCKS){ 
		pr_warn("pfs: device %s: %s: block %lld too small or big\n", inode->i_sb->s_id, "pfs_block_to_path", block);
		return n;
	}
	if(block < PFS_D_BLOCK){ 
		offsets[n++] = block;	
	}else if((block -= PFS_D_BLOCK) < PFS_IND_BLOCK * PFS_IND_BLOCKS){ 
		offsets[n++] = PFS_D_BLOCK + block / PFS_IND_BLOCKS; 
		offsets[n++] = block % PFS_IND_BLOCKS; 
	}else if((block -= PFS_IND_BLOCK * PFS_IND_BLOCKS) < PFS_DIND_BLOCK * PFS_DIND_BLOCKS){ 
		offsets[n++] = PFS_D_BLOCK + PFS_IND_BLOCK + block / PFS_DIND_BLOCKS; 
		offsets[n++] = (block % PFS_DIND_BLOCKS) / PFS_IND_BLOCKS; 
		offsets[n++] = (block % PFS_DIND_BLOCKS) % PFS_IND_BLOCKS; 
	}else if((block -= PFS_DIND_BLOCK * PFS_DIND_BLOCKS) < PFS_TIND_BLOCK * PFS_TIND_BLOCKS){ 
		offsets[n++] = PFS_D_BLOCK + PFS_IND_BLOCK + PFS_DIND_BLOCK + block / PFS_TIND_BLOCKS; 
		offsets[n++] = (block % PFS_TIND_BLOCKS) / PFS_DIND_BLOCKS; 
		offsets[n++] = (block % PFS_TIND_BLOCKS) % PFS_DIND_BLOCKS / PFS_IND_BLOCKS; 
		offsets[n++] = (block % PFS_TIND_BLOCKS) % PFS_DIND_BLOCKS % PFS_IND_BLOCKS; 
	}else{ 	
		block -= PFS_TIND_BLOCK * PFS_TIND_BLOCKS;
		offsets[n++] = PFS_D_BLOCK + PFS_IND_BLOCK + PFS_DIND_BLOCK + PFS_TIND_BLOCK + block / PFS_QIND_BLOCKS; 
		offsets[n++] = (block % PFS_QIND_BLOCKS) / PFS_TIND_BLOCKS; 
		offsets[n++] = (block % PFS_QIND_BLOCKS) % PFS_TIND_BLOCKS / PFS_DIND_BLOCKS; 
		offsets[n++] = (block % PFS_QIND_BLOCKS) % PFS_TIND_BLOCKS % PFS_DIND_BLOCKS / PFS_IND_BLOCKS; 
		offsets[n++] = (block % PFS_QIND_BLOCKS) % PFS_TIND_BLOCKS % PFS_DIND_BLOCKS % PFS_IND_BLOCKS; 
	}
	return n;
}

static int
pfs_get_block(struct inode *inode, sector_t block, struct buffer_head *bh, int create)
{
	int64_t	dno;
	int	depth;
	int64_t	offset[PFS_DEPTH];

	if(unlikely(!(depth = pfs_block_to_path(inode, block, offset)))) 
		return -EIO;
	if(!create){
		if(!(dno = pfs_bmap(inode, offset, depth)))
			return -EIO;
		goto out;
	}
	if(!(dno = pfs_bmap_alloc(inode, offset, depth)))
		return -EIO;
out:
	map_bh(bh, inode->i_sb, dno / PFS_STRS_PER_BLOCK);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0) 
static void *
pfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *inode = d_inode(dentry);
#else
        struct inode *inode = dentry->d_inode;
#endif
	nd_set_link(nd, (char *)(PFS_I(inode)->i_addr));
	return NULL;
}

const struct inode_operations simple_symlink_inode_operations = {
	.follow_link = pfs_follow_link,
        .readlink = generic_readlink
};
#endif
void
pfs_set_inode(struct inode *inode, dev_t rdev)
{
	if(S_ISREG(inode->i_mode)){
		inode->i_fop = &pfs_file_operations;
		inode->i_mapping->a_ops = &pfs_aops;
		inode->i_op = &pfs_file_inode_operations;
	}else if(S_ISDIR(inode->i_mode)){
		inode->i_fop = &pfs_dir_operations;
		inode->i_mapping->a_ops = &pfs_aops;
		inode->i_op = &pfs_dir_inode_operations;
	}else if(S_ISLNK(inode->i_mode)){
		if(!inode->i_blocks){ 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
			inode->i_link = (char *)PFS_I(inode)->i_addr; 
#endif
			inode->i_op = &simple_symlink_inode_operations;
		}else{ 
			inode->i_op = &page_symlink_inode_operations;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
			inode_nohighmem(inode);
#endif
			inode->i_mapping->a_ops = &pfs_aops;
		}
	}else
		init_special_inode(inode, inode->i_mode, rdev);
}

static int
pfs_update_inode(struct inode *inode, struct writeback_control *wbc)
{
        int     i;
        struct buffer_head      *bh;
        struct pfs_inode        *ip;

	if(!(bh = sb_bread(inode->i_sb, PFS_I(inode)->i_ino / PFS_INDS_PER_BLOCK))){
		pr_warn("pfs: device %s: %s: failed to read inode %lld\n", inode->i_sb->s_id, "pfs_update_inode", PFS_I(inode)->i_ino);
		return -EIO;
	}
	ip = (struct pfs_inode *)bh->b_data + PFS_I(inode)->i_ino % PFS_INDS_PER_BLOCK;
        ip->i_mode = cpu_to_le32(inode->i_mode);
        ip->i_uid = cpu_to_le32(i_uid_read(inode));
        ip->i_gid = cpu_to_le32(i_gid_read(inode));
        ip->i_nlink = cpu_to_le32(inode->i_nlink);
	ip->i_size = cpu_to_le64(inode->i_size);
	ip->i_blocks = cpu_to_le64(inode->i_blocks);
        ip->i_atime = cpu_to_le64(inode->i_atime.tv_sec);
        ip->i_mtime = cpu_to_le64(inode->i_mtime.tv_sec);
        ip->i_ctime = cpu_to_le64(inode->i_ctime.tv_sec);
        if(S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)){
                ip->i_addr[0] = (int64_t)cpu_to_le32(new_encode_dev(inode->i_rdev));
        }else if(S_ISLNK(inode->i_mode) && !inode->i_blocks){ 
		memmove(ip->i_addr, PFS_I(inode)->i_addr, sizeof(ip->i_addr)); 
	}else{
                for(i = 0; i < PFS_NADDR; i++)
			ip->i_addr[i] = cpu_to_le64(PFS_I(inode)->i_addr[i]);
        }
        mark_buffer_dirty(bh);
        if(wbc->sync_mode == WB_SYNC_ALL && buffer_dirty(bh)){ 
                sync_dirty_buffer(bh);
                if(buffer_req(bh) && !buffer_uptodate(bh)){ 
			pr_warn("pfs: device %s: %s: failed to update inode %lld\n", 
				inode->i_sb->s_id, "pfs_update_inode", PFS_I(inode)->i_ino);
                        brelse(bh);
                        return -EIO;
                }
        }
        brelse(bh);
        return 0;
}

static void
__pfs_truncate_blocks(struct inode *inode)
{
	int	i;
	Indirect chain;
	int64_t offset[PFS_DEPTH];

        if(unlikely(!pfs_block_to_path(inode, (inode->i_size + PFS_BLOCKSIZ - 1) >> PFS_BLOCKSFT, offset))) 
                return;
	for(i = offset[0]; i < PFS_NADDR; i++){
		pfs_add_chain(&chain, NULL, PFS_I(inode)->i_addr + i);
		if(i < PFS_D_BLOCK){ 
			pfs_bmap_free(inode, &chain, NULL, 1, 1);
		}else 
			pfs_bmap_free(inode, &chain, i == offset[0] ? offset + 1 : NULL, pfs_depth(i), i == offset[0] ? 0 : 1);
	}
	inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
}

int64_t
pfs_get_block_number(struct inode *inode, sector_t block, int create)
{
	int	depth;
        int64_t offset[PFS_DEPTH];

        if(unlikely(!(depth = pfs_block_to_path(inode, block, offset)))) 
		return 0;
        if(!create)
		return pfs_bmap(inode, offset, depth);
	return pfs_bmap_alloc(inode, offset, depth);
}

int
pfs_truncate(struct inode *inode, int64_t size)
{
	int err;

	if(!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
                return -EINVAL;
	if(S_ISLNK(inode->i_mode) && !inode->i_blocks)
		return -EINVAL;
        if(IS_APPEND(inode) || IS_IMMUTABLE(inode))
                return -EPERM;
	if((err = block_truncate_page(inode->i_mapping, size, pfs_get_block)))
		return err;
	truncate_setsize(inode, size);
	__pfs_truncate_blocks(inode);
	return 0;
}

void
pfs_truncate_blocks(struct inode *inode)
{
	if(!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
                return;
	if(S_ISLNK(inode->i_mode) && !inode->i_blocks)
		return;
        if(IS_APPEND(inode) || IS_IMMUTABLE(inode))
                return;
	__pfs_truncate_blocks(inode);
}

struct inode *
pfs_iget(struct super_block *sb, int64_t ino)
{
	int	i;
	struct inode	*inode;
	struct pfs_inode	*ip;
	struct buffer_head	*bh;

	if(!(inode = iget5_locked(sb, (uint32_t)ino, pfs_test, pfs_set, &ino))) 
		return ERR_PTR(-ENOMEM);
	if(!(inode->i_state & I_NEW))
		return inode;
	if(!(bh = sb_bread(sb, ino / PFS_INDS_PER_BLOCK))){	
		pr_warn("pfs: device %s: %s: failed to read inode %lld\n", sb->s_id, "pfs_iget", ino);
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}
	ip = (struct pfs_inode *)bh->b_data + ino % PFS_INDS_PER_BLOCK; 
	inode->i_mode = le32_to_cpu(ip->i_mode);
	i_uid_write(inode, le32_to_cpu(ip->i_uid));
	i_gid_write(inode, le32_to_cpu(ip->i_gid));
	set_nlink(inode, le32_to_cpu(ip->i_nlink));
	inode->i_size = le64_to_cpu(ip->i_size);
	inode->i_blocks = le64_to_cpu(ip->i_blocks);
	inode->i_atime.tv_sec = le64_to_cpu(ip->i_atime);
	inode->i_ctime.tv_sec = le64_to_cpu(ip->i_ctime);
	inode->i_mtime.tv_sec = le64_to_cpu(ip->i_mtime);
	inode->i_atime.tv_nsec = inode->i_ctime.tv_nsec = inode->i_mtime.tv_nsec = 0;	
	if(!(S_ISLNK(inode->i_mode) && !inode->i_blocks)){
		for(i = 0; i < PFS_NADDR; i++)
			PFS_I(inode)->i_addr[i] = le64_to_cpu(ip->i_addr[i]);
	}else 
		memmove(PFS_I(inode)->i_addr, ip->i_addr, sizeof(PFS_I(inode)->i_addr)); 
	pfs_set_inode(inode, new_decode_dev(PFS_I(inode)->i_addr[0]));
	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

int
pfs_free_inode(struct inode *inode)
{
	int	err;
	struct pfs_sb_info *sbi = PFS_SB(inode->i_sb);

	mutex_lock(&sbi->s_lock);
        err = pfs_free(inode->i_sb, PFS_I(inode)->i_ino, PFS_ALLOC_INODE);
	mutex_unlock(&sbi->s_lock);
	return err;
}

struct inode *
pfs_new_inode(struct inode *dir, umode_t mode)
{
	int64_t	ino;
	struct inode *inode;
	struct pfs_sb_info *sbi = PFS_SB(dir->i_sb);

	if(!(inode = new_inode(dir->i_sb)))		
		return ERR_PTR(-ENOMEM);
	mutex_lock(&sbi->s_lock);
	if(!(ino = pfs_alloc(dir->i_sb, PFS_ALLOC_INODE))) 
		goto err;
	inode_init_owner(inode, dir, mode); 
	inode->i_blocks = 0;
	pfs_set(inode, &ino);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	memset(PFS_I(inode)->i_addr, 0, sizeof(PFS_I(inode)->i_addr)); 
	if(insert_inode_locked4(inode, inode->i_ino, pfs_test, &ino) < 0){ 
		pfs_free(dir->i_sb, ino, PFS_ALLOC_INODE); 
		goto err;
	}
	mark_inode_dirty(inode);
	mutex_unlock(&sbi->s_lock);
	return inode;
err:
	mutex_unlock(&sbi->s_lock);
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(-EIO);	
}

int
pfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return pfs_update_inode(inode, wbc);
}

void
pfs_evict_inode(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
	truncate_inode_pages_final(&inode->i_data);
#else
	truncate_inode_pages(&inode->i_data, 0);
#endif
	if(!inode->i_nlink){
		inode->i_size = 0;
		if(inode->i_blocks) 
			pfs_truncate_blocks(inode);
	}
	invalidate_inode_buffers(inode);
	clear_inode(inode);
	if(!inode->i_nlink)
		pfs_free_inode(inode);
}

static void
pfs_write_failed(struct address_space *mapping, loff_t to)
{
        struct inode *inode = mapping->host;

        if(to > inode->i_size){
                truncate_pagecache(inode, inode->i_size);
                pfs_truncate_blocks(inode);
        }
}

static int 
pfs_readpage(struct file *file, struct page *page)
{
        return block_read_full_page(page, pfs_get_block);
}

static int
pfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, pfs_get_block, wbc);
}

static int
pfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned len, unsigned flags,
                struct page **pagep, void **fsdata)
{
        int ret;

        ret = block_write_begin(mapping, pos, len, flags, pagep, pfs_get_block);
        if(unlikely(ret))
                pfs_write_failed(mapping, pos + len);
        return ret;
}

static sector_t
pfs_block_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, pfs_get_block);
}

const struct address_space_operations pfs_aops = {
        .readpage	= pfs_readpage,
        .writepage 	= pfs_writepage,
        .write_begin 	= pfs_write_begin, 
        .write_end 	= generic_write_end,
        .bmap 		= pfs_block_bmap,
};
