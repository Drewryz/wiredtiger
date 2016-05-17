/***********************************************************
* btree��file block�Ķ�д����ʵ��
***********************************************************/

#include "wt_internal.h"

/*��block addr��Ӧ���ļ�λ���ж�ȡ��Ӧpage�����ݣ�����btree�������������Ƿ���Ҫ��ѹ*/
int __wt_bt_read(WT_SESSION_IMPL* session, WT_ITEM* buf, const uint8_t* addr, size_t addr_size)
{
	WT_BM* bm;
	WT_BTREE* btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	const WT_PAGE_HEADER* dsk;
	size_t result_len;

	btree = S2BT(session);
	bm = btree->bm;

	/*����btree��һ��ѹ���͵����ݴ洢�������Ƚ�ѹ,��ô��ȡ�����ݱ����ȴ���һ����ʱ�������У�Ȼ���ѹ��buf��*/
	if (btree->compressor == NULL){
		WT_RET(bm->read(bm, session, buf, addr, addr_size));
		dsk = buf->data;
	}
	else{
		WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(bm->read(bm, session, tmp, addr, addr_size));
		dsk = tmp->data;
	}

	/*�ж�block�����Ƿ���ѹ�����ģ�����ǣ����н�ѹ*/
	if (F_ISSET(dsk, WT_PAGE_COMPRESSED)){
		if (btree->compressor == NULL || btree->compressor->decompress == NULL)
			WT_ERR_MSG(session, WT_ERROR, "read compressed block where no compression engine configured");

		WT_ERR(__wt_buf_initsize(session, buf, dsk->mem_size));

		memcpy(buf->mem, tmp->data, WT_BLOCK_COMPRESS_SKIP);
		ret = btree->compressor->decompress(btree->compressor, &session->iface,
			(uint8_t *)tmp->data + WT_BLOCK_COMPRESS_SKIP,
			tmp->size - WT_BLOCK_COMPRESS_SKIP,
			(uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP,
			dsk->mem_size - WT_BLOCK_COMPRESS_SKIP, &result_len);

		/*��ѹʧ�ܻ���ѹ�������ݳ��Ȳ��ԣ�����һ��ϵͳ���󣬿�������Ϊ������������ɵ�*/
		if (ret != 0 || result_len != dsk->mem_size - WT_BLOCK_COMPRESS_SKIP)
			WT_ERR(F_ISSET(btree, WT_BTREE_VERIFY) || F_ISSET(session, WT_SESSION_SALVAGE_CORRUPT_OK) ?
				WT_ERROR :__wt_illegal_value(session, btree->dhandle->name));
	}
	else{
		if (btree->compressor == NULL)
			buf->size = dsk->mem_size;
		else{
			/*
			* We guessed wrong: there was a compressor, but this
			* block was not compressed, and now the page is in the
			* wrong buffer and the buffer may be of the wrong size.
			* This should be rare, but happens with small blocks
			* that aren't worth compressing.
			* �������̫���ǲ���ѹ����
			*/
			WT_ERR(__wt_buf_set(session, buf, tmp->data, dsk->mem_size));
		}
	}

	/*��������У��*/
	if (F_ISSET(btree, WT_BTREE_VERIFY)) {
		if (tmp == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));

		WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));
		WT_ERR(__wt_verify_dsk(session, (const char *)tmp->data, buf));
	}

	/*�޸�ͳ����Ϣ*/
	WT_STAT_FAST_CONN_INCR(session, cache_read);
	WT_STAT_FAST_DATA_INCR(session, cache_read);
	if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
		WT_STAT_FAST_DATA_INCR(session, compress_read);
	WT_STAT_FAST_CONN_INCRV(session, cache_bytes_read, dsk->mem_size);
	WT_STAT_FAST_DATA_INCRV(session, cache_bytes_read, dsk->mem_size);

err:
	__wt_scr_free(session, &tmp);
}

