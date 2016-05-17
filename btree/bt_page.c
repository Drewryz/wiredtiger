#include "wt_internal.h"

static void __inmem_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int  __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int  __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_row_leaf_entries(WT_SESSION_IMPL *, const WT_PAGE_HEADER *, uint32_t *);

/*���һ��page�Ƿ����ǿ��������̭�����ڴ�*/
static int __evict_force_check(WT_SESSION_IMPL* session, WT_PAGE* page, uint32_t flags)
{
	WT_BTREE* btree;
	btree = S2BT(session);

	/*page���ڴ�û�дﵽ�������ֵ�����Բ��ñ�������ڴ�*/
	if (page->memory_footprint < btree->maxmempage)
		return 0;

	/*����page�ݲ�����ֻ����leaf page*/
	if (WT_PAGE_IS_INTERNAL(page))
		return 0;

	/*���𱻹ر�,����������*/
	if (LF_ISSET(WT_READ_NO_EVICT) || F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return 0;

	/*û̫Ū����Ϊʲôû���޸ĵ�page��������*/
	if (page->modify == NULL)
		return 0;

	/*��������page����,����page������״̬*/
	__wt_page_evict_soon(page);

	/*�ж��Ƿ����ֱ������*/
	return __wt_page_can_evict(session, page, 1);
}

/*����ref��Ӧ��page���һ��hazard pointer,���page���ڴ����ϣ���page����memory��*/
int __wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_DECL_RET;
	WT_PAGE *page;
	u_int sleep_cnt, wait_cnt;
	int busy, force_attempts, oldgen;

	for (force_attempts = oldgen = 0, wait_cnt = 0;;){
		switch (ref->state){
		case WT_REF_DISK:
		case WT_REF_DELETED:
			/*page�Ѿ�����cache read������read*/
			if (LF_ISSET(WT_READ_CACHE))
				return WT_NOTFOUND;
			/*���lru cache�е������Ƿ������������95%��ռ���ʣ���Ҫ����lru��̭���ٽ��е�ǰ����page�Ķ�ȡ*/
			WT_RET(__wt_cache_full_check(session));
			WT_RET(__wt_cache_read(session, ref));
			oldgen = LF_ISSET(WT_READ_WONT_NEED) || F_ISSET(session, WT_SESSION_NO_CACHE);
			continue;

		case WT_REF_READING:
			if (LF_ISSET(WT_READ_CACHE))
				return WT_NOTFOUND;
			if (LF_ISSET(WT_READ_NO_WAIT))
				return WT_NOTFOUND;
			WT_STAT_FAST_CONN_INCR(session, page_read_blocked);
			break;

		case WT_REF_LOCKED:
			if (LF_ISSET(WT_READ_NO_WAIT))
				return WT_NOTFOUND;
			WT_STAT_FAST_CONN_INCR(session, page_locked_blocked);
			break;

		case WT_REF_SPLIT:
			return WT_RESTART;

			/*�Ѿ������ڴ�����*/
		case WT_REF_MEM:
			WT_RET(__wt_hazard_set(session, ref, &busy)); /*���page����memory����Ҫռ��һ��hazard pointer�����ռ��hazard pointerʱ����busy����ʾ���page�����Ѿ����ڱ�����,��Ҫ����*/
			if (busy){
				WT_STAT_FAST_CONN_INCR(session, page_busy_blocked);
				break;
			}

			/*�ߵ���һ����˵��page�޷����һ��hazard pointer����ôӦ�ÿ���evict��*/
			page = ref->page;
			WT_ASSERT(session, page != NULL);

			/*���page�Ƿ��������������Խ���ǿ������ref��Ӧ��page*/
			if (force_attempts < 10 && __evict_force_check(session, page, flags)){
				++force_attempts;
				ret = __wt_page_release_evict(session, ref);
				/*evictʧ��,�˳�ѭ������*/
				if (ret == EBUSY) {
					ret = 0;
					wait_cnt += 1000;
					WT_STAT_FAST_CONN_INCR(session, page_forcible_evict_blocked);
					break;
				}
				else
					WT_RET(ret);

				continue;
			}

			/* Check if we need an autocommit transaction. */
			if ((ret = __wt_txn_autocommit_check(session)) != 0) {
				WT_TRET(__wt_hazard_clear(session, page)); /*���hazard pointerռ��*/
				return (ret);
			}

			if (oldgen && page->read_gen == WT_READGEN_NOTSET)
				__wt_page_evict_soon(page);
			else if (!LF_ISSET(WT_READ_NO_GEN) && page->read_gen != WT_READGEN_OLDEST && page->read_gen < __wt_cache_read_gen(session))
				page->read_gen = __wt_cache_read_gen_set(session);

			return 0;
			WT_ILLEGAL_VALUE(session);
		}

		/*spin waiting*/
		if (++wait_cnt < 1000)
			__wt_yield();
		else {
			sleep_cnt = WT_MIN(wait_cnt, 10000);
			wait_cnt *= 2;
			WT_STAT_FAST_CONN_INCRV(session, page_sleep, sleep_cnt);
			__wt_sleep(0, sleep_cnt);
		}
	}
}

