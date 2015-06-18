/**************************************************************************
*
***************************************************************************/

#include "wt_internal.h"

static int __ckpt_process(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);
static int __ckpt_string(WT_SESSION_IMPL *, WT_BLOCK *, const uint8_t *, WT_ITEM *);
static int __ckpt_update(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *, WT_BLOCK_CKPT *, int);

/*��block checkpoint�ṹ����*/
int __wt_block_ckpt_init(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci, const char *name)
{
	WT_CLEAR(*ci);

	ci->version = WT_BM_CHECKPOINT_VERSION;
	ci->root_offset = WT_BLOCK_INVALID_OFFSET;

	/*��ʼ������ext skip list*/
	WT_RET(__wt_block_extlist_init(session, &ci->alloc, name, "alloc", 0));
	/*��WT_SIZE��Ϊ��������,������ٴ�avail�л�ú��ʵ�ext����*/
	WT_RET(__wt_block_extlist_init(session, &ci->avail, name, "avail", 1));
	WT_RET(__wt_block_extlist_init(session, &ci->discard, name, "discard", 0));
	/*��WT_SIZE��Ϊ��������*/
	WT_RET(__wt_block_extlist_init(session, &ci->ckpt_avail, name, "ckpt_avail", 1));

	return 0;
}

/*��block��Ӧ���ļ�������һ��checkpoint��Ϣ��*/
int __wt_block_checkpoint_load(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size, 
								uint8_t *root_addr, size_t *root_addr_sizep, int checkpoint)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	uint8_t *endp;

	WT_UNUSED(addr_size);
	ci = NULL;

	/*��ʱ�����ҵ�rootҳ�������������Ҫ����һ���յ�ַ��ȥ��Ҳ����root_addr_size = 0*/
	*root_addr_sizep = 0;

	if(WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT)){
		if (addr != NULL) {
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__ckpt_string(session, block, addr, tmp));
		}
		WT_ERR(__wt_verbose(session, WT_VERB_CHECKPOINT, "%s: load-checkpoint: %s", block->name, addr == NULL ? "[Empty]" : (const char *)tmp->data));
	}

	if(checkpoint){
		ci = &_ci;
		/*����checkpoint��ʼ��*/
		WT_ERR(__wt_block_ckpt_init(session, ci, "checkpoint"));
	}
	else{
		ci = &block->live;
		WT_ERR(__wt_block_ckpt_init(session, ci, "live"));
	}

	if(addr == NULL || addr_size == 0){
		ci->file_size = block->allocsize;
	}
	else{
		/*��addr�ж�ȡ����checkpoint��Ϣ(root_addr, alloc_addr, avail_addr, discard_addr)*/
		WT_ERR(__wt_block_buffer_to_ckpt(session, block, addr, ci));

		/* Verify sets up next. */
		if (block->verify)
			WT_ERR(__wt_verify_ckpt_load(session, block, ci));

		/* ��root��λ����Ϣд�뵽root addr��������*/
		if (ci->root_offset != WT_BLOCK_INVALID_OFFSET) {
			endp = root_addr;
			WT_ERR(__wt_block_addr_to_buffer(block, &endp, ci->root_offset, ci->root_size, ci->root_cksum));
			*root_addr_sizep = WT_PTRDIFF(endp, root_addr);
		}

		/*��block ��Ӧ���ļ��ж�ȡavail skip list��Ϣ,����ci->avail.off/size���ж�ȡ,liveģʽ*/
		if (!checkpoint)
			WT_ERR(__wt_block_extlist_read_avail(session, block, &ci->avail, ci->file_size));
	}

	if(!checkpoint){
		WT_ERR(__wt_verbose(session, WT_VERB_CHECKPOINT, "truncate file to %" PRIuMAX, (uintmax_t)ci->file_size));
		WT_ERR_BUSY_OK(__wt_ftruncate(session, block->fh, ci->file_size));
	}

	if(0){
err:
		if (block->verify)
			WT_TRET(__wt_verify_ckpt_unload(session, block));
	}

	if (checkpoint && ci != NULL)
		__wt_block_ckpt_destroy(session, ci);

	__wt_scr_free(session, &tmp);

	return ret;
}

