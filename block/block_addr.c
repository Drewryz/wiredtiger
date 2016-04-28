/************************************************************************
*��block addr(offset/size/checksum)�����л������л�����
************************************************************************/

#include "wt_internal.h"

/*��pp�ж�ȡblock addr(size/checksum/offset)*/
static int __block_buffer_to_addr(WT_BLOCK* block, const uint8_t **pp, wt_off_t* offsetp, uint32_t* sizep, uint32_t* cksump)
{
	uint64_t o, s, c;

	WT_RET(__wt_vunpack_uint(pp, 0, &o));
	WT_RET(__wt_vunpack_uint(pp, 0, &s));
	WT_RET(__wt_vunpack_uint(pp, 0, &c));

	/*����Ϊ0ʱ����ʾ���������ݶ���0*/
	if(s == 0){
		*offsetp = 0;
		*sizep = *cksump = 0;
	}
	else{
		/*offset��size����allocsize���������������ô���д������׺���*/
		*offsetp = (wt_off_t)(o + 1) * block->allocsize;
		*sizep = (uint32_t)s * block->allocsize;
		*cksump = (uint32_t)c;
	}

	return 0;
}

/*��block addr�����pp��������*/
int __wt_block_addr_to_buffer(WT_BLOCK *block, uint8_t **pp, wt_off_t offset, uint32_t size, uint32_t cksum)
{
	uint64_t o, s, c;

	/* See the comment above: this is the reverse operation. */
	if (size == 0) {
		o = WT_BLOCK_INVALID_OFFSET;
		s = c = 0;
	} else {
		o = (uint64_t)offset / block->allocsize - 1;
		s = size / block->allocsize;
		c = cksum;
	}

	WT_RET(__wt_vpack_uint(pp, 0, o));
	WT_RET(__wt_vpack_uint(pp, 0, s));
	WT_RET(__wt_vpack_uint(pp, 0, c));

	return 0;
}

/*��pp�ж�ȡblock addr(size/checksum/offset),�Ӵ������仺�����н���block addr*/
int __wt_block_buffer_to_addr(WT_BLOCK *block, const uint8_t *p, wt_off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	return (__block_buffer_to_addr(block, &p, offsetp, sizep, cksump));
}

/*�ж�block ��addr�Ƿ�Ϸ�,�Ϸ�����,1�����Ϸ�����Ϊ0*/
int __wt_block_addr_valid(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size, int live)
{
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(session);
	WT_UNUSED(addr_size);
	WT_UNUSED(live);

	/*��ȡblock addr��ֵ*/
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	return (offset + size > block->fh->size ? 0 : 1);
}

/*��addr�������ж�ȡ��Ӧ��block addr������addr��checksum/size/offset����ʽ�������buf��*/
int __wt_block_addr_string(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/* Printable representation. */
	WT_RET(__wt_buf_fmt(session, buf, "[%" PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		(uintmax_t)offset, (uintmax_t)offset + size, size, cksum));

	return (0);
}

/*��p����������checkpoint����Ϣ*/
int __wt_block_buffer_to_ckpt(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *p, WT_BLOCK_CKPT *ci)
{
	uint64_t a;
	const uint8_t **pp;

	ci->version = *p++;
	if (ci->version != WT_BM_CHECKPOINT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "unsupported checkpoint version");

	pp = &p;
	/*��pp�Ļ������н���������block addr*/
	WT_RET(__block_buffer_to_addr(block, pp, &ci->root_offset, &ci->root_size, &ci->root_cksum));
	WT_RET(__block_buffer_to_addr(block, pp, &ci->alloc.offset, &ci->alloc.size, &ci->alloc.cksum));
	WT_RET(__block_buffer_to_addr(block, pp, &ci->avail.offset, &ci->avail.size, &ci->avail.cksum));
	WT_RET(__block_buffer_to_addr(block, pp, &ci->discard.offset, &ci->discard.size, &ci->discard.cksum));
	/*��ȡci���ļ�����*/
	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	ci->file_size = (wt_off_t)a;
	/*��ȡcheckpoint���ֽ���*/
	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	ci->ckpt_size = a;

	return (0);
}

/*��ckeckpoint��Ϣд�뵽pp�Ļ�������*/
int __wt_block_ckpt_to_buffer(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t **pp, WT_BLOCK_CKPT *ci)
{
	uint64_t a;

	if (ci->version != WT_BM_CHECKPOINT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "unsupported checkpoint version");

	(*pp)[0] = ci->version;
	(*pp)++;

	WT_RET(__wt_block_addr_to_buffer(block, pp, ci->root_offset, ci->root_size, ci->root_cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp, ci->alloc.offset, ci->alloc.size, ci->alloc.cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp, ci->avail.offset, ci->avail.size, ci->avail.cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp, ci->discard.offset, ci->discard.size, ci->discard.cksum));

	a = (uint64_t)ci->file_size;
	WT_RET(__wt_vpack_uint(pp, 0, a));
	a = (uint64_t)ci->ckpt_size;
	WT_RET(__wt_vpack_uint(pp, 0, a));

	return (0);
}

