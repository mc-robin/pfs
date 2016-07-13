#include	<linux/fs.h>
#include	<linux/errno.h>
#include	<linux/version.h>
#include	<linux/buffer_head.h>
#include	"pfs.h"

static inline int64_t
pfs_block_number(int64_t offset)
{
	return offset >> PFS_BLOCKSFT;
}

static int
pfs_readdir(struct file *file, struct dir_context *ctx)
{
	int64_t dno;
	unsigned long off;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;
	struct inode *inode = file_inode(file);

	if(ctx->pos == 0) 
		ctx->pos = PFS_DIRHASHSIZ * sizeof(int64_t) + sizeof(int64_t);
	for(off = ctx->pos & (PFS_BLOCKSIZ - 1); ctx->pos < inode->i_size; off = ctx->pos & (PFS_BLOCKSIZ - 1)){
		if(!(dno = pfs_get_block_number(inode, pfs_block_number(ctx->pos), 0))) 
			goto skip;	
		if(!(bh = sb_bread(inode->i_sb, dno / PFS_STRS_PER_BLOCK))){ 
			pr_err("pfs: device %s: %s: failed to read block %lld of dir %lld\n", 
				inode->i_sb->s_id, "pfs_readdir", pfs_block_number(ctx->pos), PFS_I(inode)->i_ino);
			goto skip;
		}
		do{
			de = (struct pfs_dir_entry *)((char *)bh->b_data + off);
			if(de->d_ino){ 
				if(!(dir_emit(ctx, pfs_get_de_name(de), de->d_len, (int32_t)le64_to_cpu(de->d_ino), DT_UNKNOWN))){
					brelse(bh);
					return 0;
				}
			}
			off += pfs_get_de_size(de);
			ctx->pos += pfs_get_de_size(de);
		}while(off < PFS_BLOCKSIZ && ctx->pos < inode->i_size);
		brelse(bh);
		continue;
skip:
		ctx->pos += PFS_BLOCKSIZ - off; 
	}
	return 0;
}

struct pfs_dir_entry *
pfs_find_entry(struct inode *dir, const struct qstr *qstr, int (*test)(const void *, const void *), 
	struct pfs_dir_hash_info *hdp, struct pfs_dir_hash_info *hdp1)
{
	int64_t	off;
	int64_t	dno;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;
	struct super_block *sb = dir->i_sb;

	get_bh(hdp->bh); 
	hdp1->bh = NULL; 
	for(off = le64_to_cpu(*hdp->p); off; off = pfs_get_de_offset(de)){ 
		if(hdp1->bh) 
			brelse(hdp1->bh);
		pfs_add_hdentry(hdp1, hdp->p, hdp->off, hdp->bh); 
		hdp->bh = NULL;
		if(pfs_block_number(hdp1->off) != pfs_block_number(off)){ 
			if(!(dno = pfs_get_block_number(dir, pfs_block_number(off), 0)))
				goto out;
			if(!(bh = sb_bread(sb, dno / PFS_STRS_PER_BLOCK))) 
				goto out;
		}else{ 
			bh = hdp1->bh;
			get_bh(bh); 
		}
		de = (struct pfs_dir_entry *)((char *)bh->b_data + off % PFS_BLOCKSIZ);
		pfs_add_hdentry(hdp, &de->d_next, off, bh);
		if(test(qstr, de))
			break;
	}
	if(!off)
		return NULL;
	return de;
out:
	return NULL;
}

