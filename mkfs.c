#define	_BSD_SOURCE
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#include	<time.h>
#include	<fcntl.h>
#include	<errno.h>
#include	<stdio.h>
#include	<endian.h>
#include	<unistd.h>
#include	<string.h>
#include	<stdint.h>
#include	<sys/stat.h>
#include	"pfs_fs.h"

static int32_t
pfs_bread(int fd, int64_t bno, void *buf, int32_t cnt)
{
	if(cnt < 0 || buf == NULL)
		return -1;
	if(bno < 0 || bno * PFS_SECTORSIZ < 0) 
		return -1;
	if(lseek64(fd, bno * PFS_SECTORSIZ, SEEK_SET) != bno * PFS_SECTORSIZ)
		return -1;
	if(cnt && read(fd, buf, cnt * PFS_SECTORSIZ) != cnt * PFS_SECTORSIZ)
		return -1;
	return 0;
}

static int32_t
pfs_bwrite_nolseek(int fd, int64_t bno, const void *buf, int32_t cnt)
{
        if(cnt < 0 || buf == NULL)
                return -1;
        if(bno < 0 || bno * PFS_SECTORSIZ < 0) 
                return -1;
        if(cnt && write(fd, buf, cnt * PFS_SECTORSIZ) != cnt * PFS_SECTORSIZ)
                return -1;
        return 0;
}

static int32_t
pfs_bwrite(int fd, int64_t bno, const void *buf, int32_t cnt)
{
	if(cnt < 0 || buf == NULL)
                return -1;
	if(bno < 0 || bno * PFS_SECTORSIZ < 0) 
		return -1;
	if(lseek64(fd, bno * PFS_SECTORSIZ, SEEK_SET) != bno * PFS_SECTORSIZ)
		return -1;
	if(cnt && write(fd, buf, cnt * PFS_SECTORSIZ) != cnt * PFS_SECTORSIZ)
		return -1;
	return 0;
}

static int64_t
pfs_get_size(int fd)
{
        return lseek64(fd, 0, SEEK_END);
}

static int32_t
pfs_type_check(const char *image)
{
        struct stat64 stbuf;

        if(stat64(image, &stbuf) == -1) 
                return -1;
        if(!(S_ISREG(stbuf.st_mode) || S_ISBLK(stbuf.st_mode))) 
                return -1;
        return 0;
}

static int
set_reservedsectors(int fd, int32_t rsiz, int64_t bno, struct pfs_super_block *sp)
{
	int8_t	vbr[PFS_SECTORSIZ];

	if(pfs_bread(fd, bno, vbr, 1) == -1) 
                return -1;
        vbr[0] = 0xEB;  
        vbr[1] = 0x02;  
        vbr[2] = (int8_t)(rsiz & 0xFF); 
        vbr[3] = (int8_t)((rsiz >> 8) & 0xFF);
        if(pfs_bwrite(fd, bno, vbr, 1) == -1) 
                return -1;
	memmove(sp, vbr, 4);
	return 0;	
}

static int
set_blocklist(int fd, int64_t bhead, int64_t end)
{
	int	i, j;
        int64_t	n, m, buf[PFS_INBLOCKS];
	
	if(lseek64(fd, bhead * PFS_SECTORSIZ, SEEK_SET) != bhead * PFS_SECTORSIZ)
                return -1;
	n = (end / PFS_BLOCKSIZ) + (end % PFS_BLOCKSIZ ? 1 : 0);
        m = bhead + n * PFS_STRS_PER_BLOCK + PFS_STRS_PER_BLOCK;
	for(i = 0; i < n; i++){
		buf[0] = (int64_t)htole64(bhead + PFS_STRS_PER_BLOCK);
                if(m + PFS_STRS_PER_BLOCK * (PFS_INBLOCKS - 1) < end){
                        for(j = PFS_INBLOCKS -1; j > 0; j--, m += PFS_STRS_PER_BLOCK)
                                buf[j] = (int64_t)htole64(m);
                }else{
                        for(j = 1; j < PFS_INBLOCKS && m < end; j++, m += PFS_STRS_PER_BLOCK)
                                buf[j] = (int64_t)htole64(m);
                        while(j < PFS_INBLOCKS)
                                buf[j++] = 0;
                }
		bhead += PFS_STRS_PER_BLOCK;
		if(pfs_bwrite_nolseek(fd, bhead, buf, PFS_STRS_PER_BLOCK) == -1)
			return -1;
	}
	memset(buf, 0, sizeof(buf));
        if(pfs_bwrite_nolseek(fd, bhead, buf, PFS_STRS_PER_BLOCK) == -1)
                return -1;
        return 0;
}

static int
creat_root(int fd, int64_t ino, int64_t dno)
{
        struct pfs_inode   root;
	int64_t	buf[PFS_INBLOCKS];	
	struct pfs_dir_entry	dbuf[2];

        memset(&root, 0, sizeof(root));
        root.i_addr[0] = (int64_t)htole64(dno); 
        root.i_mode = (int32_t)htole32(040777); 
        root.i_nlink = (int32_t)htole32(2); 
	root.i_blocks = (int64_t)htole64(1); 
	root.i_size = (int64_t)htole64(PFS_BLOCKSIZ); 
        root.i_atime = root.i_mtime = root.i_ctime = (int64_t)htole64((int64_t)time(NULL));
        dbuf[0].d_len = 1; 
        dbuf[1].d_len = 2; 
	dbuf[0].d_next = dbuf[1].d_next = 0; 
	dbuf[0].d_reclen = dbuf[1].d_reclen = (int16_t)htole16(sizeof(dbuf[0])); 
        dbuf[0].d_ino = dbuf[1].d_ino = (int64_t)htole64(ino); 
        memmove(dbuf[0].d_name, ".", 2);
        memmove(dbuf[1].d_name, "..", 3);
	memset(buf, 0, PFS_BLOCKSIZ);
	memmove(buf + PFS_DIRHASH_UNUSED + 1, dbuf, sizeof(dbuf));
	if(pfs_bwrite(fd, dno, buf, PFS_STRS_PER_BLOCK) == -1) 
		return -1;
	if(pfs_bwrite(fd, ino, &root, 1) == -1) 
		return -1;
        return 0;
}

