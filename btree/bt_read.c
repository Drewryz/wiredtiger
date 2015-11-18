/****************************************************
* ���ļ��ж�ȡһ��page�����ݵ��ڴ��в��������ڴ����
****************************************************/
#include "wt_internal.h"

int __wt_cache_read(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *page;
	WT_PAGE_STATE previous_state;
	size_t addr_size;
	const uint8_t *addr;

	page = NULL;

	WT_CLEAR(tmp);

	/*���ref�Ƿ��Ѿ���ʼ��page�Ĵ������ݽ��ж�ȡ*/
	if (WT_ATOMIC_CAS4(ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (WT_ATOMIC_CAS4(ref->state, WT_REF_DELETED, WT_REF_LOCKED)) /*���ref��״̬�Ƿ���deleted������ǣ���lock��*/
		previous_state = WT_REF_DELETED;
	else
		return 0;

	/*���page��block address �� cookie*/
	WT_ERR(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
	if (addr == NULL){ /*û�ҵ�block addr,ֱ���½�һ��page*/
		WT_ASSERT(session, previous_state == WT_REF_DELETED);
		/*�½�һ��leaf page�ڴ����*/
		WT_ERR(__wt_btree_new_leaf_page(session, &page));
		ref->page = page;
	}
	else{
		/*�Ӵ����ļ��Ͻ�page���ݶ����ڴ�*/
		WT_ERR(__wt_bt_read(session, &tmp, addr, addr_size));
		/*����page������֯�ṹ�Ͷ���*/
		WT_ERR(__wt_page_inmem(session, ref, tmp.data, tmp.memsize, WT_DATA_IN_ITEM(&tmp) ? WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page));
		tmp.mem = NULL;

		/*���page�Ѿ�����ɾ��״̬����BTREE����ɾ�������page�Ľڵ�*/
		if (previous_state == WT_REF_DELETED)
			WT_ERR(__wt_delete_page_instantiate(session, ref));
	}

	WT_ERR(__wt_verbose(session, WT_VERB_READ, "page %p: %s", page, __wt_page_type_string(page->type)));
	/*��page���ڴ�״̬��Ч������ҳ��*/
	WT_PUBLISH(ref->state, WT_REF_MEM);

	return 0;

err:
	/*�ڶ�ȡ�����д����ˣ���page�����cache���ع�����һ��ref״̬*/
	if (ref->page != NULL)
		__wt_page_out(session, page);

	WT_PUBLISH(ref->state, previous_state);
	/*�ͷŵ�__wt_bt_read��ȡ��blockʱ������ڴ�飬����page�����ɹ����ǲ����ͷ������ģ���Ϊpage��ӳ�����������*/
	__wt_buf_free(session, &tmp);

	return ret;
}
