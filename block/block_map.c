/*************************************************************************
*block�ļ�����mmap
*************************************************************************/

#include "wt_internal.h"

/*connָ������mmap��ʽ�����ļ�����ôblock�Ĳ�����ʽ��Ҫ���ó�mmap��ʽ����*/
int __wt_block_map(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapp, size_t *maplenp, void **mappingcookie)
{
	*(void **)mapp = NULL;
	*maplenp = 0;

	/*���block������verify����������������mmap*/
	if (block->verify)
		return (0);

	/*�ļ����õ���direct io��ʽ���ж�д���޷���mmap*/
	if (block->fh->direct_io)
		return (0);

	/*block��Ӧ���ļ�������os page cache���޷�ʹ��mmap,��Ϊ�ļ���Ҫ����os_cache_maxȥ���os page cache,����mmap��������*/
	if (block->os_cache_max != 0)
		return (0);

	/*block �ļ���mmap�������*/
	__wt_mmap(session, block->fh, mapp, maplenp, mappingcookie);

	return 0;
}

/*ж��block�ļ���mmap*/
int __wt_block_unmap(WT_SESSION_IMPL *session, WT_BLOCK *block, void *map, size_t maplen, void **mappingcookie)
{
	return __wt_munmap(session, block->fh, map, maplen, mappingcookie);
}