/*ж��һ��block checkpoint,û��������Ҫ���и�ϰ*/
int __wt_block_checkpoint_unload(WT_SESSION_IMPL *session, WT_BLOCK *block, int checkpoint)
{
	WT_DECL_RET;

	if (block->verify)
		WT_TRET(__wt_verify_ckpt_unload(session, block));

	if(!checkpoint){
		WT_TRET_BUSY_OK(__wt_ftruncate(session, block->fh, block->fh->size));

		__wt_spin_lock(session, &block->live_lock);
		__wt_block_ckpt_destroy(session, &block->live);
		__wt_spin_unlock(session, &block->live_lock);
	}

	return ret;
}

/*����һ��checkpoint�ṹ*/
void __wt_block_ckpt_destroy(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci)
{
	/* Discard the extent lists. */
	__wt_block_extlist_free(session, &ci->alloc);
	__wt_block_extlist_free(session, &ci->avail);
	__wt_block_extlist_free(session, &ci->discard);
	__wt_block_extlist_free(session, &ci->ckpt_alloc);
	__wt_block_extlist_free(session, &ci->ckpt_avail);
	__wt_block_extlist_free(session, &ci->ckpt_discard);
}

/*����һ��block checkpoint*/
int __wt_block_checkpoint(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, WT_CKPT *ckptbase, int data_cksum)
{
	WT_BLOCK_CKPT *ci;
	WT_DECL_RET;

	ci = &block->live;

	/*��block���ó����¸���ģʽ��Ҳ����ext object��avail list�д�ͷ����*/
	__wt_block_configure_first_fit(block, 1);

	/*buf�ǿյģ�ֱ�Ӷ�����root page��Ϣ����*/
	if(buf == NULL){
		ci->root_offset = WT_BLOCK_INVALID_OFFSET;
		ci->root_size = ci->root_cksum = 0;
	}
	else{
		/*��buf�е�����д�뵽��Ӧ���ļ��У�������õ�offset��size��cksum*/
		WT_ERR(__wt_block_write_off(session, block, buf,
			&ci->root_offset, &ci->root_size, &ci->root_cksum, data_cksum, 0));
	}

	/*checkpoint���̿��ܻ�����ܶ��block�Ķ���д�ͺϲ���������Ҫ������ext��������������Ԥ����250��ext��������Щ��������Ҫ*/
	WT_ERR(__wt_block_ext_prealloc(session, 250));

	/*����checkpoint����*/
	ret = __ckpt_process(session, block, ckptbase);

	/*�����ͷ�һЩ����Ҫ��ext obj����ֻ����250��*/
	WT_TRET(__wt_block_ext_discard(session, 250));

err:
	__wt_block_configure_first_fit(block, 0);
	return ret;
}

/*��ckpt��Ԫ��Ϣ�ж�ȡcheckpoint����Ϣ����������Щ��Ϣ��ȡ��Ӧ��ext list��Ϣ��������������*/
static int __ckpt_extlist_read(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_CKPT* ckpt)
{
	WT_BLOCK_CKPT *ci;

	/*����һ��checkpoint�ṹ����������Ӧ��addr��checkpoint��ext list����*/
	WT_RET(__wt_calloc(session, 1, sizeof(WT_BLOCK_CKPT), &ckpt->bpriv));

	ci = ckpt->bpriv;
	/*��ckpt��raw�н�����checkpoint��root��Ϣ*/
	WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
	WT_RET(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));

	/*��block�ж�ȡalloc ext list��Ϣ*/
	WT_RET(__wt_block_extlist_read(session, block, &ci->alloc, ci->file_size));
	/*��ȡblock�е�discard ext list��Ϣ*/
	WT_RET(__wt_block_extlist_read(session, block, &ci->discard, ci->file_size));

	return 0;
}

/* ��el��off/size��Ϊext������뵽live_avail�б���,el�е���������Ӧ���������Ĳ���,�൱�ڽ�
 * checkpoint����Ҫ�ͷŵ�ext list����ת��,������Ҫ����ckpt_avail��ȷ���ļ��ĳ���*/
static int __ckpt_extlist_fblocks(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
	if(el->offset == WT_BLOCK_INVALID_OFFSET)
		return 0;

	/*
	 * Free blocks used to write checkpoint extents into the live system's
	 * checkpoint avail list (they were never on any alloc list).   Do not
	 * use the live system's avail list because that list is used to decide
	 * if the file can be truncated, and we can't truncate any part of the
	 * file that contains a previous checkpoint's extents.
	 */
	return __wt_block_insert_ext(session, &block->live.ckpt_avail, el->offset, el->size);
}


