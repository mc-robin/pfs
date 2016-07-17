#include	<linux/mutex.h>
#include	<linux/string.h>
#include	<linux/buffer_head.h>
#include	"pfs.h"

/*
 * both block and inode
 */
static int64_t
pfs_alloc0(struct super_block *sb, int type, int64_t *cntp, int64_t *headp,
        struct buffer_head **bhp, int64_t **freep)
{
	int64_t	tm, dno;
	struct buffer_head *bh;
        int64_t cnt = le64_to_cpu(*cntp);
        struct pfs_sb_info *sbi = PFS_SB(sb);

	if(!cnt){ 
		int	i;
		int64_t	isize;

		if(type) 
			return 0;
		isize = le64_to_cpu(sbi->s_spb->s_isize);
		if(isize > le64_to_cpu(sbi->s_spb->s_ilimit)) 
			return 0;
		if(!(dno = pfs_alloc(sb, PFS_ALLOC_BLOCK)))
			return 0;
		if(pfs_clear_block(sb, dno, PFS_SECTORSIZ)){
			pfs_free(sb, dno, PFS_ALLOC_BLOCK);
			return 0;
		}
		cnt = PFS_INDS_PER_BLOCK;
		isize += PFS_INDS_PER_BLOCK;
		sbi->s_spb->s_isize = cpu_to_le64(isize);
		for(i = 0; i < cnt; i++)
			(*freep)[i] = cpu_to_le64(dno + i);
		mark_buffer_dirty(*bhp); 
	}
	switch(cnt){
	case 1:
		tm = le64_to_cpu((*freep)[0]);
		if(!(bh = sb_bread(sb, tm / PFS_INDS_PER_BLOCK)))
			return 0;
		bforget(*bhp);
		*bhp = bh;
		dno = le64_to_cpu(*headp);
		*headp = cpu_to_le64(tm);
		*freep = type ? (int64_t *)bh->b_data : (int64_t *)((struct pfs_inode *)bh->b_data + tm % PFS_INDS_PER_BLOCK);
		for(cnt = 0; cnt < (type ? PFS_INBLOCKS : PFS_ININODES) && (*freep)[cnt]; cnt++) 
			;
		break;
	default:
		dno = le64_to_cpu((*freep)[--cnt]);
		break;
	}
	*cntp = cpu_to_le64(cnt);
	if(type){ 
                cntp = &sbi->s_spb->s_bsize;
                cnt = le64_to_cpu(sbi->s_spb->s_bsize) + PFS_STRS_PER_BLOCK;
        }else{
                cntp = &sbi->s_spb->s_iused;
                cnt = le64_to_cpu(sbi->s_spb->s_iused) + 1;
        }
        *cntp = cpu_to_le64(cnt); 
	sbi->s_spb->s_utime = cpu_to_le64(CURRENT_TIME_SEC.tv_sec);
	mark_buffer_dirty(sbi->s_sbh);
	return dno;
}

static int
pfs_free0(struct super_block *sb, int64_t dno, int type, int64_t *cntp, int64_t *headp, 
	struct buffer_head **bhp, int64_t **freep)
{
	int32_t	cnt = le64_to_cpu(*cntp);
	struct pfs_sb_info *sbi = PFS_SB(sb);
	
	if(cnt == 0){ 
		if(pfs_clear_block(sb, dno, type ? PFS_BLOCKSIZ : PFS_SECTORSIZ))
			return -1;
	}
	if(cnt == (type ? PFS_INBLOCKS : PFS_ININODES)){
		struct buffer_head *bh;

		if(!(bh = sb_bread(sb, dno / PFS_INDS_PER_BLOCK)))
			return -1;
		cnt = 1;
		brelse(*bhp);	
		*bhp = bh;
		*freep = type ? (int64_t *)bh->b_data : (int64_t *)((struct pfs_inode *)bh->b_data + dno % PFS_INDS_PER_BLOCK);
		(*freep)[0] = *headp; 
		*headp = cpu_to_le64(dno);
	}else
		(*freep)[cnt++] = cpu_to_le64(dno);
	*cntp = cpu_to_le64(cnt);
	if(type){ 
		cntp = &sbi->s_spb->s_bsize;
		cnt = le64_to_cpu(sbi->s_spb->s_bsize) - PFS_STRS_PER_BLOCK;
	}else{
		cntp = &sbi->s_spb->s_iused;
		cnt = le64_to_cpu(sbi->s_spb->s_iused) - 1;
	}	
	*cntp = cpu_to_le64(cnt); 
	sbi->s_spb->s_utime = cpu_to_le64(CURRENT_TIME_SEC.tv_sec);
        mark_buffer_dirty(*bhp);
        mark_buffer_dirty(sbi->s_sbh);
	return 0;
}

int
pfs_clear_block(struct super_block *sb, int64_t dno, int size)
{
	struct buffer_head *bh;

	if(!(bh = sb_bread(sb, dno / PFS_INDS_PER_BLOCK)))
		return -1;
	memset((struct pfs_inode *)bh->b_data + dno % PFS_INDS_PER_BLOCK, 0, size);
        mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

int64_t
pfs_alloc_zero(struct super_block *sb)
{
	int64_t	dno;

	if(!(dno = pfs_alloc(sb, PFS_ALLOC_BLOCK)))
		return dno;
	if(pfs_clear_block(sb, dno, PFS_BLOCKSIZ)){
		pfs_free(sb, dno, PFS_ALLOC_BLOCK);
		return 0;
	}
	return dno;
}

int64_t
pfs_alloc(struct super_block *sb, int type)
{
	struct pfs_sb_info *sbi = PFS_SB(sb);
	struct pfs_super_block *spb = sbi->s_spb;

	return pfs_alloc0(sb, type, type ? &spb->s_bcnt : &spb->s_icnt, type ? &spb->s_bhead : &spb->s_ihead,
		type ? &sbi->s_bbh : &sbi->s_ibh, type ? &sbi->s_bfree : &sbi->s_ifree);
}

int
pfs_free(struct super_block *sb, int64_t dno, int type)
{
	struct pfs_sb_info *sbi = PFS_SB(sb);
	struct pfs_super_block *spb = sbi->s_spb;

	return pfs_free0(sb, dno, type, type ? &spb->s_bcnt : &spb->s_icnt, type ? &spb->s_bhead : &spb->s_ihead, 
		type ? &sbi->s_bbh : &sbi->s_ibh, type ? &sbi->s_bfree : &sbi->s_ifree);
}
