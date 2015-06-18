/***************************************************************************
*�ļ��޸���������ʵ��
***************************************************************************/
#include "wt_internal.h"

/*��block��Ӧ���ļ����г�ʼ�����ã��൱���ļ��޸�����*/
int __wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	wt_off_t len;
	uint32_t allocsize;

	allocsize = block->allocsize;

	/*��block������Ϣд�뵽�ļ��ĵ�һ��allocsize�����λ����*/
	WT_RET(__wt_desc_init(session, block->fh, allocsize));

	/*��ʼ��live�ṹ*/
	WT_RET(__wt_block_ckpt_init(session, &(block->live), "live"));

	/*���㰴allocsize���µ����ļ���С*/
	if (block->fh->size > allocsize) {
		len = (block->fh->size / allocsize) * allocsize;
		if (len != block->fh->size)
			WT_RET(__wt_ftruncate(session, block->fh, len));
	} else
		len = allocsize;
	block->live.file_size = len;

	/*����salvage��λ�ã���Ϊֻд����һ��allocsize���ȵ�block ����ͷ��Ϣ*/
	block->slvg_off = allocsize;

	/*����alloc���õ��ļ���Χ���൱���½���һ��(allocsize, len - allocsize)ext ����*/
	WT_RET(__wt_block_insert_ext(session, &block->live.alloc, allocsize, len - allocsize));

	return 0;
}

/*�����ļ�������*/
int __wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	/* Discard the checkpoint. */
	return (__wt_block_checkpoint_unload(session, block, 0));
}

/*�ж�(off,size)���block�ĺϷ���*/
int __wt_block_offset_invalid(WT_BLOCK *block, wt_off_t offset, uint32_t size)
{
	if (size == 0)				/* < minimum page size */
		return 1;

	if (size % block->allocsize != 0)	/* not allocation-size units */
		return 1;

	if (size > WT_BTREE_PAGE_SIZE_MAX)	/* > maximum page size */
		return 1;

	if (offset + (wt_off_t)size > block->fh->size) 	/* past end-of-file */
		return 1;

	return 0;
}

/*��block�ļ��л�ȡ��һ�����������ָ���page����*/
int __wt_block_salvage_next(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr, size_t *addr_sizep, int *eofp)
{
	WT_BLOCK_HEADER *blk;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_FH *fh;
	wt_off_t max, offset;
	uint32_t allocsize, cksum, size;
	uint8_t *endp;

	*eofp = 0;

	fh = block->fh;
	allocsize = block->allocsize;

	WT_ERR(__wt_scr_alloc(session, allocsize, &tmp));

	max = fh->size;
	for(;;){
		offset = block->slvg_off;
		if (offset >= max) {			/* �Ѿ����ļ�ĩβ�� */
			*eofp = 1;
			goto done;
		}

		/*��ȡһ�����볤�ȵ�tmp������*/
		WT_ERR(__wt_read(session, fh, offset, (size_t)allocsize, tmp->mem));

		/*���block header*/
		blk = (WT_BLOCK_HEADER*)WT_BLOCK_HEADER_REF(tmp->mem);
		size = blk->disk_size;
		cksum = blk->cksum;

		/*���offset,size�Ϸ�����offset����ʼ��ȡsize���ֽڵ�tmp�У��൱��һ��page*/
		if (!__wt_block_offset_invalid(block, offset, size) &&
			__wt_block_read_off(session, block, tmp, offset, size, cksum) == 0)
			break;

		/*��ȡoffset����pageʧ�ܣ����������ƻ���*/
		WT_ERR(__wt_verbose(session, WT_VERB_SALVAGE, "skipping %" PRIu32 "Bat file offset %" PRIuMAX, allocsize, (uintmax_t)offset));

		WT_ERR(__wt_block_off_free(session, block, offset, (wt_off_t)allocsize));
		/*����һ�����볤�ȣ�Ѱ����һ��δ�ƻ���page*/
		block->slvg_off += allocsize;
	}

	endp = addr;
	/*��page��offset size��cksum���л���addr��������*/
	WT_ERR(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_sizep = WT_PTRDIFF(endp, addr);

done:
err:
	__wt_scr_free(session, &tmp);
	return ret;
}

/*addrλ�õ�page�Ƿ�Ϸ�������block�޸�״̬����*/
int __wt_block_salvage_valid(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr, size_t addr_size, int valid)
{
	wt_off_t offset;
	uint32_t size, cksum;

	WT_UNUSED(session);
	WT_UNUSED(addr_size);

	/*��addr�����е����ݷ����У����off/size/cksum����ֵ*/
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, cksum));
	if (valid)
		block->slvg_off = offset + size;
	else{ /*ֻ����ǰ����һ�����볤��*/
		WT_RET(__wt_block_off_free(session, block, offset, (wt_off_t)block->allocsize));
		block->slvg_off = offset + block->allocsize;
	}

	return 0;
}
