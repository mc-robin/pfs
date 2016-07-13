#ifndef __LINUX_PFS_FS_H
#define __LINUX_PFS_FS_H

#define PFS_MAXNAMLEN	255
#define PFS_BLOCKSFT	12	
#define PFS_SECTORSIZ	512
#define PFS_BLOCKSIZ	4096

#define PFS_ININODES	64	
#define PFS_INBLOCKS	512	

#define PFS_NEXT	2	
#define PFS_NADDR	48	

#define PFS_EXT_BLOCK	2	
#define PFS_D_BLOCK	20ULL	
#define PFS_IND_BLOCK	16ULL	
	#define PFS_IND_BLOCKS	(1ULL << 9) 
#define PFS_DIND_BLOCK	4ULL	
	#define PFS_DIND_BLOCKS	(1ULL << 18) 
#define PFS_TIND_BLOCK	4ULL	
	#define PFS_TIND_BLOCKS	(1ULL << 27) 
#define PFS_QIND_BLOCK	4ULL	
	#define PFS_QIND_BLOCKS	(1ULL << 36) 

#define PFS_MININODES	1024	
#define PFS_MINSECTORS	32768 	

#define PFS_STRS_PER_BLOCK	8	
#define PFS_INDS_PER_BLOCK 	PFS_STRS_PER_BLOCK 

#define PFS_MAGIC	0x50465331
#define PFS_MAGIC_STRING	"PFS1" 

#define PFS_DIRHASHSIZ	(((PFS_BLOCKSIZ - 2 * sizeof(struct pfs_dir_entry)) / 8) - 1)
#define PFS_DIRHASH_UNUSED	PFS_DIRHASHSIZ

#define PFS_MAXBLOCKS		0x100000000ULL		
#define PFS_MAXFILESIZ		0x100000000000ULL 	

struct pfs_super_block{ 	
	int32_t	s_rev;
	int64_t	s_icnt;
	int64_t	s_bcnt;
	int64_t	s_utime;	
	int64_t	s_fsize;	
	int64_t	s_iused;	
	int64_t	s_isize;	
	int64_t	s_bsize;	
	int64_t	s_iroot;	
	int64_t	s_bhead;
	int64_t	s_ihead;
	int64_t	s_ilimit;	
	char	s_magic[4];
	char	s_depend[416];
};

struct pfs_inode{	
        int32_t i_uid; 
        int32_t i_gid;
        int32_t i_mode;
	int32_t	i_esiz;		
        int32_t i_nlink;
	int64_t	i_blocks;	
        int64_t i_size;
        int64_t i_atime;
        int64_t i_mtime;
        int64_t i_ctime;
	int64_t	i_otime;
        int64_t	i_ext[PFS_NEXT];   	
        int64_t i_addr[PFS_NADDR]; 
        char	i_pad[42];
};

#define PFS_DIR_RECLEN     (sizeof(struct pfs_dir_entry) - (int)((struct pfs_dir_entry *)0)->d_name) 
struct pfs_dir_entry{	
	int64_t	d_ino; 	
	int64_t	d_next; 
	int16_t	d_reclen; 
	uint8_t	d_len; 	
	char	d_name[5];
};

static inline int
pfs_hash(const char *str)
{
	uint32_t	hash;

	if(!str) 
		return 0;
	for(hash = 0; *str; str++)
		hash = *str + (hash << 6) + (hash << 16) - hash;
	return hash % PFS_DIRHASHSIZ;
}

#endif