int
main(int argc, char *argv[])
{
	int i, fd;
	int32_t	rsiz;
	int64_t	buf[PFS_ININODES];
	struct pfs_super_block	spb;
	int64_t root, start, end, fsiz;
	
	rsiz = 0; 
	while(--argc > 4){ 
		if((*++argv)[0] == '-'){
			switch(*++argv[0]){ 
			case 'r':
				--argc; 
				if(strpbrk(*++argv, "0123456789") == NULL || (rsiz = strtol(*argv, NULL, 0)) < 0
					|| rsiz > UINT16_MAX){
					printf("mkfs: wrong reserved sectors '%s'\n", *argv);
					return -1;
				}
				break;
			default:
				break;
			}
		}
	}
	if(argc < 4){ 
		printf("mkfs: usage: mkfs -r reserved-sectors start-sector sector-numbers inode-limit image-name\n"); 
		return -1;
	}
	memset(&spb, 0, sizeof(spb)); 
	if(strpbrk(*++argv, "0123456789") == NULL ||  
		(start = strtoll(*argv, NULL, 0)) < 0){
		printf("mkfs: wrong start sector '%s'\n", *argv);
		return -1;
	}
	if(strpbrk(*++argv, "0123456789") == NULL ||  
		(end = strtoll(*argv, NULL, 0)) < PFS_MINSECTORS){
		printf("mkfs: sector numbers '%s' too small: at least %d\n", *argv, PFS_MINSECTORS);
		return -1;
	}
	if(strpbrk(*++argv, "0123456789") == NULL || 
		(spb.s_ilimit = strtoll(*argv, NULL, 0)) < PFS_MININODES){ 
		printf("mkfs: inode numbers '%s' too small: at least %d\n", *argv, PFS_MININODES);
		return -1;
	}
	if((end += start) < start){ 
		printf("mkfs: start-sector + sector-numbers overflow\n");
		return -1;
	}
	if(pfs_type_check(*++argv) == -1){ 
		printf("mkfs: regular file or block device only\n");
		return -1;
	}
	if((fd = open(*argv, O_RDWR)) < 0){ 
		printf("mkfs: fail to open '%s': %s\n", *argv, strerror(errno));
		return -1;
	}
	if((fsiz = pfs_get_size(fd)) < 0){ 
		close(fd);
		printf("mkfs: fail to get size of '%s': %s\n", *argv, strerror(errno));
		return -1;
	}
	if(end > (fsiz /= PFS_SECTORSIZ)){ 
		close(fd);
		printf("mkfs: start-sector + sector-numbers > image-sectors\n");
		return -1;
	}
	if(set_reservedsectors(fd, rsiz, start, &spb) == -1){ 
		close(fd);
		printf("mkfs: failed to set reserved sectors\n");
		return -1;
	}
	root = rsiz + start + 1;
	root = ((root + PFS_STRS_PER_BLOCK - 1) / PFS_STRS_PER_BLOCK) * PFS_STRS_PER_BLOCK; 
	spb.s_fsize = (int64_t)htole64(fsiz);
	spb.s_isize = (int64_t)htole64(PFS_INDS_PER_BLOCK); 
	spb.s_bsize = (int64_t)htole64(PFS_STRS_PER_BLOCK);	
	memmove(spb.s_magic, PFS_MAGIC_STRING, 4);
	spb.s_iused = (int64_t)htole64(2);
	spb.s_iroot = (int64_t)htole64(root); 
	spb.s_icnt = (int64_t)htole64(PFS_INDS_PER_BLOCK - 2);     
	spb.s_ihead = (int64_t)htole64(root + 1); 
	spb.s_bcnt = (int64_t)htole64(PFS_INBLOCKS);
	spb.s_bhead = (int64_t)htole64(root + 2 * PFS_STRS_PER_BLOCK); 
	memset(buf, 0, sizeof(buf));
	for(i = 2; i < PFS_INDS_PER_BLOCK; i++)
		buf[i - 2] = (int64_t)htole64(root + i);
	if(pfs_bwrite(fd, root + 1, buf, 1) == -1){ 
		close(fd);
		printf("mkfs: failed to init inode map\n"); 
		return -1;
	}
	memset(buf, 0, sizeof(buf));
        if(pfs_bwrite(fd, root + 2, buf, 1) == -1){ /* clear */
                close(fd);
                printf("mkfs: failed to init inode map\n");
                return -1;
        }
	if(set_blocklist(fd, root + 2 * PFS_STRS_PER_BLOCK, end) == -1){ 
		close(fd);
		printf("mkfs: failed to init block map\n");
		return -1;
	} 
	if(creat_root(fd, root, root + PFS_STRS_PER_BLOCK) == -1){ 
		close(fd);
		printf("mkfs: failed to creat root directory\n");
		return -1;
	}
	spb.s_utime = (int64_t)htole64((int64_t)time(NULL));
	if(pfs_bwrite(fd, rsiz + start, &spb, 1) == -1){ 
		close(fd);
		printf("mkfs: failed to update super block\n");
		return -1;
	}
	close(fd);
	return 0;
}