static int __ckpt_process(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_CKPT* ckptbase)
{
	WT_BLOCK_CKPT *a, *b, *ci;
	WT_CKPT *ckpt, *next_ckpt;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	uint64_t ckpt_size;
	int deleting, locked;

	ci = &block->live;
	locked = 0;	

	/*
	 * Checkpoints are a two-step process: first, write a new checkpoint to
	 * disk (including all the new extent lists for modified checkpoints
	 * and the live system).  As part of this, create a list of file blocks
	 * newly available for reallocation, based on checkpoints being deleted.
	 * We then return the locations of the new checkpoint information to our
	 * caller.  Our caller has to write that information into some kind of
	 * stable storage, and once that's done, we can actually allocate from
	 * that list of newly available file blocks.  (We can't allocate from
	 * that list immediately because the allocation might happen before our
	 * caller saves the new checkpoint information, and if we crashed before
	 * the new checkpoint location was saved, we'd have overwritten blocks
	 * still referenced by checkpoints in the system.)  In summary, there is
	 * a second step: after our caller saves the checkpoint information, we
	 * are called to add the newly available blocks into the live system's
	 * available list.
	 *
	 * This function is the first step, the second step is in the resolve
	 * function.
	 *
	 * If we're called to checkpoint the same file twice, without the second
	 * resolution step, it's an error at an upper level and our choices are
	 * all bad: either leak blocks or risk crashing with our caller not
	 * having saved the checkpoint information to stable storage.  Leaked
	 * blocks are a safer choice, but that means file verify will fail for
	 * the rest of "forever", and the chance of us allocating a block and
	 * then crashing such that it matters is reasonably low: don't leak the
	 * blocks.
	 */
	/*���ڽ���checkpoint���������������checkpoint����ת��__wt_block_checkpoint_resolve���д���*/
	if (block->ckpt_inprogress) {
		__wt_errx(session, "%s: checkpointed without the checkpoint being resolved", block->name);

		WT_RET(__wt_block_checkpoint_resolve(session, block));
	}

	/*�ͷŵ�ԭ����ckpt_avail�е�ext list*/
	__wt_block_extlist_free(session, &ci->ckpt_avail);
	/*��ckpt_avail���и�λ*/
	WT_RET(__wt_block_extlist_init(session, &ci->ckpt_avail, "live", "ckpt_avail", 1));

	/*�ͷŵ�alloc list��discard list*/
	__wt_block_extlist_free(session, &ci->ckpt_alloc);
	__wt_block_extlist_free(session, &ci->ckpt_discard);

	deleting = 0;
	WT_CKPT_FOREACH(ckptbase, ckpt){
		/*��λ��һ��fake checkpoint*/
		if (F_ISSET(ckpt, WT_CKPT_FAKE) || !F_ISSET(ckpt, WT_CKPT_DELETE))
			continue;

		deleting = 1;

		/*��checkpointԪ��Ϣ�ж�ȡ��Ӧ��checkpoint��Ϣ*/
		if (ckpt->bpriv == NULL)
			WT_ERR(__ckpt_extlist_read(session, block, ckpt));

		/*���fake checkpoint���滹�в���fake���͵�checkpoint*/
		for (next_ckpt = ckpt + 1;; ++next_ckpt){
			if (!F_ISSET(next_ckpt, WT_CKPT_FAKE))
				break;
		}

		/*�����checkpoint��ϢҲȫ������*/
		if (next_ckpt->bpriv == NULL && !F_ISSET(next_ckpt, WT_CKPT_ADD))
			WT_ERR(__ckpt_extlist_read(session, block, next_ckpt));
	}

	/*��block��live����lock*/
	__wt_spin_lock(session, &block->live_lock);
	locked = 1;

	ckpt_size = ci->ckpt_size;
	ckpt_size += ci->alloc.bytes;
	ckpt_size -= ci->discard.bytes;

	if (!deleting)
		goto live_update;

	/*�ϲ�����ɾ��fake���͵�checkpoint*/
	WT_CKPT_FOREACH(ckptbase, ckpt){
		if (F_ISSET(ckpt, WT_CKPT_FAKE) || !F_ISSET(ckpt, WT_CKPT_DELETE))
			continue;

		if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT)) {
			if (tmp == NULL)
				WT_ERR(__wt_scr_alloc(session, 0, &tmp));

			WT_ERR(__ckpt_string(session, block, ckpt->raw.data, tmp));
			WT_ERR(__wt_verbose(session, WT_VERB_CHECKPOINT, "%s: delete-checkpoint: %s: %s", block->name, ckpt->name, (const char *)tmp->data));
		}

		for (next_ckpt = ckpt + 1;; ++next_ckpt)
			if (!F_ISSET(next_ckpt, WT_CKPT_FAKE))
				break;

		/*
		 * Set the from/to checkpoint structures, where the "to" value
		 * may be the live tree.
		 */
		a = ckpt->bpriv;
		if (F_ISSET(next_ckpt, WT_CKPT_ADD))
			b = &block->live;
		else
			b = next_ckpt->bpriv;

		/*
		 * Free the root page: there's nothing special about this free,
		 * the root page is allocated using normal rules, that is, it
		 * may have been taken from the avail list, and was entered on
		 * the live system's alloc list at that time.  We free it into
		 * the checkpoint's discard list, however, not the live system's
		 * list because it appears on the checkpoint's alloc list and so
		 * must be paired in the checkpoint.
		 */
		if (a->root_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_insert_ext(session, &a->discard, a->root_offset, a->root_size));
		/*��a�и���ext ����ȫ������block->live.avail��*/
		WT_ERR(__ckpt_extlist_fblocks(session, block, &a->alloc));
		WT_ERR(__ckpt_extlist_fblocks(session, block, &a->avail));
		WT_ERR(__ckpt_extlist_fblocks(session, block, &a->discard));

		/*��a������ext�ϲ���b��*/
		if (a->alloc.entries != 0)
			WT_ERR(__wt_block_extlist_merge(session, &a->alloc, &b->alloc));

		if (a->discard.entries != 0)
			WT_ERR(__wt_block_extlist_merge(session, &a->discard, &b->discard));

		/*
		 * If the "to" checkpoint is also being deleted, we're done with
		 * it, it's merged into some other checkpoint in the next loop.
		 * This means the extent lists may aggregate over a number of
		 * checkpoints, but that's OK, they're disjoint sets of ranges.
		 */
		if (F_ISSET(next_ckpt, WT_CKPT_DELETE))
			continue;

		/*��b�е�discard list��alloc list�ص�����ת�Ƶ�block->live.avail��*/
		WT_ERR(__wt_block_extlist_overlap(session, block, b));

		/*
		 * If we're updating the live system's information, we're done.
		 * ���checkpoint base�Ѿ�������Ϊadd״̬�ˣ�������Ϊcheckpoint*/
		if (F_ISSET(next_ckpt, WT_CKPT_ADD))
			continue;

		/*��b�и���ext ����ȫ������block->live.avail��*/
		WT_ERR(__ckpt_extlist_fblocks(session, block, &b->alloc));
		WT_ERR(__ckpt_extlist_fblocks(session, block, &b->discard));

		F_SET(next_ckpt, WT_CKPT_UPDATE);
	}

	/* Update checkpoints marked for update. */
	WT_CKPT_FOREACH(ckptbase, ckpt){
		if (F_ISSET(ckpt, WT_CKPT_UPDATE))
			WT_ERR(__ckpt_update(session, block, ckpt, ckpt->bpriv, 0));
	}

