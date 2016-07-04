/***********************************************************************
* btree page discardʵ��
*
*
***********************************************************************/
#include "wt_internal.h"


static void __free_page_modify(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_skip_array(WT_SESSION_IMPL *, WT_INSERT_HEAD **, uint32_t);
static void __free_skip_list(WT_SESSION_IMPL *, WT_INSERT *);
static void __free_update(WT_SESSION_IMPL *, WT_UPDATE **, uint32_t);
static void __free_update_list(WT_SESSION_IMPL *, WT_UPDATE *);

/*����һ���ڴ��е�btree page�����ͷ���֮�й�����ڴ�*/
void __wt_ref_out(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_ASSERT(session, S2BT(session)->evict_ref != ref);
	__wt_page_out(session, &ref->page);
}

/*����btree page�ڴ��еĶ���*/
void __wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_PAGE_HEADER *dsk;
	WT_PAGE_MODIFY *mod;

	/*�Ƚ�pagep��ֵ��ΪNULL����ֹ�����ط����ͷŵ�ʱ��ʹ��*/
	page = *pagep;
	*pagep = NULL;

	/*�Ϸ����жϣ����ͷŵ�page��������ҳ������������split��ҳ*/
	WT_ASSERT(session, !__wt_page_is_modified(page));
	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU));
	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_SPLITTING));

	switch(page->type){
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		mod = page->modify;
		if(mod != NULL && mod->mod_root_split != NULL) /*���root page��split���ģ������и����page��Ҫ������page��������Ҫȫ���ҳ���������*/
			__wt_page_out(session, &mod->mod_root_split);
		break;
	}

	/*��page������ڴ�cache*/
	__wt_cache_page_evict(session, page);

	/*����discarded page�ǲ��ͷŵģ���ôֱ�ӷ��أ�����������ڴ�й¶*/
	if(F_ISSET(S2C(session), WT_CONN_LEAK_MEMORY))
		return ;

	switch(page->type){
	case WT_PAGE_COL_FIX:
		break;

	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		__free_page_int(session, page);
		break;

	case WT_PAGE_COL_VAR:
		__free_page_col_var(session, page);
		break;

	case WT_PAGE_ROW_LEAF:
		__free_page_row_leaf(session, page);
		break;
	}

	dsk = (WT_PAGE_HEADER *)page->dsk;
	/*�Ѿ�������dsk�ռ������Ҫ�����ͷ�*/
	if(F_ISSET_ATOMIC(page, WT_PAGE_DISK_ALLOC))
		__wt_overwrite_and_free_len(session, dsk, dsk->mem_size);
	
	/*page�Ѿ��ڴ����������˴洢�ռ䣬���пռ��ͷ�*/
	if(F_ISSET_ATOMIC(page, WT_PAGE_DISK_MAPPED))
		__wt_mmap_discard(session, dsk, dsk->mem_size);

	__wt_overwrite_and_free(session, page);
}

/*�ͷŷ���page����ص��޸ĵ����ݶ���*/
static void __free_page_modify(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_INSERT_HEAD *append;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	mod = page->modify;

	/*��¼���޸�,��Ҫ�ͷŶ�Ӧ���޸Ķ���*/
	switch(F_ISSET(mod, WT_PM_REC_MASK)){
	case WT_PM_REC_MULTIBLOCK:
		/*�ͷ�modify��entry����*/
		for(multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i){
			switch(page->type){
				/*������д洢��ʽ����Ҫ���ͷ�keyֵ*/
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				__wt_free(session, multi->key.ikey);
				break;
			}

			__wt_free(session, multi->skip);
			__wt_free(session, multi->skip_dsk);
			__wt_free(session, multi->addr.addr);
		}
		__wt_free(session, mod->mod_multi);
		break;

		/*��¼�����滻ģ��,ֱ���ͷż���*/
	case WT_PM_REC_REPLACE:
		__wt_free(session, mod->mod_replace.addr);
		break;
	}

	switch(page->type){
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		/*�ͷ�append����*/
		if((append == WT_COL_APPEND(page)) != NULL){
			__free_skip_list(session, WT_SKIP_FIRST(append));
			__wt_free(session, append);
			__wt_free(session, mod->mod_append);
		}

		/*�ͷŵ�insert/update��������*/
		if(mod->mod_update != NULL){
			__free_skip_array(session, mod->mod_update, page->type == WT_PAGE_COL_FIX ? 1 : page->pg_var_entries);
		}

		break;
	}

	/*�ͷ����page�Ľṹ����*/
	__wt_ovfl_reuse_free(session, page);
	__wt_ovfl_txnc_free(session, page);
	__wt_ovfl_discard_free(session, page);
	__wt_free(session, page->modify->ovfl_track);

	__wt_free(session, page->modify);
}

