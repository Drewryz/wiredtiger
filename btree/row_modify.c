/**********************************************************************
 *ʵ��row store��ʽ��btree���������޸ĺ�ɾ����ʽ
 **********************************************************************/

#include "wt_internal.h"

/* ���ڴ��з���һ��page���޸Ķ��� */
int __wt_page_modify_alloc(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_CONNECTION_IMPL *conn;
	WT_PAGE_MODIFY *modify;

	conn = S2C(session);

	WT_RET(__wt_calloc_one(session, &modify));

	/* Ϊmodify����ѡ��һ��spin lock,���ڶ��߳̾����������� */
	modify->page_lock = ++conn->page_lock_cnt % WT_PAGE_LOCKS(conn);

	/* �п��ܶ���߳��ڽ��з��䣬ֻ��һ���̻߳ᴴ���ɹ�,�����̴߳����Ķ�����Ҫ�ͷŵ� */
	if(WT_ATOMIC_CAS8(page->modify, NULL, modify))
		__wt_cache_page_inmem_incr(session, page, sizeof(*modify));
	else
		__wt_free(session, modify);

	return 0;
}

/*row store btree���޸�ʵ�֣�����:insert, update��delete */
int __wt_row_modify(WT_SESSION_IMPL* session, WT_CURSOR_BTREE* cbt, WT_ITEM* key, WT_ITEM* value, WT_UPDATE* upd, int is_remove)
{
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *ins_head, **ins_headp;
	WT_PAGE *page;
	WT_UPDATE *old_upd, **upd_entry;
	size_t ins_size, upd_size;
	uint32_t ins_slot;
	u_int i, skipdepth;
	int logged;

	ins = NULL;
	page = cbt->ref->page;
	logged = 0;

	if(is_remove)
		value = NULL;

	/*���䲢��ʼ��һ��page modify����*/
	WT_RET(__wt_page_modify_init(session, page));

	/* �޸Ĳ����� */
	if(cbt->compare == 0){
		if (cbt->ins == NULL){ /*����update list�����ҵ��޸ĵļ�¼��ֱ�ӻ�ȡudpate list���ж�Ӧ��update����*/
			/* Ϊcbt�α����һ��update��������,������update�����λ��ԭ�Ӳ����Եķ���*/
			WT_PAGE_ALLOC_AND_SWAP(session, page, page->pg_row_upd, upd_entry, page->pg_row_entries);
			upd_entry = &page->pg_row_upd[cbt->slot];
		}
		else /* ��ȡһ��upd_entry */
			upd_entry = &cbt->ins->upd;

		if(upd == NULL){
			/*ȷ���Ƿ���Խ��и��²���*/
			WT_ERR(__wt_txn_update_check(session, old_upd = *upd_entry));

			WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
			WT_ERR(__wt_txn_modify(session, upd));
			logged = 1;

			/* Avoid WT_CURSOR.update data copy. */
			cbt->modify_update = upd;
		}
		else{
			upd_size = __wt_update_list_memsize(upd);

			WT_ASSERT(session, *upd_entry == NULL);
			old_upd = *upd_entry = upd->next;
		}
		/*
		 * Point the new WT_UPDATE item to the next element in the list.
		 * If we get it right, the serialization function lock acts as
		 * our memory barrier to flush this write.
		 */
		upd->next = old_upd;

		/*���д��и��²���*/
		WT_ERR(__wt_update_serial(session, page, upd_entry, &upd, upd_size));
	}
	else{ /*û�ж�λ������ļ�¼����ô�൱�ڲ���һ���µļ�¼��*/
		WT_PAGE_ALLOC_AND_SWAP(session, page, page->pg_row_ins, ins_headp, page->pg_row_entries + 1);
		ins_slot = F_ISSET(cbt, WT_CBT_SEARCH_SMALLEST) ? page->pg_row_entries : cbt->slot;
		ins_headp = &page->pg_row_ins[ins_slot];

		/*����һ��insert head����*/
		WT_PAGE_ALLOC_AND_SWAP(session, page, *ins_headp, ins_head, 1);
		ins_head = *ins_headp;

		skipdepth = __wt_skip_choose_depth(session);

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and
		 * update the cursor to reference it (the WT_INSERT_HEAD might
		 * be allocated, the WT_INSERT was allocated).
		 */
		WT_ERR(__wt_row_insert_alloc(session, key, skipdepth, &ins, &ins_size));
		cbt->ins_head = ins_head;
		cbt->ins = ins;

		/*ͨ��value����upd����*/
		if(upd == NULL){
			WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
			WT_ERR(__wt_txn_modify(session, upd));
			logged = 1;

			/* Avoid WT_CURSOR.update data copy. */
			cbt->modify_update = upd;
		}
		else 
			upd_size = __wt_update_list_memsize(upd);

		ins->upd = upd;
		ins_size += upd_size;

		if (WT_SKIP_FIRST(ins_head) == NULL)
			for (i = 0; i < skipdepth; i++) {
				cbt->ins_stack[i] = &ins_head->head[i];
				ins->next[i] = cbt->next_stack[i] = NULL;
			}
		else
			for (i = 0; i < skipdepth; i++)
				ins->next[i] = cbt->next_stack[i];

		/* Insert the WT_INSERT structure. */
		WT_ERR(__wt_insert_serial(session, page, cbt->ins_head, cbt->ins_stack, &ins, ins_size, skipdepth));
	}
	
	if(logged)
		WT_ERR(__wt_txn_log_op(session, cbt));

	if(0){
err:
		if (logged)
			__wt_txn_unmodify(session);

		__wt_free(session, ins);
		cbt->ins = NULL;
		__wt_free(session, upd);
	}

	return ret;
}