int
pfs_make_empty(struct inode *inode)
{
	int64_t	dno;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;

	if(!(dno = pfs_alloc(inode->i_sb, PFS_ALLOC_BLOCK)))
		return -ENOSPC;
	if(!(bh = sb_bread(inode->i_sb, dno / PFS_STRS_PER_BLOCK))){
		pfs_free(inode->i_sb, dno, PFS_ALLOC_BLOCK);
		return -EIO;
	}
	memset(bh->b_data, 0, PFS_BLOCKSIZ);
	de = (struct pfs_dir_entry *)((char *)bh->b_data + PFS_DIRHASH_UNUSED * sizeof(int64_t) + sizeof(int64_t)); 
	de->d_len = 1;
	de->d_reclen = cpu_to_le16(sizeof(*de));
	de->d_ino = cpu_to_le64(PFS_I(inode)->i_ino);
	strcpy(de->d_name, "."); 
	de = (struct pfs_dir_entry *)((char *)de + sizeof(*de)); 
	de->d_len = 2;
	de->d_reclen = cpu_to_le16(sizeof(*de));
	de->d_ino = cpu_to_le64(PFS_I(inode)->i_ino);
	strcpy(de->d_name, "..");
	PFS_I(inode)->i_addr[0] = dno; 
	truncate_setsize(inode, PFS_BLOCKSIZ);
	mark_inode_dirty(inode);
	mark_buffer_dirty_inode(bh, inode);
	brelse(bh);
	return 0;
}

