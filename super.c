#include	<linux/fs.h>
#include	<linux/slab.h>
#include	<linux/init.h>
#include	<linux/mutex.h>
#include	<linux/module.h>
#include	<linux/printk.h>
#include	<linux/string.h>
#include	<linux/statfs.h>
#include	<linux/buffer_head.h>
#include	"pfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("颜文泽");
MODULE_VERSION("1.0");

static struct kmem_cache *pfs_inode_cachep;

static inline int64_t
pfs_get_blocks(struct pfs_sb_info *sbi)
{
	return le64_to_cpu(sbi->s_spb->s_fsize) - le64_to_cpu(sbi->s_spb->s_isize) - sbi->s_sbh->b_blocknr * PFS_STRS_PER_BLOCK;
}

static int
pfs_recovery(struct super_block *s)
{
	return 0;
}

static int32_t
pfs_get_rsiz(int8_t *vbr)
{
	int32_t	rsiz;

	if((vbr[0] & 0xFF) != 0xEB || (vbr[1] & 0xFF) != 0x2)
		return -1;
	rsiz = (vbr[2] & 0xFF) | ((vbr[3] & 0xFF) << 8);	
	return rsiz;
}

static void
pfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(pfs_inode_cachep, PFS_I(inode));
}

static void
init_once(void *foo)
{
	struct pfs_inode_info *ei = (struct pfs_inode_info *)foo;
	inode_init_once(&ei->vfs_inode);
}

static int
init_inodecache(void)
{
	if(!(pfs_inode_cachep = kmem_cache_create("pfs_inode_cache", 
		sizeof(struct pfs_inode_info), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD), init_once)))
		return -ENOMEM;
	return 0;
}

static void
destroy_inodecache(void)
{
	rcu_barrier();
	kmem_cache_destroy(pfs_inode_cachep);
}

static struct inode *
pfs_alloc_inode(struct super_block *sb)
{
        struct pfs_inode_info   *ei;

        if(!(ei = (struct pfs_inode_info *)kmem_cache_alloc(pfs_inode_cachep, GFP_KERNEL)))
                return NULL;
        return &ei->vfs_inode;
}

static void
pfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, pfs_i_callback);
}

static void
pfs_put_super(struct super_block *sb)
{
	struct pfs_sb_info	*sbi = PFS_SB(sb);
	
	brelse(sbi->s_sbh);
	brelse(sbi->s_ibh);
	brelse(sbi->s_bbh);
	mutex_destroy(&sbi->s_lock);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static int
pfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
        struct super_block      *s = dentry->d_sb;
        struct pfs_sb_info      *sbi = PFS_SB(s);
	u64	id = huge_encode_dev(s->s_bdev->bd_dev);

	mutex_lock(&sbi->s_lock);
	buf->f_type = s->s_magic;	
	buf->f_bsize = s->s_blocksize; 	
	buf->f_blocks = pfs_get_blocks(sbi) / PFS_STRS_PER_BLOCK; 
	buf->f_bfree = buf->f_blocks - le64_to_cpu(sbi->s_spb->s_bsize) / PFS_STRS_PER_BLOCK; 
	buf->f_bavail = buf->f_bfree;	
	buf->f_files = le64_to_cpu(sbi->s_spb->s_ilimit);	
	buf->f_ffree = le64_to_cpu(sbi->s_spb->s_ilimit) - le64_to_cpu(sbi->s_spb->s_iused);	
	buf->f_namelen = PFS_MAXNAMLEN; 
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	mutex_unlock(&sbi->s_lock);
	return 0;	
}

static int
pfs_remount(struct super_block *s, int *flags, char *data)
{
	struct pfs_sb_info	*sbi = PFS_SB(s);

	mutex_lock(&sbi->s_lock);
	sync_filesystem(s); 
	mutex_unlock(&sbi->s_lock);
	return 0;
}

static const struct super_operations pfs_super_ops = {
	.alloc_inode	= pfs_alloc_inode,
	.destroy_inode	= pfs_destroy_inode,
	.write_inode	= pfs_write_inode,
	.evict_inode	= pfs_evict_inode,
	.put_super	= pfs_put_super,
	.statfs		= pfs_statfs,
	.remount_fs	= pfs_remount,
};

