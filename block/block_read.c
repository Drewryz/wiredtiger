/**************************************************************************
*block�Ķ�����ʵ��
**************************************************************************/
#include "wt_internal.h"

/*��addrָ���block����Ԥ����*/
int __wt_bm_preload(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BLOCK *block;
	WT_DECL_RET;
	wt_off_t offset;
	uint32_t cksum, size;
	int mapped;


	WT_UNUSED(addr_size);
	block = bm->block;
	ret = EINVAL;

	/* У��addr�ĺϷ���,����ȡ��Ӧ��offset/size/checksum*/
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/*����Ƿ����Ϊmmap��ʽ�ṩԤ��*/
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if(mapped){
		/*����mmap��ʽ���ļ�Ԥ����*/
		WT_RET(__wt_mmap_preload(session, (uint8_t *)bm->map + offset, size));
	}
	else{ /*ֱ�Ӷ��ļ���ʽ��Ԥ����*/
		ret = posix_fadvise(block->fh->fd, (wt_off_t)offset, (wt_off_t)size, POSIX_FADV_WILLNEED);

		/*Ԥ����ʧ�ܣ�ֱ�ӽ���Ԥ���������ļ��ⲿ������һ������page cache��*/
		if (ret != 0) {
			WT_DECL_ITEM(tmp);
			WT_RET(__wt_scr_alloc(session, size, &tmp));
			ret = __wt_block_read_off(session, block, tmp, offset, size, cksum);
			__wt_scr_free(session, &tmp);
			WT_RET(ret);
		}
	}

	/*����Ԥ������Ϣ*/
	WT_STAT_FAST_CONN_INCR(session, block_preload);

	return 0;
}

/*��addr��Ӧ��block���ݶ�ȡ��buf��*/
int __wt_bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
	WT_BLOCK *block;
	int mapped;
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(addr_size);
	block = bm->block;

	/*��addr�ж�ȡoffset/checksum/size*/
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/*��mmap����ķ�Χ��*/
	mapped = bm->map != NULL && offset + size <= (wt_off_t)bm->maplen;
	if (mapped) {
		buf->data = (uint8_t *)bm->map + offset;
		buf->size = size;
		WT_RET(__wt_mmap_preload(session, buf->data, buf->size));

		WT_STAT_FAST_CONN_INCR(session, block_map_read);
		WT_STAT_FAST_CONN_INCRV(session, block_byte_map_read, size);
		return 0;
	}
	/*��block���ݶ�ȡ��buf��*/
	WT_RET(__wt_block_read_off(session, block, buf, offset, size, cksum));

	/*����page cache�����block�����Ѿ��������õ����ߣ���block��Ӧ���ļ�page cache���*/
	if (block->os_cache_max != 0 && (block->os_cache += size) > block->os_cache_max) {
		WT_DECL_RET;
		block->os_cache = 0;
		ret = posix_fadvise(block->fh->fd,(wt_off_t)0, (wt_off_t)0, POSIX_FADV_DONTNEED);
		if (ret != 0 && ret != EINVAL)
			WT_RET_MSG(session, ret, "%s: posix_fadvise", block->name);
	}

	return 0;
}

/*����offset��size����Ϣ����block�����ݶ�ȡ��buf��,��У��checksum*/
int __wt_block_read_off(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_ITEM* buf, wt_off_t offset, uint32_t size, uint32_t cksum)
{
	WT_BLOCK_HEADER *blk;
	size_t bufsize;
	uint32_t page_cksum;

	WT_RET(__wt_verbose(session, WT_VERB_READ, "off %" PRIuMAX ", size %" PRIu32 ", cksum %" PRIu32, (uintmax_t)offset, size, cksum));
	/*����״̬ͳ����Ϣ*/
	WT_STAT_FAST_CONN_INCR(session, block_read);
	WT_STAT_FAST_CONN_INCRV(session, block_byte_read, size);

	/*����bufsize�Ķ���*/
	if (F_ISSET(buf, WT_ITEM_ALIGNED))
		bufsize = size;
	else {
		F_SET(buf, WT_ITEM_ALIGNED);
		bufsize = WT_MAX(size, buf->memsize + 10);
	}

	/*ȷ��buf���д�СΪbufsize*/
	WT_RET(__wt_buf_init(session, buf, bufsize));
	/*���ļ��ж�ȡ���ݵ�buf��*/
	WT_RET(__wt_read(session, block->fh, offset, size, buf->mem));
	buf->size = size;

	/*����checksumУ��*/
	blk = WT_BLOCK_HEADER_REF(buf->mem);
	page_cksum = blk->cksum;
	if (page_cksum == cksum) {
		blk->cksum = 0;
		page_cksum = __wt_cksum(buf->mem, F_ISSET(blk, WT_BLOCK_DATA_CKSUM) ?size : WT_BLOCK_COMPRESS_SKIP);
		if (page_cksum == cksum)
			return 0;
	}

	/*block���ݱ��ƻ�*/
	if (!F_ISSET(session, WT_SESSION_SALVAGE_CORRUPT_OK))
		__wt_errx(session, "read checksum error [%" PRIu32 "B @ %" PRIuMAX ", %" PRIu32 " != %" PRIu32 "]", size, (uintmax_t)offset, cksum, page_cksum);

	/* Panic if a checksum fails during an ordinary read. */
	return (block->verify || F_ISSET(session, WT_SESSION_SALVAGE_CORRUPT_OK) ? WT_ERROR : __wt_illegal_value(session, block->name));
}
