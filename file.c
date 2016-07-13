#include	<linux/fs.h>
#include	<linux/version.h>
#include	"pfs.h"


/*
 * we have mostly NULLs here: the current defaults are ok for the pfs filesystem
 */
const struct file_operations pfs_file_operations = {
        .llseek         = generic_file_llseek,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        .read_iter      = generic_file_read_iter,
#else
	.read		= do_sync_read,	
	.aio_read	= generic_file_aio_read,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        .write_iter     = generic_file_write_iter,
#else
	.write		= do_sync_write,
	.aio_write	= generic_file_aio_write,
#endif
        .mmap           = generic_file_mmap,
	.open 		= generic_file_open,
        .fsync          = generic_file_fsync,
        .splice_read    = generic_file_splice_read,
};

static int
pfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	int	err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        struct inode *inode = d_inode(dentry);
#else
        struct inode *inode = dentry->d_inode;
#endif

	if((err = inode_change_ok(inode, attr)))
		return err;
	if(attr->ia_valid & ATTR_SIZE && attr->ia_size != inode->i_size){
		if((err = pfs_truncate(inode, attr->ia_size)))
			return err;
	}
	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations pfs_file_inode_operations = {
	.setattr = pfs_setattr,
};