static int
pfs_fill_super(struct super_block *s, void *data, int silent)
{
	int	rev;
	int	ret = -EINVAL;
	struct inode	*rootp;
	struct buffer_head	*bh;
	struct pfs_sb_info	*sbi;

	if(!(sbi = kzalloc(sizeof(*sbi), GFP_KERNEL))){ 
		pr_warn("pfs: device %s: %s: out of memory\n", s->s_id, "pfs_fill_super");	
		return -ENOMEM;
	}
	mutex_init(&sbi->s_lock);	
	s->s_fs_info = sbi;
	if(!sb_set_blocksize(s, PFS_BLOCKSIZ)){ 
		pr_warn("pfs: device %s: %s: failed to set block size\n", s->s_id, "pfs_fill_super");
		goto out;
	}
	if(!(bh = sb_bread(s, 0))){ 
		if(!silent)
			pr_warn("pfs: device %s: %s: failed to read VBR\n", s->s_id, "pfs_fill_super");
		goto out;
	}
	if((rev = pfs_get_rsiz((int8_t *)bh->b_data)) < 0) 
		goto out1;
	brelse(bh);
	if(!(bh = sb_bread(s, rev / PFS_STRS_PER_BLOCK))){ 
		if(!silent)
			pr_warn("pfs: device %s: %s: failed to read superblock\n", s->s_id, "pfs_fill_super");
		goto out;
	}
	sbi->s_sbh = bh;
	sbi->s_spb = (struct pfs_super_block *)bh->b_data + rev % PFS_STRS_PER_BLOCK; 
	if(memcmp(sbi->s_spb->s_magic, PFS_MAGIC_STRING, 4)){
		if(!silent)
			pr_warn("pfs: device %s: %s: unknown filesystem on device\n", s->s_id, "pfs_fill_super");
		goto out1;
	}
	s->s_magic = PFS_MAGIC;
	if(!(sbi->s_ibh = sb_bread(s, le64_to_cpu(sbi->s_spb->s_ihead) / PFS_INDS_PER_BLOCK))){
		if(!silent) 
			pr_warn("pfs: device %s: %s: failed to read inode bmap\n", s->s_id, "pfs_fill_super");
		goto out1;
	}
	sbi->s_ifree = (int64_t *)((struct pfs_inode *)sbi->s_ibh->b_data + le64_to_cpu(sbi->s_spb->s_ihead) % PFS_INDS_PER_BLOCK);
	if(!(sbi->s_bbh = sb_bread(s, le64_to_cpu(sbi->s_spb->s_bhead) / PFS_STRS_PER_BLOCK))){
		if(!silent) 
			pr_warn("pfs: device %s: %s: failed to read block bmap\n", s->s_id, "pfs_fill_super");
		goto out2;
	}
	sbi->s_bfree = (int64_t *)sbi->s_bbh->b_data;
	s->s_op = &pfs_super_ops;
	rootp = pfs_iget(s, le64_to_cpu(sbi->s_spb->s_iroot));
	if(IS_ERR(rootp)){
		ret = PTR_ERR(rootp);
		if(!silent)
			pr_warn("pfs: device %s: %s: failed to get root inode\n", s->s_id, "pfs_fill_super");
		goto out3;
	}
	if(!(s->s_root = d_make_root(rootp))){
		ret = -ENOMEM;
		if(!silent)
			pr_warn("pfs: device %s: %s: failed to get root dentry: out of memory\n", s->s_id, "pfs_fill_super");
		goto out3;
	}
	if((s->s_flags & MS_RDONLY) || !pfs_recovery(s)) 
		return 0;
	if(!silent)
		pr_warn("pfs: device %s: %s: failed to recover filesystem\n", s->s_id, "pfs_fill_super");
out3:
	brelse(sbi->s_bbh);
out2:
	brelse(sbi->s_ibh);
out1:
	brelse(bh);
out:
	mutex_destroy(&sbi->s_lock);	
	kfree(sbi);
	s->s_fs_info = NULL;
	return ret;
}

static struct dentry *
pfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
        return mount_bdev(fs_type, flags, dev_name, data, pfs_fill_super); 
}


static struct file_system_type pfs_fs_type = {
	.owner 		= THIS_MODULE,
	.name 		= "pfs",
	.mount 		= pfs_mount,
	.kill_sb 	= kill_block_super,
	.fs_flags 	= FS_REQUIRES_DEV,
};

static int __init
init_pfs_fs(void)
{
	int	err;

	if((err = init_inodecache()))
		return err;
	if((err = register_filesystem(&pfs_fs_type))) 
		destroy_inodecache(); 
	return err;
}

static void __exit
exit_pfs_fs(void)
{
	unregister_filesystem(&pfs_fs_type); 
	destroy_inodecache();
}

module_init(init_pfs_fs);
module_exit(exit_pfs_fs);
