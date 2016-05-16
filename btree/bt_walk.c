/************************************************************
* btree ��cursor��ǰ��������ƶ�page����ʵ��
************************************************************/
#include "wt_internal.h"

int __wt_tree_walk(WT_SESSION_IMPL* session, WT_REF** refp, uint64_t* walkcntp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *couple, *couple_orig, *ref;
	int prev, skip;
	uint32_t slot;

	btree = S2BT(session);

	/*
	* Tree walks are special: they look inside page structures that splits
	* may want to free.  Publish that the tree is active during this window.
	*/
	WT_ENTER_PAGE_INDEX(session);

	/*fast truncate �������д洢btree��ʹ��*/
	if (btree->type != BTREE_ROW)
		LF_CLR(WT_READ_TRUNCATE);

	prev = LF_ISSET(WT_READ_PREV) ? 1 : 0;
	couple = couple_orig = ref = *refp;
	*refp = NULL;

	/*ref��NULL����ʾ�Ǵ�root page��ʼ*/
	if (ref == NULL){
		ref = &btree->root;
		if (ref->page == NULL)
			goto done;
		goto descend;
	}

ascend:
	/*����ص�root page,��ʾ�Ѿ�walk��ϣ��ͷ����������ֵ�harzard pointer*/
	if (__wt_ref_is_root(ref)){
		WT_ERR(__wt_page_release(session, couple, flags));
		goto done;
	}
	/*��õ�ǰref��Ϣ��internal page��slotλ��*/
	__wt_page_refp(session, ref, &pindex, &slot);

	for (;;){
		/*�Ѿ���internal page��ĩβ��Ӧ�û���һ��internal page������*/
		if (prev && slot == 0 || (!prev && slot == pindex->entries - 1)){
			ref = ref->home->pg_intl_parent_ref; /*�ص�����internal*/

			/*ֻ�������ڵ�internal page,������internal page�ļ���*/
			if (LF_ISSET(WT_READ_SKIP_INTL))
				goto ascend;

			/*�ص�root page�ˣ���ʾ�Ѿ�walk��ϣ��ͷ����������ֵ�harzard pointer*/
			if (__wt_ref_is_root(ref))
				WT_ERR(__wt_page_release(session, couple, flags));
			else{
				__wt_page_refp(session, ref, &pindex, &slot);
				if ((ret = __wt_page_swap(session, couple, ref, flags)) != 0) { /*��ȡ���׽ڵ������溢�ӽڵ㣬Ȼ���������*/
					WT_TRET(__wt_page_release(session, couple, flags));
					WT_ERR(ret);
				}
			}
			*refp = ref;
			goto done;
		}

		/*ǰ�ƻ��ߺ���*/
		if (prev)
			--slot;
		else
			++slot;

		/*��������*/
		if (walkcntp != NULL)
			++(*walkcntp);

		for (;;){
			ref = pindex->index[slot];
			if (LF_ISSET(WT_READ_CACHE)){
				if (LF_ISSET(WT_READ_NO_WAIT) && ref->state != WT_REF_MEM)
					break;
			}
			else if (LF_ISSET(WT_READ_TRUNCATE)){
				/*ҳ����Ѿ���ɾ���ˣ�������*/
				if (ref->state == WT_REF_DELETED && __wt_delete_page_skip(session, ref))
					break;

				WT_ERR(__wt_delete_page(session, ref, &skip));
				if (skip)
					break;
			}
			else if (LF_ISSET(WT_READ_COMPACT)){
				if (ref->state == WT_REF_DELETED)
					break;

				/*���page���ڴ����ϣ���ô��Ҫ������Ƿ���Ҫcompact�������Ҫ����ô��Ҫ������*/
				if (ref->state == WT_REF_DISK) {
					WT_ERR(__wt_compact_page_skip(session, ref, &skip));
					if (skip)
						break;
				}
			}
			else{
				/*page�Ѿ���ʾɾ���ˣ�������*/
				if (ref->state == WT_REF_DELETED && __wt_delete_page_skip(session, ref))
					break;
			}
			/*��ȡref��Ӧ��page���ڴ���*/
			ret = __wt_page_swap(session, couple, ref, flags);
			if (ret == WT_NOTFOUND){
				ret = 0;
				break;
			}

			/*
			* If a new walk that never coupled from the
			* root to a new saved position in the tree,
			* restart the walk.
			*/
			if (ret == WT_RESTART){
				ret = 0;
				if (couple == &btree->root) {
					ref = &btree->root;
					if (ref->page == NULL)
						goto done;
					goto descend;
				}
				/*���»ص���ʼλ������һ��*/
				WT_ASSERT(session, couple == couple_orig || WT_PAGE_IS_INTERNAL(couple->page));
				ref = couple;
				__wt_page_refp(session, ref, &pindex, &slot);
				if (couple == couple_orig)
					break;
			}

			WT_ERR(ret);
descend:
			couple = ref;
			page = ref->page;
			/*�ڲ�����page����ȡǰһ������*/
			if (page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_COL_INT){
				WT_INTL_INDEX_GET(session, page, pindex);
				slot = prev ? pindex->entries - 1 : 0;
			}
			else{
				*refp = ref;
				goto done;
			}
		}
	}
done:
err:
	WT_LEAVE_PAGE_INDEX(session);
   return ret;
}