/*��������ȡһ��PAGE���ݵ�cache��*/
int __wt_page_alloc(WT_SESSION_IMPL* session, uint8_t type, uint64_t recno, uint32_t alloc_entries, int alloc_refs, WT_PAGE** pagep)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	size_t size;
	uint32_t i;
	void *p;

	*pagep = NULL;

	cache = S2C(session)->cache;
	page = NULL;

	size = sizeof(WT_PAGE);
	switch (type){
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		break;

	case WT_PAGE_COL_VAR:
		/*����page���ڴ�ռ�ռ��*/
		size += alloc_entries * sizeof(WT_COL);
		break;

	case WT_PAGE_ROW_LEAF:
		size += alloc_entries * sizeof(WT_ROW);
		break;

	WT_ILLEGAL_VALUE(session);
	}

	/*����page����ռ䲢����page��������*/
	WT_RET(__wt_calloc(session, 1, size, &page));
	page->type = type;
	page->read_gen = WT_READGEN_NOTSET;

	switch (type){
	case WT_PAGE_COL_FIX:
		page->pg_fix_recno = recno;
		page->pg_fix_entries = alloc_entries;
		break;

	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		page->pg_intl_recno = recno;

		WT_ERR(__wt_calloc(session, 1, sizeof(WT_PAGE_INDEX)+alloc_entries * sizeof(WT_REF *), &p));
		size += sizeof(WT_PAGE_INDEX)+alloc_entries * sizeof(WT_REF *);
		pindex = p;
		pindex->index = (WT_REF **)((WT_PAGE_INDEX *)p + 1);
		pindex->entries = alloc_entries;
		WT_INTL_INDEX_SET(page, pindex);
		if (alloc_refs){
			for (i = 0; i < pindex->entries; ++i) {
				WT_ERR(__wt_calloc_one(session, &pindex->index[i]));
				size += sizeof(WT_REF);
			}
		}
		if (0){
err:
			if ((pindex = WT_INTL_INDEX_GET_SAFE(page)) != NULL) {
				for (i = 0; i < pindex->entries; ++i)
					__wt_free(session, pindex->index[i]);
				__wt_free(session, pindex);
			}
			__wt_free(session, page);
			return (ret);
		}

	case WT_PAGE_COL_VAR:
		page->pg_var_recno = recno;
		page->pg_var_d = (WT_COL *)((uint8_t *)page + sizeof(WT_PAGE));
		page->pg_var_entries = alloc_entries;
		break;

	case WT_PAGE_ROW_LEAF:
		page->pg_row_d = (WT_ROW *)((uint8_t *)page + sizeof(WT_PAGE));
		page->pg_row_entries = alloc_entries;
		break;

		WT_ILLEGAL_VALUE(session);
	}
	/*����cache��״̬��Ϣ*/
	__wt_cache_page_inmem_incr(session, page, size);
	WT_ATOMIC_ADD8(cache->bytes_read, size);
	WT_ATOMIC_ADD8(cache->pages_inmem, 1);

	*pagep = page;
	return 0;
}