live_update:
	/*����avail�ĳ��Ƚض�block�ļ�*/
	WT_ERR(__wt_block_extlist_truncate(session, block, &ci->avail));

	WT_CKPT_FOREACH(ckptbase, ckpt){
		if (F_ISSET(ckpt, WT_CKPT_ADD)) {
			/*
			 * Set the checkpoint size for the live system.
			 *
			 * !!!
			 * Our caller wants the final checkpoint size.  Setting
			 * the size here violates layering, but the alternative
			 * is a call for the btree layer to crack the checkpoint
			 * cookie into its components, and that's a fair amount
			 * of work.
			 */
			ckpt->ckpt_size = ci->ckpt_size = ckpt_size;
			WT_ERR(__ckpt_update(session, block, ckpt, ci, 1));
		}
	}

	ci->ckpt_alloc = ci->alloc;
	WT_ERR(__wt_block_extlist_init(session, &ci->alloc, "live", "alloc", 0));
	ci->ckpt_discard = ci->discard;
	WT_ERR(__wt_block_extlist_init(session, &ci->discard, "live", "discard", 0));

	/*��ʶ���ڽ���checkpoint*/
	block->ckpt_inprogress = 1;

err:
	if(locked)
		__wt_spin_unlock(session, &block->live_lock);

	WT_CKPT_FOREACH(ckptbase, ckpt){
		if ((ci = ckpt->bpriv) != NULL)
			__wt_block_ckpt_destroy(session, ci);
	}

	__wt_scr_free(session, &tmp);
	return ret;
}