/* ����һ��row insert��WT_INSERT���� */
int __wt_row_insert_alloc(WT_SESSION_IMPL* session, WT_ITEM* key, u_int skipdepth, WT_INSERT** insp, size_t* ins_sizep)
{
	WT_INSERT *ins;
	size_t ins_size;

	ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *) + key->size;
	WT_RET(__wt_calloc(session, 1, ins_size, &ins));

	/*ȷ��key����ʼƫ��λ��*/
	ins->u.key.offset = WT_STORE_SIZE(ins_size - key->size);
	/*����key�ĳ���*/
	WT_INSERT_KEY_SIZE(ins) = WT_STORE_SIZE(key->size);
	/*keyֵ�Ŀ���*/
	memcpy(WT_INSERT_KEY(ins), key->data, key->size);

	insp = ins;
	if(ins_sizep != NULL)
		*ins_sizep = ins_size;

	return 0;
}

/*����һ��row update��WT_UPDATE����, value = NULL��ʾdelete����*/
int __wt_update_alloc(WT_SESSION_IMPL* session, WT_ITEM* value, WT_UPDATE** updp, size_t* sizep)
{
	WT_UPDATE *upd;
	size_t size;

	size = (value == NULL ? 0 : value->size);
	WT_RET(__wt_calloc(session, 1, sizeof(WT_UPDATE) + size, &upd));
	if (value == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = WT_STORE_SIZE(size);
		memcpy(WT_UPDATE_DATA(upd), value->data, size);
	}

	*updp = upd;
	*sizep = WT_UPDATE_MEMSIZE(upd);
	return 0;
}

/* �����ڷ�����update������update listȥ����������ȥ�����update��һ��update��Ԫ */
WT_UPDATE* __wt_update_obsolete_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_UPDATE *first, *next;


	for(first = NULL; upd != NULL; upd = upd->next){
		if(__wt_txn_visible_all(session, upd->txnid)){
			if(first == NULL)
				first = upd;
		}
		else if(upd->txnid != WT_TXN_ABORTED)
			first = NULL;
	}

	/*�ص��������һ����Ϊ�յ�upd����TXNID = WT_TXN_ABORTED���������upd���������񲻿ɼ�*/
	if(first != NULL &&& (next = first->next) != NULL && WT_ATOMIC_CAS8(first->next, next, NULL))
		return next;

	return NULL;
}

/*�ͷ�upd list�еĶ���,��Щ����ȷ���Ǳ����ڷ�����*/
void __wt_update_obsolete_free(WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd)
{
	WT_UPDATE *next;
	size_t size;

	/* Free a WT_UPDATE list. */
	for (size = 0; upd != NULL; upd = next) {
		next = upd->next;
		size += WT_UPDATE_MEMSIZE(upd);
		__wt_free(session, upd);
	}

	if (size != 0)
		__wt_cache_page_inmem_decr(session, page, size);
}






