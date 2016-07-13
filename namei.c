#include	<linux/fs.h>
#include	<linux/version.h>
#include	<linux/buffer_head.h>
#include	"pfs.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
extern const struct inode_operations simple_symlink_inode_operations;
#endif

static inline int
pfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int	err;

	if(!(err = pfs_add_link(dentry, inode))){
		unlock_new_inode(inode);
		d_instantiate(dentry, inode);
		return err;
	}	
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

static struct dentry *
pfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	int64_t	ino;
	struct inode *inode;

	if(dentry->d_name.len > PFS_MAXNAMLEN)
		return ERR_PTR(-ENAMETOOLONG);
	inode = NULL;
	if((ino = pfs_inode_by_name(dir, &dentry->d_name)) > 0){
		inode = pfs_iget(dir->i_sb, ino);
		if(IS_ERR(inode))
			return ERR_CAST(inode);
	}
	return d_splice_alias(inode, dentry);
}

static int
pfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;

	inode = pfs_new_inode(dir, mode);
	if(!IS_ERR(inode)){
		pfs_set_inode(inode, rdev);
		mark_inode_dirty(inode);
		return pfs_add_nondir(dentry, inode);
	}
	return PTR_ERR(inode);
}

static int
pfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool execl)
{
	return pfs_mknod(dir, dentry, mode, 0);
}

static int
pfs_tmpfile(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;	

	inode = pfs_new_inode(dir, mode);
	if(!IS_ERR(inode)){
		pfs_set_inode(inode, 0);
		mark_inode_dirty(inode);
		d_tmpfile(dentry, inode);
		unlock_new_inode(inode);
		return 0;
	}
	return PTR_ERR(inode);
}

static int
pfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int	len = strlen(symname) + 1;

	if(len > dir->i_sb->s_blocksize)
		return -ENAMETOOLONG;
	inode = pfs_new_inode(dir, S_IFLNK | S_IRWXUGO);
	if(!IS_ERR(inode)){
		if(len > sizeof(PFS_I(inode)->i_addr)){
			int	err;
			inode->i_op = &page_symlink_inode_operations;	
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
			inode_nohighmem(inode);
#endif
			inode->i_mapping->a_ops = &pfs_aops;
			if((err = page_symlink(inode, symname, len))){
				inode_dec_link_count(inode);
				unlock_new_inode(inode);
				iput(inode);
				return err;
			}	
		}else{ 
			inode->i_op = &simple_symlink_inode_operations;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
			inode->i_link = (char *)PFS_I(inode)->i_addr;
			memcpy(inode->i_link, symname, len);
#else
			memcpy((char *)PFS_I(inode)->i_addr, symname, len);
#endif
			inode->i_size = len - 1;
		}
		mark_inode_dirty(inode);
		return pfs_add_nondir(dentry, inode);
	}
	return PTR_ERR(inode);
}

static int
pfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	int	err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *inode = d_inode(old_dentry);
#else
        struct inode *inode = old_dentry->d_inode;
#endif

	inode->i_ctime = CURRENT_TIME_SEC;
	inode_inc_link_count(inode);
	ihold(inode);
	if((err = pfs_add_link(dentry, inode))){
		inode_dec_link_count(inode);
		iput(inode);
	}else
		d_instantiate(dentry, inode);	
	return err;
}

static int
pfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int	err;
	int64_t	dno;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;
	struct pfs_dir_hash_info hd, hd1;
	const struct qstr *qstr = &dentry->d_name;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *inode = d_inode(dentry);
#else
        struct inode *inode = dentry->d_inode;
#endif
	err = -ENOENT;
	if(!(dno = pfs_get_block_number(dir, 0, 0))) 
                return -EIO;
        if(!(bh = sb_bread(dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
                return -EIO;
        pfs_add_hdentry(&hd, (int64_t *)((char *)bh->b_data + pfs_hash(qstr->name) * sizeof(int64_t)), 0, bh); 
        if(!(de = pfs_find_entry(dir, qstr, pfs_match, &hd, &hd1))) 
		goto out;
	if((err = pfs_delete_entry(dir, de, bh, &hd, &hd1)))
		goto out;
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode); 
out:
	if(hd.bh)
                brelse(hd.bh);
        if(hd1.bh)
                brelse(hd1.bh);
        brelse(bh);
        return err;
}

static int
pfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct inode *inode;

	inode_inc_link_count(dir);
	inode = pfs_new_inode(dir, S_IFDIR | mode);
	err = PTR_ERR(inode);
	if(IS_ERR(inode))
		goto out_dir;
	pfs_set_inode(inode, 0);
	inode_inc_link_count(inode);
	if((err = pfs_make_empty(inode)))
		goto out_fail;
	if((err = pfs_add_link(dentry, inode)))
		goto out_fail;
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	return err;
out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);
out_dir:
	inode_dec_link_count(dir);
	return err;
}

static int
pfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int 	err = -ENOTEMPTY;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *inode = d_inode(dentry);
#else
        struct inode *inode = dentry->d_inode;
