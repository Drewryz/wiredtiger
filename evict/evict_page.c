
#include "wt_internal.h"

static int  __evict_page_dirty_update(WT_SESSION_IMPL *, WT_REF *, int);
static int  __evict_review(WT_SESSION_IMPL *, WT_REF *, int, int *);

/*�ͷ�page�Ķ�ռʽ����Ȩ��*/
static inline void __evict_exclusive_clear(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_ASSERT(session, ref->state == WT_REF_LOCKED && ref->page != NULL);

	ref->state = WT_REF_MEM;
}

/*���page�Ķ�ռʽ����Ȩ��*/
static inline int __evict_exclusive(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	/*
	* Check for a hazard pointer indicating another thread is using the
	* page, meaning the page cannot be evicted.
	* ͨ��hazard pionter��������߳��Ƿ��ڷ���page,����ڷ��ʣ����ܽ���evict
	*/
	if (__wt_page_hazard_check(session, ref->page) == NULL)
		return 0;

	WT_STAT_FAST_DATA_INCR(session, cache_eviction_hazard);
	WT_STAT_FAST_CONN_INCR(session, cache_eviction_hazard);
	return EBUSY;
}

/*��һ��page����evict����*/
int __wt_evict(WT_SESSION_IMPL* session, WT_REF* ref, int exclusive)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	int forced_eviction, inmem_split;

	conn = S2C(session);

	page = ref->page;
	forced_eviction = (page->read_gen == WT_READGEN_OLDEST);
	inmem_split = 0;

	WT_RET(__wt_verbose(session, WT_VERB_EVICT, "page %p (%s)", page, __wt_page_type_string(page->type)));

	/*
	* Get exclusive access to the page and review it for conditions that
	* would block our eviction of the page.  If the check fails (for
	* example, we find a page with active children), we're done.  We have
	* to make this check for clean pages, too: while unlikely eviction
	* would choose an internal page with children, it's not disallowed.
	*/
	WT_RET(__evict_review(session, ref, exclusive, &inmem_split));

	/*
	* If there was an in-memory split, the tree has been left in the state
	* we want: there is nothing more to do.
	*/
	if (inmem_split)
		goto done;

	mod = page->modify;

	/*���page��internal������session��Ӧ��cache_eviction_internal������*/
	if (!exclusive && WT_PAGE_IS_INTERNAL(page)){
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_internal);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_internal);
	}

	/*���footprint���ڴ�ռ�ô�С����evict_max_page_size���������page�ǳ�����ô��Ҫevict_max_page_size���ֵ�޸�Ϊ���page��footprint*/
	if (page->memory_footprint > conn->cache->evict_max_page_size)
		conn->cache->evict_max_page_size = page->memory_footprint;

	if (mod == NULL || !F_ISSET(mod, WT_PM_REC_MASK)){ /*�������Ѿ�reconcile�������ϣ�ֱ�ӽ�page������ڴ�*/
		if (__wt_ref_is_root(ref))/*ֱ�ӷ����ڴ��е�page�ṹ���󼴿�*/
			__wt_ref_out(session, ref);
		else
			__wt_evict_page_clean_update(session, ref);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_clean);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_clean);
	}
	else{
		if (__wt_ref_is_root(ref))
			__wt_ref_out(session, ref);
		else
			WT_ERR(__evict_page_dirty_update(session, ref, exclusive));

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_dirty);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_dirty);
	}

	if (0){
err:
		if (!exclusive)
			__evict_exclusive_clear(session, ref);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_fail);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_fail);
	}

done:
	/*����eviction server�߳���������һ��evict������Ϊ��β�û�����evict page������*/
	if ((inmem_split || (forced_eviction && ret == EBUSY)) && !F_ISSET(conn->cache, WT_CACHE_WOULD_BLOCK)) {
		F_SET(conn->cache, WT_CACHE_WOULD_BLOCK);
		WT_TRET(__wt_evict_server_wake(session));
	}

	return ret;
}

/*���ڴ��е�page�ṹ����ȫ��ɾ������������������Ҫ�ı�page��Ӧ��ref״̬*/
void __wt_evict_page_clean_update(WT_SESSION_IMPL* session, WT_REF* ref)
{
	__wt_ref_out(session, ref);
	WT_PUBLISH(ref->state, ref->addr == NULL ? WT_REF_DELETED : WT_REF_DISK);
}