/*��һ��page�Ӵ����϶����ڴ棬���ҹ�����page���ڴ�ṹ��Ϣ*/
int __wt_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, size_t memsize, uint32_t flags, WT_PAGE **pagep)
{
	WT_DECL_RET;
	WT_PAGE *page;
	const WT_PAGE_HEADER *dsk;
	uint32_t alloc_entries;
	size_t size;

	*pagep = NULL;

	dsk = image;
	alloc_entries = 0;

	/*ȷ����Ҫ���ڴ��з����entry����*/
	switch(dsk->type){
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		alloc_entries = dsk->u.entries;
		break;

	case WT_PAGE_ROW_INT:
		alloc_entries = dsk->u.entries / 2;
		break;

	case WT_PAGE_ROW_LEAF:
		if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL))
			alloc_entries = dsk->u.entries;
		else if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE))
			alloc_entries = dsk->u.entries / 2;
		else
			WT_RET(__inmem_row_leaf_entries(session, dsk, &alloc_entries));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	/*����page������ڴ沢��ʼ��*/
	WT_RET(__wt_page_alloc(session, dsk->type, dsk->recno, alloc_entries, 1, &page));
	page->dsk = dsk;
	F_SET_ATOMIC(page, flags);

	size = LF_ISSET(WT_PAGE_DISK_ALLOC) ? memsize : 0;

	/*����page�ڲ��ڴ����ṹ�ͼ�¼���ȵ�*/
	switch(page->type){
	case WT_PAGE_COL_FIX:
		__inmem_col_fix(session, page);
		break;

	case WT_PAGE_COL_INT:
		__inmem_col_int(session, page);
		break;

	case WT_PAGE_COL_VAR:
		WT_ERR(__inmem_col_var(session, page, &size));
		break;

	case WT_PAGE_ROW_INT:
		WT_ERR(__inmem_row_int(session, page, &size));
		break;

	case WT_PAGE_ROW_LEAF:
		WT_ERR(__inmem_row_leaf(session, page));
		break;

	WT_ILLEGAL_VALUE_ERR(session);
	}

	/*�޸�page��Ӧ���ڴ�cacheͳ��*/
	__wt_cache_page_inmem_incr(session, page, size);

	if(ref != NULL){
		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			page->pg_intl_parent_ref = ref;
			break;
		}
		ref->page = page;
	}

	*pagep = page;
	return 0;

err:
	__wt_page_out(session, &page); /*����ڴ����ʧ�ܣ����ڴ����page����*/
	return ret;
}

/*�������ʽ�̶����ȴ洢��ָ���洢���ݿ�ʼ�Ļ�����λ��*/
static void __inmem_col_fix(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	const WT_PAGE_HEADER *dsk;

	btree = S2BT(session);
	dsk = page->dsk;

	page->pg_fix_bitf = WT_PAGE_HEADER_BYTE(btree, dsk);
}

/*������ʽ�洢 �ڴ������Ľṹ*/
static void __inmem_col_int(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	WT_PAGE_INDEX *pindex;
	WT_REF **refp, *ref;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	/*���page��indexָ������*/
	pindex = WT_INTL_INDEX_GET_SAFE(page);
	refp = pindex->index;

	/*ָ�������ṹָ�����һ��pageλ��*/
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		ref = *refp ++;
		ref->home = page;
		/*��ȡcell��Ӧ��kv���ݣ�������unpack�����õ�ref��*/
		__wt_cell_unpack(cell, unpack);
		ref->addr = cell;
		ref->key.recno = unpack->v;
	}
}

/*ͳ��page���ظ�entry�ĸ���*/
static int __inmem_col_var_repeats(WT_SESSION_IMPL* session, WT_PAGE* page, uint32_t* np)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	*np = 0;

	*np = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (__wt_cell_rle(unpack) > 1) /*unpack->v <= 1��ʾentries�ظ�*/
			++*np;
	}

	return 0;
}