/*��buf�е�����д�뵽addr��Ӧpage�е�block��*/
int __wt_bt_write(WT_SESSION_IMPL* session, WT_ITEM* buf, uint8_t* addr, size_t* addr_sizep, int checkpoint, int compressed)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_ITEM *ip;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE_HEADER *dsk;
	size_t len, src_len, dst_len, result_len, size;
	int data_cksum, compression_failed;
	uint8_t *src, *dst;

	btree = S2BT(session);
	bm = btree->bm;

	WT_ASSERT(session, (checkpoint == 0 && addr != NULL && addr_sizep != NULL) || (checkpoint == 1 && addr == NULL && addr_sizep == NULL));

	/*������ѹ�����жϣ����Ҫѹ���������Ƚ�buf�е�����ͨ��btree->compressor->compress����ѹ����һ����ʱ��������*/
	if (btree->compressor == NULL || btree->compressor->compress == NULL || compressed)
		ip = buf;
	else if (buf->size <= btree->allocsize) /*����̫���ˣ�����ѹ��*/
		ip = buf;
	else{ /*��������ѹ��*/
		src = (uint8_t *)buf->mem + WT_BLOCK_COMPRESS_SKIP;
		src_len = buf->size - WT_BLOCK_COMPRESS_SKIP;

		/*Ԥ�ȼ���ѹ��������ݳ��ȣ��������ڷ�����ʱ�������ͱ��page header*/
		if (btree->compressor->pre_size == NULL)
			len = src_len;
		else
			WT_ERR(btree->compressor->pre_size(btree->compressor, &session->iface, src, src_len, &len));

		/*����block�����ݳ���size*/
		size = len + WT_BLOCK_COMPRESS_SKIP;
		WT_ERR(bm->write_size(bm, session, &size));
		WT_ERR(__wt_scr_alloc(session, size, &tmp));

		dst = (uint8_t *)tmp->mem + WT_BLOCK_COMPRESS_SKIP;
		dst_len = len;

		/*����ѹ��*/
		compression_failed = 0;
		WT_ERR(btree->compressor->compress(btree->compressor,
			&session->iface,
			src, src_len,
			dst, dst_len,
			&result_len, &compression_failed));
		result_len += WT_BLOCK_COMPRESS_SKIP;

		/*��������ѹ��ʧ�ܣ�ֱ��д��ԭʼ����*/
		if (compression_failed || buf->size / btree->allocsize <= result_len / btree->allocsize) {
			ip = buf;
			WT_STAT_FAST_DATA_INCR(session, compress_write_fail);
		}
		else{
			compressed = 1;
			WT_STAT_FAST_DATA_INCR(session, compress_write);

			memcpy(tmp->mem, buf->mem, WT_BLOCK_COMPRESS_SKIP);
			tmp->size = result_len;
			ip = tmp;
		}
	}

	/*��ip��mem��Ϊpage�Ŀռ��׵�ַ*/
	dsk = ip->mem;
	/*����page��ѹ����־*/
	if (compressed)
		F_SET(dsk, WT_PAGE_COMPRESSED);

	/*
	* We increment the block's write generation so it's easy to identify
	* newer versions of blocks during salvage.  (It's common in WiredTiger,
	* at least for the default block manager, for multiple blocks to be
	* internally consistent with identical first and last keys, so we need
	* a way to know the most recent state of the block.  We could check
	* which leaf is referenced by a valid internal page, but that implies
	* salvaging internal pages, which I don't want to do, and it's not
	* as good anyway, because the internal page may not have been written
	* after the leaf page was updated.  So, write generations it is.
	*
	* Nothing is locked at this point but two versions of a page with the
	* same generation is pretty unlikely, and if we did, they're going to
	* be roughly identical for the purposes of salvage, anyway.
	*/
	dsk->write_gen = ++btree->write_gen;

	switch (btree->checksum){
	case CKSUM_ON:
		data_cksum = 1;
		break;

	case CKSUM_OFF:
		data_cksum = 0;
		break;

	case CKSUM_UNCOMPRESSED:
	default:
		data_cksum = !compressed;
		break;
	}

	/*����checkpoint��������*/
	WT_ERR(checkpoint ? bm->checkpoint(bm, session, ip, btree->ckpt, data_cksum) : bm->write(bm, session, ip, addr, addr_sizep, data_cksum));

	WT_STAT_FAST_CONN_INCR(session, cache_write);
	WT_STAT_FAST_DATA_INCR(session, cache_write);
	WT_STAT_FAST_CONN_INCRV(session, cache_bytes_write, dsk->mem_size);
	WT_STAT_FAST_DATA_INCRV(session, cache_bytes_write, dsk->mem_size);

err:
	__wt_scr_free(session, &tmp);
	return ret;
}