/*�Ƚ�page�е�������reconcile�������ϣ���evict page���ڴ�*/
static int __evict_page_dirty_update(WT_SESSION_IMPL* session, WT_REF* ref, int exclusive)
{
	WT_ADDR *addr;
	WT_PAGE *parent;
	WT_PAGE_MODIFY *mod;

	parent = ref->home;
	mod = ref->page->modify;

	switch (F_ISSET(mod, WT_PM_REC_MASK)){
	case WT_PM_REC_EMPTY: /*û����������ҪReconcile*/
		if (ref->addr != NULL && __wt_off_page(parent, ref->addr)){ /*ref->addr��page�ռ����⿪�ٵ��ڴ棬��Ҫ�ͷ�*/
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}

		__wt_ref_out(session, ref); /*ֱ��ɾ��page�ṹ����*/
		ref->addr = NULL;
		WT_PUBLISH(ref->state, WT_REF_DELETED); /*���Ϊ���ڴ���ɾ��״̬*/
		break;

	case WT_PM_REC_MULTIBLOCK:
		/*
		* There are two cases in this code.
		*
		* First, an in-memory page that got too large, we forcibly
		* evicted it, and there wasn't anything to write. (Imagine two
		* threads updating a small set keys on a leaf page. The page is
		* too large so we try to evict it, but after reconciliation
		* there's only a small amount of data (so it's a single page we
		* can't split), and because there are two threads, there's some
		* data we can't write (so we can't evict it). In that case, we
		* take advantage of the fact we have exclusive access to the
		* page and rewrite it in memory.)
		*
		* Second, a real split where we reconciled a page and it turned
		* into a lot of pages.
		*/
		if (mod->mod_multi_entries == 1)
			WT_RET(__wt_split_rewrite(session, ref)); /*�����ºϲ���btree����,�����޸ĺ��page,evict�޸�ǰ��page����*/
		else
			WT_RET(__wt_split_multi(session, ref, exclusive));
		break;

	case WT_PM_REC_REPLACE:
		if (ref->addr != NULL && __wt_off_page(parent, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}
		/*block addr�滻*/
		WT_RET(__wt_calloc_one(session, &addr));
		*addr = mod->mod_replace;
		mod->mod_replace.addr = NULL;
		mod->mod_replace.size = 0;

		__wt_ref_out(session, ref);
		ref->addr = addr;
		WT_PUBLISH(ref->state, WT_REF_DISK);

		break;
	WT_ILLEGAL_VALUE(session);
	}

	return 0;
}

/*���parent��Ӧ��internal page�й�Ͻ��child page�Ƿ񶼴��� On-disk״̬,������ǣ�����EBUSY*/
static int __evict_child_check(WT_SESSION_IMPL* session, WT_REF* parent)
{
	WT_REF *child;

	WT_INTL_FOREACH_BEGIN(session, parent->page, child) {
		switch (child->state) {
		case WT_REF_DISK:		/* On-disk */
		case WT_REF_DELETED:	/* On-disk, deleted */
			break;
		default:
			return (EBUSY);
		}
	} WT_INTL_FOREACH_END;

	return (0);
}

/**/
static int __evict_review(WT_SESSION_IMPL* session, WT_REF* ref, int exclusive, int* inmem_splitp)
{
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	uint32_t flags;

   /*
	* Get exclusive access to the page if our caller doesn't have the tree
	* locked down.
	*/
	if (!exclusive){
		WT_RET(__evict_exclusive(session, ref));
		/*
		* Now the page is locked, remove it from the LRU eviction
		* queue.  We have to do this before freeing the page memory or
		* otherwise touching the reference because eviction paths
		* assume a non-NULL reference on the queue is pointing at
		* valid memory.
		*/
		__wt_evict_list_clear_page(session, ref);
	}
	/* Now that we have exclusive access, review the page. */
	page = ref->page;
	mod = page->modify;
	/*
	* Fail if an internal has active children, the children must be evicted
	* first. The test is necessary but shouldn't fire much: the eviction
	* code is biased for leaf pages, an internal page shouldn't be selected
	* for eviction until all children have been evicted.
	*/
	/*��֤���к��ӽڵ㶼��On-disk״̬(evited)���������ܽ���evict����ڲ�����page*/
	if (WT_PAGE_IS_INTERNAL(page)){
		WT_WITH_PAGE_INDEX(session, ret = __evict_child_check(session, ref));
		WT_RET(ret);
	}

	/* Check if the page can be evicted. */
	if (!exclusive && !__wt_page_can_evict(session, page, 0))
		return EBUSY;

	/*
	* Check for an append-only workload needing an in-memory split.
	*
	* We can't do this earlier because in-memory splits require exclusive
	* access, and we can't split if a checkpoint is in progress because
	* the checkpoint could be walking the parent page.
	*
	* If an in-memory split completes, the page stays in memory and the
	* tree is left in the desired state: avoid the usual cleanup.
	*/
	if (!exclusive) {
		WT_RET(__wt_split_insert(session, ref, inmem_splitp));
		if (*inmem_splitp)
			return 0;
	}

	flags = WT_EVICTING;
	if (__wt_page_is_modified(page)) {
		if (exclusive)
			LF_SET(WT_SKIP_UPDATE_ERR);
		else if (!WT_PAGE_IS_INTERNAL(page) && page->read_gen == WT_READGEN_OLDEST)
			LF_SET(WT_SKIP_UPDATE_RESTORE);

		/*����page��reconcile������*/
		WT_RET(__wt_reconcile(session, ref, NULL, flags));
		WT_ASSERT(session, !__wt_page_is_modified(page) || LF_ISSET(WT_SKIP_UPDATE_RESTORE));
	}

	if (!exclusive && mod != NULL && !__wt_txn_visible_all(session, mod->rec_max_txn) && !LF_ISSET(WT_SKIP_UPDATE_RESTORE))
		return EBUSY;

	return 0;
}