/*��ci��checkpoint��Ϣ�����ļ�д��*/
static int __ckpt_update(WT_SESSION_IMPL* session, WT_BLOCK* block, WT_CKPT* ckpt, WT_BLOCK_CKPT* ci, int is_live)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	uint8_t *endp;

	WT_RET(__wt_block_extlist_write(session, block, &ci->alloc, NULL));
	WT_RET(__wt_block_extlist_write(session, block, &ci->discard, NULL));

	/*�����availģʽ����ci��avail���й̻�*/
	if (is_live)
		WT_RET(__wt_block_extlist_write(session, block, &ci->avail, &ci->ckpt_avail));

	if (is_live)
		ci->file_size = block->fh->size;

	/*��ci��Ԫ��Ϣ�����ckpt->raw��*/
	WT_RET(__wt_buf_init(session, &ckpt->raw, WT_BTREE_MAX_ADDR_COOKIE));
	endp = ckpt->raw.mem;
	WT_RET(__wt_block_ckpt_to_buffer(session, block, &endp, ci));
	ckpt->raw.size = WT_PTRDIFF(endp, ckpt->raw.mem);

	if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT)) {
		WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__ckpt_string(session, block, ckpt->raw.data, tmp));
		WT_ERR(__wt_verbose(session, WT_VERB_CHECKPOINT, "%s: create-checkpoint: %s: %s", block->name, ckpt->name, (const char *)tmp->data));
	}

err:	
	__wt_scr_free(session, &tmp);
	return ret;
}

int __wt_block_checkpoint_resolve(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_CKPT *ci;
	WT_DECL_RET;

	ci = &block->live;

	if (!block->ckpt_inprogress)
		WT_RET_MSG(session, WT_ERROR, "%s: checkpoint resolved, but no checkpoint in progress", block->name);

	block->ckpt_inprogress = 0;

	/*��ckpt_avail�е�ext objȫ��ת��avail��*/
	__wt_spin_lock(session, &block->live_lock);
	ret = __wt_block_extlist_merge(session, &ci->ckpt_avail, &ci->avail);
	__wt_spin_unlock(session, &block->live_lock);

	/* Discard the lists remaining after the checkpoint call. */
	__wt_block_extlist_free(session, &ci->ckpt_avail);
	__wt_block_extlist_free(session, &ci->ckpt_alloc);
	__wt_block_extlist_free(session, &ci->ckpt_discard);

	return ret;
}


static int __ckpt_string(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, WT_ITEM *buf)
{
	WT_BLOCK_CKPT *ci, _ci;

	/* Initialize the checkpoint, crack the cookie. */
	ci = &_ci;
	WT_RET(__wt_block_ckpt_init(session, ci, "string"));
	WT_RET(__wt_block_buffer_to_ckpt(session, block, addr, ci));

	WT_RET(__wt_buf_fmt(session, buf,
		"version=%d",
		ci->version));

	if (ci->root_offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", root=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		", root=[%"
		PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		(uintmax_t)ci->root_offset,
		(uintmax_t)(ci->root_offset + ci->root_size),
		ci->root_size, ci->root_cksum));

	if (ci->alloc.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", alloc=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		", alloc=[%"
		PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		(uintmax_t)ci->alloc.offset,
		(uintmax_t)(ci->alloc.offset + ci->alloc.size),
		ci->alloc.size, ci->alloc.cksum));

	if (ci->avail.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", avail=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		", avail=[%"
		PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		(uintmax_t)ci->avail.offset,
		(uintmax_t)(ci->avail.offset + ci->avail.size),
		ci->avail.size, ci->avail.cksum));

	if (ci->discard.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", discard=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		", discard=[%"
		PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		(uintmax_t)ci->discard.offset,
		(uintmax_t)(ci->discard.offset + ci->discard.size),
		ci->discard.size, ci->discard.cksum));

	WT_RET(__wt_buf_catfmt(session, buf, ", file size=%" PRIuMAX, (uintmax_t)ci->file_size));

	__wt_block_ckpt_destroy(session, ci);

	return (0);
}






