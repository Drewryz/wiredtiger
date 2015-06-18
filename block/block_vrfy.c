/**************************************************************************
*
**************************************************************************/

#include "wt_internal.h"

static int __verify_ckptfrag_add(WT_SESSION_IMPL *, WT_BLOCK *, wt_off_t, wt_off_t);
static int __verify_ckptfrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_filefrag_add(WT_SESSION_IMPL *, WT_BLOCK *, const char *, wt_off_t, wt_off_t, int);
static int __verify_filefrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_last_avail(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);
static int __verify_last_truncate(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);

/*����offƫ��λ����block����������������ļ���ͷ��block desc��һ�����볤��*/
#define	WT_wt_off_TO_FRAG(block, off)		((off) / (block)->allocsize - 1)
/*ͨ�������±꣬����offsetƫ��λ��*/
#define	WT_FRAG_TO_OFF(block, frag)			(((wt_off_t)(frag + 1)) * (block)->allocsize)

/*��һ����block�ĺ�ʵУ��*/
int __wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase)
{
	WT_CKPT *ckpt;
	wt_off_t size;

	/*��λ�����һ����checkpointԪ��Ϣ,������Ǹ���Ч�Ŀ���Ϣ��Ԫ������Ҫ��������У��??*/
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;

	for (;; --ckpt) {
		if (ckpt->name != NULL && !F_ISSET(ckpt, WT_CKPT_FAKE))
			break;

		/*ckpt�ǵ�һ�����������У��*/
		if (ckpt == ckptbase)
			return 0;
	}

	/* Truncate the file to the size of the last checkpoint. */
	WT_RET(__verify_last_truncate(session, block, ckpt));

	/*�ļ�û���κ�pageҳ���ݣ��޷�У��*/
	size = block->fh->size;
	if (size <= block->allocsize)
		return 0;

	/*�ļ�������blockҪ��ĳ��Ӷ���ģ�ֱ�ӷ��ش���*/
	if (size % block->allocsize != 0)
		WT_RET_MSG(session, WT_ERROR, "the file size is not a multiple of the allocation size");

	/* ����ļ��ж��ٸ���blockҪ��Ķ��볤�ȿ�,������һ��fragfile bitλ������Ӧ��
	 * ����size = 1TB,��ôfragfile = (((1 * 2^40) / 512) / 8) = 256 * 2^20 = 256MB*/
	block->frags = (uint64_t)WT_wt_off_TO_FRAG(block, size);
	WT_RET(__bit_alloc(session, block->frags, &block->fragfile));

	WT_RET(__wt_block_extlist_init(session, &block->verify_alloc, "verify", "alloc", 0));
	/*�������һ��checkpoint��Ϣ��ȷ��fragfile����Щ������г�����extent����*/
	WT_RET(__verify_last_avail(session, block, ckpt));

	block->verify = 1;
	return 0;
}

static int __verify_last_avail(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_CKPT* ckpt)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_DECL_RET;
	WT_EXT *ext;
	WT_EXTLIST *el;

	ci = &_ci;
	/*��ʼ��ci�ṹ*/
	WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
	/*��ckpt�е�checkpointԪ��Ϣ�����е�ci��*/
	WT_ERR(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));

	el = &ci->avail;

	if(el->offset != WT_BLOCK_INVALID_OFFSET){
		/*���ļ��е�ext�����ȡ��el�б��У����ǻ��ȥel����ռ�õ�ext����(entry.off/sizeռ�õ�ext)*/
		WT_ERR(__wt_block_extlist_read_avail(session, block, el, ci->file_size));

		/*����el�б�ȷ��el�е�ext.off����Щ������У�*/
		WT_EXT_FOREACH(ext, el->off){
			if ((ret = __verify_filefrag_add(session, block, "avail-list chunk", ext->off, ext->size, 1)) != 0)
				break;
		}
	}

err:
	__wt_block_ckpt_destroy(session, ci);

	return ret;
}

/*ǿ�ƽ��ļ����ó����һ��checkpointָ�����ļ�����*/
static int __verify_last_truncate(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_CKPT* ckpt)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_DECL_RET;

	ci = &_ci;
	WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
	WT_ERR(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));
	WT_ERR(__wt_ftruncate(session, block->fh, ci->file_size));

err:	
	__wt_block_ckpt_destroy(session, ci);
	return ret;
}

/*������һ��block�ĺ�ʵУ�����*/
int __wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;

	ret = __verify_filefrag_chk(session, block);
	/*��verify alloc�е�ext�ͷŵ�*/
	__wt_block_extlist_free(session, &block->verify_alloc);

	__wt_free(session, block->fragfile);
	__wt_free(session, block->fragckpt);

	block->verify = 0;
	return ret;
}

int __wt_verify_ckpt_load(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
{
	WT_EXTLIST *el;
	WT_EXT *ext;
	uint64_t frag, frags;

	block->verify_size = ci->file_size;

	/*��checkpointԪ����λ��Ҳ���õ�filefrag��*/
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "checkpoint", ci->root_offset, (wt_off_t)ci->root_size, 1));

	if (ci->alloc.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "alloc list", ci->alloc.offset, (wt_off_t)ci->alloc.size, 1));

	if (ci->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "avail list", ci->avail.offset, (wt_off_t)ci->avail.size, 1));

	if (ci->discard.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "discard list", ci->discard.offset, (wt_off_t)ci->discard.size, 1));

	el = &ci->alloc;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		/*��alloc�е�ext�������*/
		WT_RET(__wt_block_extlist_read(session, block, el, ci->file_size));
		/*��el�ϲ���verify_alloc��*/
		WT_RET(__wt_block_extlist_merge( session, el, &block->verify_alloc));
		__wt_block_extlist_free(session, el);
	}

	el = &ci->discard;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		/*��discard list�е�ext������������Щext��verify��ȥ��*/
		WT_RET(__wt_block_extlist_read(session, block, el, ci->file_size));
		WT_EXT_FOREACH(ext, el->off){
			WT_RET(__wt_block_off_remove_overlap(session, &block->verify_alloc, ext->off, ext->size));
		}
		__wt_block_extlist_free(session, el);
	}

	/*��ȡavail��ext����,��Ҫ��У��avail list�ĺϷ���*/
	el = &ci->avail;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el, ci->file_size));
		__wt_block_extlist_free(session, el);
	}

	/*ɾ����root ext(root.off, root.size)��block->verify_alloc�ص�����*/
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_off_remove_overlap(session, &block->verify_alloc, ci->root_offset, ci->root_size));

	WT_RET(__bit_alloc(session, block->frags, &block->fragckpt));
	el = &block->verify_alloc;

	/*ȷ��ext��ĩβһ�������bit������1��Ϊ���У��*/
	WT_EXT_FOREACH(ext, el->off) {
		frag = (uint64_t)WT_wt_off_TO_FRAG(block, ext->off);
		frags = (uint64_t)(ext->size / block->allocsize);
		__bit_nset(block->fragckpt, frag, frag + (frags - 1));
	}

	return 0;
}

/*����block��ռ�Ϸ���У��*/
int __wt_verify_ckpt_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;

	ret = __verify_ckptfrag_chk(session, block);
	__wt_free(session, block->fragckpt);

	return ret;
}