/*�ͷ�һ���ڲ�����page, ����Ϊ��WT_PAGE_COL_INT����WT_PAGE_ROW_INT*/
static void __free_page_int(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	__wt_free_ref_index(session, page, WT_INTL_INDEX_GET_SAFE(page), 0);
}

/*����һ��page��Ӧ��ref����*/
void __wt_free_ref(WT_SESSION_IMPL* session, WT_PAGE* page, WT_REF* ref, int free_pages)
{
	WT_IKEY *ikey;

	if (ref == NULL)
		return;

	/*ȷ����Ҫ�ͷŵ���Ӧ��page,��ô�ȸı��Ӧ��cache�������ݳ��ȣ�Ȼ��page�����ͷ�*/
	if(free_pages && ref->page != NULL){
		if(ref->page->modify != NULL){
			ref->page->modify->write_gen = 0;
			__wt_cache_dirty_decr(session, ref->page);
		}

		__wt_page_out(session, &ref->page);
	}

	/*�ͷ�key*/
	switch(page->type){
		/*�д洢���ͷ�ikey����*/
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if ((ikey = __wt_ref_key_instantiated(ref)) != NULL)
			__wt_free(session, ikey);
		break;
	}

	/*���ref->addr����page�ռ��ϣ��ǵ������ٵ��ڴ�ռ䣬��Ҫ�����ͷ�*/
	if(ref->addr != NULL && __wt_off_page(page, ref->addr)){
		__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
		__wt_free(session, ref->addr);
	}

	if(ref->page_del != NULL){
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	__wt_overwrite_and_free(session, ref);
}

/*�ͷ�index entry����*/
void __wt_free_ref_index(WT_SESSION_IMPL* session, WT_PAGE* page, WT_PAGE_INDEX* pindex, int free_pages)
{
	uint32_t i;

	if (pindex == NULL)
		return;

	for (i = 0; i < pindex->entries; ++i)
		__wt_free_ref(session, page, pindex->index[i], free_pages);
	__wt_free(session, pindex);
}

/*����һ��WT_PAGE_COL_VAR page����*/
static void __free_page_col_var(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	__wt_free(session, page->pg_var_repeats);
}

/*����һ���д洢��leaf page*/
static void __free_page_row_leaf(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_IKEY *ikey;
	WT_ROW *rip;
	uint32_t i;
	void *copy;

	WT_ROW_FOREACH(page, rip, i){
		copy = WT_ROW_KEY_COPY(rip);
		(void)__wt_row_leaf_key_info(page, copy, &ikey, NULL, NULL, NULL);
		if (ikey != NULL)
			__wt_free(session, ikey);
	}

	if (page->pg_row_ins != NULL)
		__free_skip_array(session, page->pg_row_ins, page->pg_row_entries + 1);

	if (page->pg_row_upd != NULL)
		__free_update(session, page->pg_row_upd, page->pg_row_entries);

}

/*����header��skip array*/
static void __free_skip_array(WT_SESSION_IMPL* session, WT_INSERT_HEAD** head_arg, uint32_t entries)
{
	WT_INSERT_HEAD **head;

	for (head = head_arg; entries > 0; --entries, ++head){
		if (*head != NULL) {
			__free_skip_list(session, WT_SKIP_FIRST(*head));
			__wt_free(session, *head);
		}
	}

	__wt_free(session, head_arg);
}

/*����insert list����*/
static void __free_skip_list(WT_SESSION_IMPL* session, WT_INSERT* ins)
{
	WT_INSERT *next;
	for (; ins != NULL; ins = next) {
		__free_update_list(session, ins->upd);
		next = WT_SKIP_NEXT(ins);
		__wt_free(session, ins);
	}
}

/*����һ��update list*/
static void __free_update(WT_SESSION_IMPL *session, WT_UPDATE **update_head, uint32_t entries)
{
	WT_UPDATE **updp;
	for (updp = update_head; entries > 0; --entries, ++updp)
		if (*updp != NULL)
			__free_update_list(session, *updp);

	__wt_free(session, update_head);
}

static void __free_update_list(WT_SESSION_IMPL* session, WT_UPDATE* upd)
{
	WT_UPDATE *next;

	for (; upd != NULL; upd = next) {
		/* Everything we free should be visible to everyone. */
		WT_ASSERT(session, F_ISSET(session, WT_SESSION_DISCARD_FORCE) ||
			upd->txnid == WT_TXN_ABORTED || __wt_txn_visible_all(session, upd->txnid));

		next = upd->next;
		__wt_free(session, upd);
	}
}

