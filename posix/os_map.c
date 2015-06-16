/************************************************************************
*��MMAP������װ
************************************************************************/
#include "wt_internal.h"

/*mmapһ���ļ�*/
int __wt_mmap(WT_SESSION_IMPL* session, WT_FH* fh, void* mapp, size_t* lenp, void** mappingcookie)
{
	void* map;
	size_t orig_size;

	UT_UNUSED(mappingcookie);

	orig_size = (size_t)fh->size;

	map = mmap(NULL, PROT_READ, orig_size, MAP_PRIVATE, fh->fd, (wt_off_t)0);
	if(map == MAP_FAILED){
		WT_RET_MSG(session, __wt_errno(), "%s map error: failed to map %" WT_SIZET_FMT " bytes", fh->name, orig_size);
	}

	__wt_verbose(session, WT_VERB_FILEOPS, "%s: map %p: %" WT_SIZET_FMT " bytes", fh->name, map, orig_size);

	*(void **)mapp = map;
	*lenp = orig_size;

	return 0;
}

/* Linux requires the address be aligned to a 4KB boundary. */
#define	WT_VM_PAGESIZE	4096

/*Ԥ����block manager��Ӧ�ļ���page cache,��p��ַ��ʼ��Ԥ����size���ȵ�����,Ϊ��˳���*/
int __wt_mmap_preload(WT_SESSION_IMPL *session, const void *p, size_t size)
{

	WT_BM *bm = S2BT(session)->bm;
	WT_DECL_RET;

	/**4KB����Ѱַ,��ǰ���룬����p = 4097,��ôblk = 4096, size = size + 1*/
	void *blk = (void *)((uintptr_t)p & ~(uintptr_t)(WT_VM_PAGESIZE - 1));
	size += WT_PTRDIFF(p, blk);

	/* XXX proxy for "am I doing a scan?" -- manual read-ahead,,����2MΪ��λ������ļ�����,��ΪԤ����2MΪ��λ*/
	if (F_ISSET(session, WT_SESSION_NO_CACHE)) {
		/* Read in 2MB blocks every 1MB of data. */
		if (((uintptr_t)((uint8_t *)blk + size) & (uintptr_t)((1<<20) - 1)) < (uintptr_t)blk)
			return 0;

		/*����ȷ��SIZE,��С����ռ�2M*/
		size = WT_MIN(WT_MAX(20 * size, 2 << 20), WT_PTRDIFF((uint8_t *)bm->map + bm->maplen, blk));
	}

	/*4KB����*/
	size &= ~(size_t)(WT_VM_PAGESIZE - 1);

	/*�ļ�����Ԥ����*/
	if (size > WT_VM_PAGESIZE && (ret = posix_madvise(blk, size, POSIX_MADV_WILLNEED)) != 0)
		WT_RET_MSG(session, ret, "posix_madvise will need");

	return 0;
}

/*����mmap������ڴ棬��P��ַ��ʼ������Ϊsize*/
int __wt_mmap_discard(WT_SESSION_IMPL *session, void *p, size_t size)
{
	WT_DECL_RET;
	/*4k���룬linux�ļ����ٻ�������4KBΪһ��page cache*/
	void *blk = (void *)((uintptr_t)p & ~(uintptr_t)(WT_VM_PAGESIZE - 1));
	size += WT_PTRDIFF(p, blk);

	return 0;
}

/*ע��һ���ļ���mmap�ڴ�����*/
int __wt_munmap(WT_SESSION_IMPL *session, WT_FH *fh, void *map, size_t len, void **mappingcookie)
{
	WT_UNUSED(mappingcookie);

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: unmap %p: %" WT_SIZET_FMT " bytes", fh->name, map, len));

	if (munmap(map, len) == 0)
		return (0);

	WT_RET_MSG(session, __wt_errno(), "%s unmap error: failed to unmap %" WT_SIZET_FMT " bytes",
		fh->name, len);
}