int
pfs_empty_dir(struct inode *dir)
{
	int	i;
	int64_t	dno;
	struct buffer_head *bh;

	if(!(dno = pfs_get_block_number(dir, 0, 0))) 
                return 0;
        if(!(bh = sb_bread(dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
                return 0;
	for(i = 0; i < PFS_DIRHASHSIZ; i++){ 
		if(((int64_t *)bh->b_data)[i])
			break;
	}
	brelse(bh);
	return i == PFS_DIRHASHSIZ; 
}

int64_t
pfs_inode_by_name(struct inode *dir, const struct qstr *qstr)
{
	int64_t	ino;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;
	struct pfs_dir_hash_info hd, hd1; 
	
	if(strcmp(qstr->name, ".") == 0) 
		return PFS_I(dir)->i_ino;
	if(!(ino = pfs_get_block_number(dir, 0, 0))) 
		return 0;
	if(!(bh = sb_bread(dir->i_sb, ino / PFS_STRS_PER_BLOCK))) 
		return 0;
	ino = 0; 
	if(strcmp(qstr->name, "..") == 0){ 
		de = (struct pfs_dir_entry *)((char *)bh->b_data +  
			PFS_DIRHASH_UNUSED * sizeof(int64_t) + sizeof(int64_t) + sizeof(*de)); 
		ino = le64_to_cpu(de->d_ino); 
		brelse(bh);
		return ino;
	}
	pfs_add_hdentry(&hd, (int64_t *)((char *)bh->b_data + pfs_hash(qstr->name) * sizeof(int64_t)), 0, bh); 
	if((de = pfs_find_entry(dir, qstr, pfs_match, &hd, &hd1))) 
		ino = le64_to_cpu(de->d_ino);
	if(hd.bh)
		brelse(hd.bh);
	if(hd1.bh)
		brelse(hd1.bh);
	brelse(bh);
	return ino;
}

/*
 * it's too bad
 */
int
pfs_add_link(struct dentry *dentry, struct inode *inode)
{
	int	err;
	int64_t	dno;
	int	hashval;
	int	left, reclen;
	struct buffer_head *bh;
	struct pfs_dir_entry *de;
        struct pfs_dir_hash_info hd, hd1;
	const struct qstr *qstr = &dentry->d_name;	
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
	struct inode *dir = d_inode(dentry->d_parent);
#else
	struct inode *dir = dentry->d_parent->d_inode;
#endif

	err = -EIO;
        if(!(dno = pfs_get_block_number(dir, 0, 0))) 
                return err;
        if(!(bh = sb_bread(dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
                return err;
	hashval = pfs_hash(qstr->name); 
        pfs_add_hdentry(&hd, (int64_t *)((char *)bh->b_data + PFS_DIRHASH_UNUSED * sizeof(int64_t)), 0, bh); 
        if((de = pfs_find_entry(dir, qstr, pfs_find_empty_entry, &hd, &hd1))){ 
		*(hd1.p) = *(hd.p); 
		mark_buffer_dirty_inode(hd1.bh, dir);
		*(hd.p) = ((int64_t *)bh->b_data)[hashval]; 
        	((int64_t *)bh->b_data)[hashval] = cpu_to_le64(hd.off); 
		mark_buffer_dirty_inode(bh, dir);
		de->d_len = qstr->len; 
		de->d_ino = cpu_to_le64(PFS_I(inode)->i_ino);
		memmove(pfs_get_de_name(de), qstr->name, qstr->len + 1); 
		mark_buffer_dirty_inode(hd.bh, dir); 
        	dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
        	mark_inode_dirty(dir);
		goto out;
	} 
expand:
	hd.bh = hd1.bh = NULL;
	reclen = pfs_get_reclen(qstr->len);
	left = dir->i_size % PFS_BLOCKSIZ;
	left = left ? PFS_BLOCKSIZ - left : left; 
	if(left){ 
		if(!(dno = pfs_get_block_number(dir, pfs_block_number(dir->i_size), 0))) 
			goto out1;
		if(!(hd.bh = sb_bread(dir->i_sb, dno / PFS_STRS_PER_BLOCK))) 
			goto out1;
add_dentry: 
		de = (struct pfs_dir_entry *)((char *)hd.bh->b_data + dir->i_size % PFS_BLOCKSIZ); 
		pfs_add_hdentry(&hd, &de->d_next, dir->i_size, hd.bh);
		if(left >= reclen){ 
			de->d_len = qstr->len;
			de->d_ino = cpu_to_le64(PFS_I(inode)->i_ino);
			memmove(pfs_get_de_name(de), qstr->name, qstr->len + 1);
			de->d_reclen = cpu_to_le16(left - reclen >= sizeof(*de) ? reclen : left);
			*(hd.p) = ((int64_t *)bh->b_data)[hashval]; 
                	((int64_t *)bh->b_data)[hashval] = cpu_to_le64(hd.off); 
		}else{ 
			de->d_ino = 0; 
			de->d_reclen = cpu_to_le16(left); 
			*(hd.p) = ((int64_t *)bh->b_data)[PFS_DIRHASH_UNUSED]; 
			((int64_t *)bh->b_data)[PFS_DIRHASH_UNUSED] = cpu_to_le64(hd.off);
		}
		mark_buffer_dirty_inode(bh, dir);
		mark_buffer_dirty_inode(hd.bh, dir);
		dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
		truncate_setsize(dir, dir->i_size + le16_to_cpu(de->d_reclen));
		mark_inode_dirty(dir);
		if(left >= reclen) 
			goto out;
		brelse(hd.bh);
	}
	if(!(dno = pfs_get_block_number(dir, pfs_block_number(dir->i_size), 1))) 
		goto out1;
	if(!(hd.bh = sb_bread(dir->i_sb, dno / PFS_STRS_PER_BLOCK)))
		goto out1;
	left = reclen + sizeof(*de); 
	goto add_dentry;
out:
	err = 0;
        if(hd.bh)
                brelse(hd.bh);
        if(hd1.bh)
                brelse(hd1.bh);
        if(!de)
                goto expand;
out1:
        brelse(bh);
        return err;
}

int
pfs_delete_entry(struct inode *dir, struct pfs_dir_entry *de, struct buffer_head *bh, 
	struct pfs_dir_hash_info *hdp, struct pfs_dir_hash_info *hdp1)
{
	*(hdp1->p) = *(hdp->p); 
	mark_buffer_dirty_inode(hdp1->bh, dir);
	if(hdp->off + pfs_get_de_size(de) == dir->i_size){ 
		pfs_truncate(dir, dir->i_size - pfs_get_de_size(de)); 
		goto out;
	}
	*(hdp->p) = ((int64_t *)bh->b_data)[PFS_DIRHASH_UNUSED]; 
	((int64_t *)bh->b_data)[PFS_DIRHASH_UNUSED] = cpu_to_le64(hdp->off); 
	mark_buffer_dirty_inode(bh, dir); 
	de->d_ino = 0;
	mark_buffer_dirty_inode(hdp->bh, dir); 
out:
	dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(dir);
	return 0;
}

const struct file_operations pfs_dir_operations = {
	.read		= generic_read_dir,
	.iterate	= pfs_readdir,
	.fsync		= generic_file_fsync,
	.llseek		= generic_file_llseek,
};
