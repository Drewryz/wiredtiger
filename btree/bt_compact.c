/****************************************************************
* btree��compact����ʵ��
****************************************************************/
#include "wt_internal.h"

/*��������ļ��ռ�compact�����ʵ�λ��,���ref��compact��Χ�ڣ�����skip = 1,��ʾ�ļ��ռ䲻�ܽ���compact*/
static int __compact_rewrite(WT_SESSION_IMPL* session, WT_REF* ref, int* skipp)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	size_t addr_size;
	const uint8_t *addr;

	*skipp = 1;	

	bm = S2BT(session)->bm;
	page = ref->page;
	mod = page->modify;

	/*root page�ǲ��ܱ�compact*/
	if (__wt_ref_is_root(ref))
		return 0;

	/*refָ����Ǹ���ҳ��������compact*/
	if (__wt_page_is_modified(page))
		return (0);

	/*����pageһ�Ѿ�����յģ�ֱ���ж��Ƿ��������block�ռ�compact*/
	if (mod == NULL || F_ISSET(mod, WT_PM_REC_MASK) == 0) {
		WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
		if (addr == NULL)
			return (0);
		WT_RET(bm->compact_page_skip(bm, session, addr, addr_size, skipp));
	}
	else if (F_ISSET(mod, WT_PM_REC_MASK) == WT_PM_REC_REPLACE){ /*���page�ռ����滻����ô�����滻block��compact�����ж�*/
		WT_PAGE_LOCK(session, page);
		ret = bm->compact_page_skip(bm, session, mod->mod_replace.addr, mod->mod_replace.size, skipp);
		WT_PAGE_UNLOCK(session, page);
		WT_RET(ret);
	}

	return 0;
}

/*���ļ�����compact����*/
int __wt_compact(WT_SESSION_IMPL* session, const char* cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_REF *ref;
	int block_manager_begin, evict_reset, skip;

	WT_UNUSED(cfg);

	conn = S2C(session);
	btree = S2BT(session);
	bm = btree->bm;
	ref = NULL;
	block_manager_begin = 0;

	WT_STAT_FAST_DATA_INCR(session, session_compact);

	/*���bm����Ӧ��blocks�Ƿ����compact,��������ԣ�ֱ�ӷ���*/
	WT_RET(bm->compact_skip(bm, session, &skip));
	if (skip)
		return 0;

	/*
	* Reviewing in-memory pages requires looking at page reconciliation
	* results, because we care about where the page is stored now, not
	* where the page was stored when we first read it into the cache.
	* We need to ensure we don't race with page reconciliation as it's
	* writing the page modify information.
	*
	* There are three ways we call reconciliation: checkpoints, threads
	* writing leaf pages (usually in preparation for a checkpoint or if
	* closing a file), and eviction.
	*
	* We're holding the schema lock which serializes with checkpoints.
	*/
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*���btree flusk_lock,��ֹ���ļ��ռ�compact�������߳�flush*/
	__wt_spin_lock(session, &btree->flush_lock);

	conn->compact_in_memory_pass = 1;
	WT_ERR(__wt_evict_file_exclusive_on(session, &evict_reset));
	if (evict_reset)
		__wt_evict_file_exclusive_off(session);

	WT_ERR(bm->compact_start(bm, session));
	block_manager_begin = 1;

	session->compaction = 1;
	for (;;){
		
		WT_ERR(__wt_tree_walk(session, &ref, NULL, WT_READ_COMPACT | WT_READ_NO_GEN | WT_READ_WONT_NEED));
		if (ref == NULL)
			break;

		/*����compact���*/
		WT_ERR(__compact_rewrite(session, ref, &skip));
		if (skip)
			continue;

		/*�����Ҫcompact��page��Ҫ���Ϊ��page,ͨ���ڴ���������дcompact���*/
		WT_ERR(__wt_page_modify_init(session, ref->page));
		__wt_page_modify_set(session, ref->page);

		WT_STAT_FAST_DATA_INCR(session, btree_compact_rewrite);
	}

err:
	if (ref != NULL)
		WT_TRET(__wt_page_release(session, ref, 0));

	/*����compact����*/
	if (block_manager_begin)
		WT_TRET(bm->compact_end(bm, session));

	/*
	 * Unlock will be a release barrier, use it to update the compaction
	 * status for reconciliation.
	 */
	conn->compact_in_memory_pass = 0;
	__wt_spin_unlock(session, &btree->flush_lock);

	return ret;
}

/*�ڶ�ȡref��Ӧ��pageʱ��������Ƿ���Ҫcompact*/
int __wt_compact_page_skip(WT_SESSION_IMPL* session, WT_REF* ref, int* skipp)
{
	WT_BM *bm;
	size_t addr_size;
	u_int type;
	const uint8_t *addr;

	*skipp = 0;				
	type = 0;

	bm = S2BT(session)->bm;

	/*
	* We aren't holding a hazard pointer, so we can't look at the page
	* itself, all we can look at is the WT_REF information.  If there's no
	* address, the page isn't on disk, but we have to read internal pages
	* to walk the tree regardless; throw up our hands and read it.
	*/
	WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, &type));
	if (addr == NULL)
		return 0;

	return (type == WT_CELL_ADDR_INT ? 0 : bm->compact_page_skip(bm, session, addr, addr_size, skipp));
}




