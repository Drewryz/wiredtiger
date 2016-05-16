/***********************************************************************
* ����ļ���Ҫ��ʵ��btree page�ı��ɾ�����ܣ�fast-deleted������Ҫ����ɾ��
* ��ʱ���page->state���ΪWT_REF_DELETED,������page walk��skip���page
*
***********************************************************************/
#include "wt_internal.h"

/*����ɾ��һ��page,���page������û�б��޸Ĺ���,���ڴ��е����ݺʹ����ϵ�������һ�µ�*/
int __wt_delete_page(WT_SESSION_IMPL* session, WT_REF* ref, int* skipp)
{
	WT_DECL_RET;
	WT_PAGE *parent;

	*skipp = 0;

	/*�������page�Ѿ���ʵ�������ڴ��У����ǳ��Դ�cache��������������Ҫ��ref����Ϊlocked״̬����ֹ����������в���*/
	if(ref->state == WT_REF_MEM && WT_ATOMIC_CAS4(ref->state, WT_REF_MEM, WT_REF_LOCKED)){
		if (__wt_page_is_modified(ref->page)) {
			WT_PUBLISH(ref->state, WT_REF_MEM);
			return (0);
		}

		/*����page����*/
		WT_ATOMIC_ADD4(S2BT(session)->evict_busy, 1);
		ret = __wt_evict_page(session, ref);
		WT_ATOMIC_SUB4(S2BT(session)->evict_busy, 1);

		WT_RET_BUSY_OK(ret);
	}

	/*�������page��Ӧref->stateһ����WT_REF_DISK,������ǣ�˵����ʵ��������ʹ���������ܽ���ɾ�����*/
	if(ref->state != WT_REF_DISK || !WT_ATOMIC_CAS4(ref->state, WT_REF_DISK, WT_REF_LOCKED))
		return 0;

	/*���page����overflow��KV�ԣ����ܽ���fast delete����ֻ��ͨ��page discard��ɾ��,�����漰�Ķ����Ƚϸ��ӣ���ʱû̫Ū����*/
	parent = ref->home;
	if(__wt_off_page(parent, ref->addr) || __wt_cell_type_raw(ref->addr) != WT_CELL_ADDR_LEAF_NO)
		goto err;

	/*��Ϊ�ӽڵ�Ҫ���Ϊɾ�����������ĸ��ڵ���Ҫ���Ϊ��ҳ*/
	WT_ERR(__wt_page_parent_modify_set(session, ref, 0));

	/*��ref�ϲ���һ��page_del�ṹ����ʾref��Ӧ��pageɾ����*/
	WT_ERR(__wt_calloc_one(session, &ref->page_del));
	ref->page_del->txnid = session->txn.id;

	WT_ERR(__wt_txn_modify_ref(session, ref));

	*skipp = 1;
	/*��page ref�ϱ��Ϊɾ��״̬*/
	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return 0;

err:
	__wt_free(session, ref->page_del);
	/*��ref�ϱ��ΪWT_REF_DISK����Ϊ���ʱ��page�������cache��*/
	WT_PUBLISH(ref->state, WT_REF_DISK);
	return ret;
}

/*����ع�range deleted,�ָ�deleted��page*/
void __wt_delete_page_rollback(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_UPDATE **upd;

	for(;; __wt_yield()){
		switch(ref->state){
		case WT_REF_DISK:
		case WT_REF_READING:
			WT_ASSERT(session, 0);
			break;

		case WT_REF_DELETED:
			/*page�Ѿ�ɾ����������Ҫ���������Ա��ظ�ʹ�ã���*/
			if(WT_ATOMIC_ADD4(ref->state, WT_REF_DELETED, WT_REF_DISK))
				return ;
			break;

		case WT_REF_LOCKED: /*page����ʵ�������������״̬����Ϊ����page��block�������첽��*/
			break;

		case WT_REF_MEM:
		case WT_REF_SPLIT:
			/*
			 * We can't use the normal read path to get a copy of
			 * the page because the session may have closed the
			 * cursor, we no longer have the reference to the tree
			 * required for a hazard pointer.  We're safe because
			 * with unresolved transactions, the page isn't going
			 * anywhere.
			 *
			 * The page is in an in-memory state, walk the list of
			 * update structures and abort them.
			 * ���µ���˼��˵���ڴ��е�page����һ��������TXN ID,�����
			 * ���delete page�Ĳ����Ѿ����ع����ָ���ɾ��ǰ��״̬��
			 * ��ֹ�������������ȡ���Ĳ��������
			 */
			for (upd =ref->page_del->update_list; *upd != NULL; ++upd)
				(*upd)->txnid = WT_TXN_ABORTED;

			/*�ͷŵ��洢upd��list��page_del*/
			__wt_free(session, ref->page_del->update_list);
			__wt_free(session, ref->page_del);

			return ;
		}
	}
}

/*
 * __wt_delete_page_skip --
 *	If iterating a cursor, skip deleted pages that are visible to us.
 * ��btree cursor����btreeʱ����Ҫ�ж�page�Ƿ���ɾ�������ɾ���ˣ�
 * ��Ҫskip ��
 */
int __wt_delete_page_skip(WT_SESSION_IMPL *session, WT_REF *ref)
{
	int skip; 

	/*page�Ѿ�ɾ��������skip*/
	if(ref->page_del == NULL)
		return 1;

	/*������deleted״̬,����skip��*/
	if (!WT_ATOMIC_CAS4(ref->state, WT_REF_DELETED, WT_REF_LOCKED))
		return 0;

	/*page del��null,��ʾ�Ѿ�û�������ط���������������������Ƕ����page�ɼ�����ô��ʾ���page��session��˵��ɾ���ģ�Ӧ��skip*/
	skip = (ref->page_del == NULL || __wt_txn_visible(session, ref->page_del->txnid));

	WT_PUBLISH(ref->state, WT_REF_DELETED);

	return skip;
}

/*ʵ����һ��deleted row leaf page,�������ɾ���İ汾������*/
int __wt_delete_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_DELETED *page_del;
	WT_UPDATE **upd_array, *upd;
	size_t size;
	uint32_t i;

	btree = S2BT(session);
	page = ref->page;
	page_del = ref->page_del;

	/*���btree���Ϊ�����������Ҫ��page����д����ô������pageΪ��ҳ*/
	if(btree->modified){
		WT_RET(__wt_page_modify_init(session, page));
		__wt_page_modify_set(session, page);
	}

	/*page�Ѿ����Ϊdeleted,��ô��ҪΪpage���һ���汾�����ڴ�ʵ����*/
	if(page_del != NULL){
		WT_RET(__wt_calloc_def(session, page->pg_row_entries + 1, &page_del->update_list));
	}

	WT_ERR(__wt_calloc_def(session, page->pg_row_entries, &upd_array));
	page->pg_row_upd = upd_array;

	/*����upd��txnid������һ������deleted page��mvcc��¼����*/
	for (i = 0, size = 0; i < page->pg_row_entries; ++i) {
		WT_ERR(__wt_calloc_one(session, &upd));
		WT_UPDATE_DELETED_SET(upd);

		if (page_del == NULL) /*���page��������ɾ������ô������еļ�¼�����е�����ɼ�*/
			upd->txnid = WT_TXN_NONE;	/* Globally visible */
		else {
			upd->txnid = page_del->txnid;
			page_del->update_list[i] = upd;
		}

		upd->next = upd_array[i];
		upd_array[i] = upd;

		size += sizeof(WT_UPDATE *) + WT_UPDATE_MEMSIZE(upd);
	}

	__wt_cache_page_inmem_incr(session, page, size);
}