/*Ϊ�䳤����ʽ�洢page�����ڴ��е������ṹ����*/
static int __inmem_col_var(WT_SESSION_IMPL *session, WT_PAGE *page, size_t *sizep)
{
	WT_BTREE *btree;
	WT_COL *cip;
	WT_COL_RLE *repeats;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	uint64_t recno, rle;
	size_t bytes_allocated;
	uint32_t i, indx, n, repeat_off;

	btree = S2BT(session);
	dsk = page->dsk;
	recno = page->pg_var_recno;

	repeats = NULL;
	repeat_off = 0;
	unpack = &_unpack;
	bytes_allocated = 0;

	indx = 0;
	cip = page->pg_var_d;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);
		WT_COL_PTR_SET(cip, WT_PAGE_DISK_OFFSET(page, cell));
		cip++;

		/*Ϊ�ظ���entry�����ڴ沢����Ӧ��repeat��ʾ*/
		rle = __wt_cell_rle(unpack);
		if (rle > 1) {
			if(repeats == NULL){
				WT_RET(__inmem_col_var_repeats(session, page, &n));
				WT_RET(__wt_realloc_def(session, &bytes_allocated, n + 1, &repeats));

				page->pg_var_repeats = repeats;
				page->pg_var_nrepeats = n;
				*sizep += bytes_allocated;
			}
			repeats[repeat_off].indx = indx;
			repeats[repeat_off].recno = recno;
			repeats[repeat_off++].rle = rle;
		}

		indx++;
		recno += rle;
	}

	return 0;
}

/*Ϊ�д洢��page�����ڲ��������ڴ����,��Ҷ�ӽڵ�*/
static int __inmem_row_int(WT_SESSION_IMPL* session, WT_PAGE* page, size_t* sizep)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(current);
	WT_DECL_RET;
	const WT_PAGE_HEADER *dsk;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref, **refp;
	uint32_t i;

	btree = S2BT(session);
	unpack = &_unpack;
	dsk = page->dsk;

	WT_RET(__wt_scr_alloc(session, 0, &current));

	/*������������ָ����һ��page�Ķ���ָ��*/
	pindex = WT_INTL_INDEX_GET_SAFE(page);
	refp = pindex->index;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		ref = *refp;
		ref->home = page;

		__wt_cell_unpack(cell, unpack);
		switch(unpack->type){
		case WT_CELL_KEY:
			__wt_ref_key_onpage_set(page, ref, unpack);
			break;

		case WT_CELL_KEY_OVFL:
			/*overflow key�������Ǵ洢�ڵ����洢�ռ��ϣ���Ҫ��overflow��ʽ��ȡ���ڴ��������������row key*/
			WT_ERR(__wt_dsk_cell_data_ref(session, page->type, unpack, current));
			WT_ERR(__wt_row_ikey_incr(session, page, WT_PAGE_DISK_OFFSET(page, cell), current->data, current->size, ref));
			*sizep += sizeof(WT_IKEY) + current->size;
			break;

			/*��del mark��cell��������Ҫ�ؽ���ʾmodified��*/
		case WT_CELL_ADDR_DEL:
			ref->addr = cell;
			ref->state = WT_REF_DELETED;
			++refp;
			if(btree->modified){
				WT_ERR(__wt_page_modify_init(session, page));
				__wt_page_modify_set(session, page);
			}
			break;

		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
			ref->addr = cell;
			++refp;
			break;

		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

err:
	__wt_scr_free(session, &current);
	return ret;
}

/*�����д洢page�е�entry����*/
static int __inmem_row_leaf_entries(WT_SESSION_IMPL* session, const WT_PAGE_HEADER* dsk, uint32_t* nindxp)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i, nindx;

	btree = S2BT(session);
	unpack = &_unpack;

	nindx = 0;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			++nindx;
			break;

		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
			break;

		WT_ILLEGAL_VALUE(session);
		}
	}

	*nindxp = nindx;

	return 0;
}

/*�����д洢leaf page���ڴ���������(row array)*/
static int __inmem_row_leaf(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	WT_ROW *rip;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	rip = page->pg_row_d;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);

		switch(unpack->type){
			case WT_CELL_KEY_OVFL:
				__wt_row_leaf_key_set_cell(page, rip, cell); /*ֱ�����ó�CELL_FLAG��ʽ����*/
				++rip;
				break;

			case WT_CELL_KEY:
				if (!btree->huffman_key && unpack->prefix == 0)
					__wt_row_leaf_key_set(page, rip, unpack); /*���ó�K_FLAG��ʽ����*/
				else
					__wt_row_leaf_key_set_cell(page, rip, cell);
				++rip;
				break;

			case WT_CELL_VALUE:
				if (!btree->huffman_value)
					__wt_row_leaf_value_set(page, rip - 1, unpack); /*��K_CELL_FLAG���ó�KV_CELL_FLAG*/
				break;

			case WT_CELL_VALUE_OVFL:
				break;

			WT_ILLEGAL_VALUE(session);
		}
	}

	return 0;
}