#endif

	if(pfs_empty_dir(inode)){
		if(!(err = pfs_unlink(dir, dentry))){
			inode->i_size = 0;
			inode_dec_link_count(dir);
			inode_dec_link_count(inode);
		}
	}
	return err;
}

static int
pfs_rename(struct inode *old_dir, struct dentry *old_dentry,  struct inode *new_dir, struct dentry *new_dentry)
{
	int	err;
	int64_t	dno;
	const struct qstr *qstr;
	struct buffer_head *old_bh; 
	struct buffer_head *new_bh; 
	struct buffer_head *dir_bh;
	struct pfs_dir_entry *old_de;	
	struct pfs_dir_entry *new_de;
	struct pfs_dir_entry *dir_de = NULL;
	struct pfs_dir_hash_info old_hd, old_hd1;
	struct pfs_dir_hash_info new_hd, new_hd1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *old_inode = d_inode(old_dentry);
#else
        struct inode *old_inode = old_dentry->d_inode;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *new_inode = d_inode(new_dentry);
#else
        struct inode *new_inode = new_dentry->d_inode;
#endif
	
	err = -EIO;
	dir_bh = old_bh = new_bh = old_hd.bh = old_hd1.bh = new_hd.bh = new_hd1.bh = NULL;
        if(!(dno = pfs_get_block_number(old_dir, 0, 0))) 
		goto out;
        if(!(old_bh = sb_bread(old_dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
		goto out;
	err = -ENOENT;
	qstr = &old_dentry->d_name;
        pfs_add_hdentry(&old_hd, (int64_t *)((char *)old_bh->b_data + pfs_hash(qstr->name) * sizeof(int64_t)), 0, old_bh);
        if(!(old_de = pfs_find_entry(old_dir, qstr, pfs_match, &old_hd, &old_hd1))) 
                goto out;
	if(S_ISDIR(old_inode->i_mode)){
                err = - EIO;
                if(!(dno = pfs_get_block_number(old_inode, 0, 0)))
                        goto out;
                if(!(dir_bh = sb_bread(old_inode->i_sb, dno / PFS_STRS_PER_BLOCK)))
                        goto out;
                dir_de = (struct pfs_dir_entry *)((char *)dir_bh->b_data +
                        PFS_DIRHASH_UNUSED * sizeof(int64_t) + sizeof(int64_t) + sizeof(*dir_de));
        }
	if(new_inode){
		err = -ENOTEMPTY;
		if(dir_de && !pfs_empty_dir(new_inode)) 
			goto out;
		err = -EIO;
		if(!(dno = pfs_get_block_number(new_dir, 0, 0))) 
                	goto out;
        	if(!(new_bh = sb_bread(new_dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
                	goto out;
		err = -ENOENT;
		qstr = &new_dentry->d_name;
		pfs_add_hdentry(&new_hd, (int64_t *)((char *)new_bh->b_data + pfs_hash(qstr->name) * sizeof(int64_t)), 0, new_bh);
		if(!(new_de = pfs_find_entry(new_dir, qstr, pfs_match, &new_hd, &new_hd1)))
			goto out;
		new_de->d_ino = old_de->d_ino; 
		new_dir->i_ctime = new_dir->i_mtime = CURRENT_TIME_SEC;
		mark_inode_dirty(new_dir);
		new_inode->i_ctime = CURRENT_TIME_SEC;
		if(dir_de) 
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	}else{
		if((err = pfs_add_link(new_dentry, old_inode)))
			goto out;
		if(dir_de) 
			inode_inc_link_count(new_dir);
	}
	err = 0;
	old_inode->i_ctime = CURRENT_TIME_SEC;
	pfs_delete_entry(old_dir, old_de, old_bh, &old_hd, &old_hd1);
	mark_inode_dirty(old_inode);
	if(dir_de){ 
		if(old_dir != new_dir){ 
			dir_de->d_ino = cpu_to_le64(PFS_I(new_dir)->i_ino); 
			mark_buffer_dirty_inode(dir_bh, old_inode);
		}
		inode_dec_link_count(old_dir); 
	}
out:
	if(old_hd.bh)
		brelse(old_hd.bh);
	if(old_hd1.bh)
		brelse(old_hd1.bh);
	if(new_hd.bh)
		brelse(new_hd.bh);
	if(new_hd1.bh)
		brelse(new_hd1.bh);
	if(old_bh)
		brelse(old_bh);
	if(new_bh)
		brelse(new_bh);
	if(dir_bh)
		brelse(dir_bh);
	return err;
}

const struct inode_operations pfs_dir_inode_operations = {
        .create         = pfs_create,
        .lookup         = pfs_lookup,
        .link           = pfs_link,
        .unlink         = pfs_unlink,
	.symlink	= pfs_symlink,
	.mkdir		= pfs_mkdir,
	.rmdir		= pfs_rmdir,
	.mknod 		= pfs_mknod,
        .rename         = pfs_rename,
	.tmpfile	= pfs_tmpfile,
};
