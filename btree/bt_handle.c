/*******************************************************************
*btree�Ļ�����open close�Ȳ���ʵ�֣���Ҫ�ǹ����ڴ��е�btree�Ķ���ṹ
*���漰��BTREE��������Ϣ��ȡ��meta��Ϣ��ȡ
*******************************************************************/

#include "wt_internal.h"

static int __btree_conf(WT_SESSION_IMPL* sssion, WT_CKPT* ckpt);
static int __btree_get_last_recno(WT_SESSION_IMPL* session);
static int __btree_page_sizes(WT_SESSION_IMPL* session);
static int __btree_preload(WT_SESSION_IMPL* session);
static int __btree_tree_open_empty(WT_SESSION_IMPL* session, int creation);

/*�������ߴ�һ��btree*/
int __wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CKPT ckpt;
	WT_CONFIG_ITEM cval;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	size_t root_addr_size;
	uint8_t root_addr[WT_BTREE_MAX_ADDR_COOKIE];
	int creation, forced_salvage, readonly;
	const char *filename;

	dhandle = session->dhandle;
	btree = S2BT(session);

	/*�ж��Ƿ���ֻ�����ԣ����SESSION�Ѿ�������CHECKPOINT����ô���ܶ����BTREE�ļ������޸�*/
	readonly = (dhandle->checkpoint == NULL ? 0 : 1);
	/*���checkpoint��Ϣ*/
	WT_CLEAR(ckpt);
	WT_RET(__wt_meta_checkpoint(session, dhandle->name, dhandle->checkpoint, &ckpt));

	/*�ж��Ƿ����½�һ��btree*/
	creation = (ckpt.raw.size == 0);
	if (!creation && F_ISSET(btree, WT_BTREE_BULK))
		WT_ERR_MSG(session, EINVAL, "bulk-load is only supported on newly created objects");

	/*�ж��Ƿ���Ҫ��btree����salvage�޸�*/
	forced_salvage = 0;
	if(F_ISSET(btree, WT_BTREE_SALVAGE)){
		WT_ERR(__wt_config_gets(session, op_cfg, "force", &cval));
		forced_salvage = (cval.val != 0);
	}
	/*��ʼ��btree����ṹ*/
	WT_ERR(__btree_conf(session, &ckpt));

	/*����btree�Ͷ�Ӧ��file block manager����*/
	filename = dhandle->name;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_ERR_MSG(session, EINVAL, "expected a 'file:' URI");
	/*Ϊbtree��һ��block manager*/
	WT_ERR(__wt_block_manager_open(session, filename, dhandle->cfg, forced_salvage, readonly, btree->allocsize, &btree->bm));
	bm = btree->bm;

	btree->block_header = bm->block_header(bm);

	/*��һ��ָ����checkpointλ�õ����ݣ��ǲ���Ҫ����У���޸��Ȳ�������Ϊ�Լ�������checkpoint�ǿɿ���*/
	if(!F_ISSET(btree, WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)){
		WT_ERR(bm->checkpoint_load(bm, session, ckpt.raw.data, ckpt.raw.size,
			root_addr, &root_addr_size, readonly));
		/*����ǽ���һ���յ�btreeֻ��Ҫֱ�Ӵ򿪾����ˣ�����������ݵ�btree��Ҫ����������֯�������ڴ���Ϣ����*/
		if(creation || root_addr_size == 0)
			WT_ERR(__btree_tree_open_empty(session, creation));
		else{
			WT_ERR(__wt_btree_tree_open(session, root_addr, root_addr_size));

			/*Ԥ�ȼ�������*/
			WT_WITH_PAGE_INDEX(session, ret = __btree_preload(session));
			WT_ERR(ret);

			/*��ȡ��ʽ�洢���ļ�¼���*/
			if(btree->type != BTREE_ROW)
				WT_ERR(__btree_get_last_recno(session));
		}
	}
	if(0){
err:	WT_TRET(__wt_btree_close(session));
	}

	__wt_meta_checkpoint_free(session, &ckpt);
	return ret;
}

/*�ر�һ��btree����*/
int __wt_btree_close(WT_SESSION_IMPL* session)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = session->dhandle;
	btree = S2BT(session);

	if((bm = btree->bm) != NULL){
		/*ֻ�в��Ƕ�btree����У���޸���session��unload checkpoint����*/
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) && !F_ISSET(btree, WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
			WT_TRET(bm->checkpoint_unload(bm, session));

		WT_TRET(bm->close(bm, session));
		btree->bm = NULL;
	}

	/*�ر�huffman tree*/
	__wt_btree_huffman_close(session);

	/*�ͷŵ�latch��btree��kv�ڴ����collator�����*/
	WT_TRET(__wt_rwlock_destroy(session, &btree->ovfl_lock));
	__wt_spin_destroy(session, &btree->flush_lock);

	__wt_free(session, btree->key_format);
	__wt_free(session, btree->value_format);

	if (btree->collator_owned) {
		if (btree->collator->terminate != NULL)
			WT_TRET(btree->collator->terminate(btree->collator, &session->iface));
		btree->collator_owned = 0;
	}
	btree->collator = NULL;

	btree->bulk_load_ok = 0;

	return ret;
}

/*����ckpt��Ϣ����btree��Ӧ�Ľṹ���󲢳�ʼ��*/
static int __btree_conf(WT_SESSION_IMPL* sssion, WT_CKPT* ckpt)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval, metadata;
	int64_t maj_version, min_version;
	uint32_t bitcnt;
	int fixed;
	const char **cfg;

	btree = S2BT(session);
	cfg = btree->dhandle->cfg;

	/*��ȡcfg������Ϣ�еİ汾��Ϣ*/
	if (WT_VERBOSE_ISSET(session, WT_VERB_VERSION)) {
		WT_RET(__wt_config_gets(session, cfg, "version.major", &cval));
		maj_version = cval.val;
		WT_RET(__wt_config_gets(session, cfg, "version.minor", &cval));
		min_version = cval.val;
		WT_RET(__wt_verbose(session, WT_VERB_VERSION, "%" PRIu64 ".%" PRIu64, maj_version, min_version));
	}

	/*����ļ�ID*/
	WT_RET(__wt_config_gets(session, cfg, "id", &cval));
	btree->id = (uint32_t)cval.val;

	/*��ȡcfg�е�key format��ʽ*/
	WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_RET(__wt_struct_confchk(session, &cval));
	if (WT_STRING_MATCH("r", cval.str, cval.len))
		btree->type = BTREE_COL_VAR;
	else
		btree->type = BTREE_ROW;

	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->key_format));

	WT_RET(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_RET(__wt_struct_confchk(session, &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->value_format));

	/*����btree��У����collator*/
	if (btree->type == BTREE_ROW) {
		WT_RET(__wt_config_gets_none(session, cfg, "collator", &cval));
		if (cval.len != 0) {
			WT_RET(__wt_config_gets(session, cfg, "app_metadata", &metadata));
			WT_RET(__wt_collator_config(session, btree->dhandle->name, &cval, &metadata,
				&btree->collator, &btree->collator_owned));
		}

		WT_RET(__wt_config_gets(session, cfg, "key_gap", &cval));
		btree->key_gap = (uint32_t)cval.val;
	}

	/*��ʽ�洢��Ҫ���fixed-size data��ָ��btree-type = FIX*/
	if (btree->type == BTREE_COL_VAR) {
		WT_RET(__wt_struct_check(session, cval.str, cval.len, &fixed, &bitcnt));
		if (fixed) {
			if (bitcnt == 0 || bitcnt > 8)
				WT_RET_MSG(session, EINVAL, "fixed-width field sizes must be greater than 0 and less than or equal to 8");
			btree->bitcnt = (uint8_t)bitcnt;
			btree->type = BTREE_COL_FIX;
		}
	}

	WT_RET(__btree_page_sizes(session));

	/*����page����ѡ����Ϣ��metadata�ǲ��ᱻ�����*/
	if(WT_IS_METADATA(btree->dhandle))
		F_SET(btree, WT_BTREE_NO_EVICTION | WT_BTREE_NO_HAZARD);
	else{
		WT_RET(__wt_config_gets(session, cfg, "cache_resident", &cval));
		if (cval.val)
			F_SET(btree, WT_BTREE_NO_EVICTION | WT_BTREE_NO_HAZARD);
		else
			F_CLR(btree, WT_BTREE_NO_EVICTION);
	}

	/*checksumsУ����Ϣ����*/
	WT_RET(__wt_config_gets(session, cfg, "checksum", &cval));
	if (WT_STRING_MATCH("on", cval.str, cval.len))
		btree->checksum = CKSUM_ON;
	else if (WT_STRING_MATCH("off", cval.str, cval.len))
		btree->checksum = CKSUM_OFF;
	else
		btree->checksum = CKSUM_UNCOMPRESSED;

	/*��huffman�������*/
	WT_RET(__wt_btree_huffman_open(session));

	switch(btree->type){
	case BTREE_COL_FIX:
		break;

	case BTREE_ROW: /*�д洢ʱָ������ѹ���ķ�ʽ*/
		WT_RET(__wt_config_gets(session, cfg, "internal_key_truncate", &cval));
		btree->internal_key_truncate = cval.val == 0 ? 0 : 1;

		WT_RET(__wt_config_gets(session, cfg, "prefix_compression", &cval));
		btree->prefix_compression = cval.val == 0 ? 0 : 1;

		WT_RET(__wt_config_gets(session, cfg, "prefix_compression_min", &cval));
		btree->prefix_compression_min = (u_int)cval.val;

	case BTREE_COL_VAR:
		WT_RET(__wt_config_gets(session, cfg, "dictionary", &cval));
		btree->dictionary = (u_int)cval.val;
		break;
	}
	/*��������ʼ��ѹ������*/
	WT_RET(__wt_config_gets_none(session, cfg, "block_compressor", &cval));
	WT_RET(__wt_compressor_config(session, &cval, &btree->compressor));

	/*��ʼ��latch*/
	WT_RET(__wt_rwlock_alloc(session, &btree->ovfl_lock, "btree overflow lock"));
	WT_RET(__wt_spin_init(session, &btree->flush_lock, "btree flush lock"));
	/*��ʼ��ͳ����Ϣ*/
	__wt_stat_init_dsrc_stats(&btree->dhandle->stats);

	btree->write_gen = ckpt->write_gen;		/* Write generation */
	btree->modified = 0;					/* Clean */

	return 0;
}

/*��ʼ��btree root��ref����*/
void __wt_root_ref_init(WT_REF* root_ref, WT_PAGE* root, int is_recno)
{
	memset(root_ref, 0, sizeof(*root_ref));

	root_ref->page = root;
	root_ref->state = WT_REF_MEM;

	root_ref->key.recno = is_recno ? 1 : 0;

	root->pg_intl_parent_ref = root_ref;
}

/*�Ӵ����ж�ȡbtree�����ݲ���ʼ������page*/
int __wt_btree_tree_open(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM dsk;
	WT_PAGE *page;

	btree = S2BT(session);

	WT_CLEAR(dsk);
	/*����block addr��ȡ��Ӧ���ļ�����*/
	WT_ERR(__wt_bt_read(session, &dsk, addr, addr_size));
	/**/
	WT_ERR(__wt_page_inmem(session, NULL, dsk.data, dsk.memsize, WT_DATA_IN_ITEM(&dsk) ? WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page));
	dsk.mem = NULL;
	
	__wt_root_ref_init(&btree->root, page, btree->type != BTREE_ROW);
err:
	__wt_buf_free(session, &dsk);
	return ret;
}

/*���ڴ��д���һ���յ�btree����*/
static int __btree_tree_open_empty(WT_SESSION_IMPL* session, int creation)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *leaf, *root;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref;

	btree = S2BT(session);
	root = leaf = NULL;
	ref = NULL;

	/*
	 * Newly created objects can be used for cursor inserts or for bulk
	 * loads; set a flag that's cleared when a row is inserted into the
	 * tree.   Objects being bulk-loaded cannot be evicted, we set it
	 * globally, there's no point in searching empty trees for eviction.
	 */
	if(creation){
		btree->bulk_load_ok = 1;
		__wt_btree_evictable(session, 0);
	}

	switch(btree->type){
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__wt_page_alloc(session, WT_PAGE_COL_INT, 1, 1, 1, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_GET_SAFE(root);
		ref = pindex->index[0];
		ref->home = root;
		ref->page = NULL;
		ref->addr = NULL;
		ref->state = WT_REF_DELETED;
		ref->key.recno = 1;
		break;

	case BTREE_ROW:
		WT_ERR(__wt_page_alloc(session, WT_PAGE_ROW_INT, 0, 1, 1, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_GET_SAFE(root);
		ref = pindex->index[0];
		ref->home = root;
		ref->page = NULL;
		ref->addr = NULL;
		ref->state = WT_REF_DELETED;
		WT_ERR(__wt_row_ikey_incr(session, root, 0, "", 1, ref));
		break;

	WT_ILLEGAL_VALUE_ERR(session);
	}

	/*���bulk load��������Ҫ��ǰ�½�һ��Ҷ��ҳ����Э���洢*/
	if(F_ISSET(btree, WT_BTREE_BULK)){
		WT_ERR(__wt_btree_new_leaf_page(session, &leaf));
		/*����modifyѡ��*/
		ref->page = leaf;
		ref->state = WT_REF_MEM;
		WT_ERR(__wt_page_modify_init(session, leaf));
		__wt_page_only_modify_set(session, leaf);
	}

	__wt_root_ref_init(&btree->root, root, btree->type != BTREE_ROW);
	return 0;

	
err:/*���������Ҫ������root�ͷ����leaf page*/
	if (leaf != NULL)
		__wt_page_out(session, &leaf);
	if (root != NULL)
		__wt_page_out(session, &root);

	return ret;
}

/*����һ���յ�leaf page*/
int __wt_btree_new_leaf_page(WT_SESSION_IMPL* session, WT_PAGE** pagep)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	switch(btree->type){
	case BTREE_COL_FIX:
		WT_RET(__wt_page_alloc(session, WT_PAGE_COL_FIX, 1, 0, 0, pagep));
		break;

	case BTREE_COL_VAR:
		WT_RET(__wt_page_alloc(session, WT_PAGE_COL_VAR, 1, 0, 0, pagep));
		break;

	case BTREE_ROW:
		WT_RET(__wt_page_alloc(session, WT_PAGE_ROW_LEAF, 0, 0, 0, pagep));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	return 0;
}

/*��ʾbtree�е�page�ǲ��ܱ�cahce �����*/
void __wt_btree_evictable(WT_SESSION_IMPL* session, int on)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/* The metadata file is never evicted. */
	if (on && !WT_IS_METADATA(btree->dhandle))
		F_CLR(btree, WT_BTREE_NO_EVICTION);
	else
		F_SET(btree, WT_BTREE_NO_EVICTION);
}

/*Ԥ��internal page����*/
static int __btree_preload(WT_SESSION_IMPL* session)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_REF *ref;
	size_t addr_size;
	const uint8_t *addr;

	btree = S2BT(session);
	bm = btree->bm;

	/* Pre-load the second-level internal pages. */
	WT_INTL_FOREACH_BEGIN(session, btree->root.page, ref) {
		WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
		if (addr != NULL)
			WT_RET(bm->preload(bm, session, addr, addr_size));
	} WT_INTL_FOREACH_END;

	return 0;
}

/*Ϊ��ʽ�洢�������ļ�¼��ţ���������������page??*/
static int __btree_get_last_recno(WT_SESSION_IMPL* session)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_REF *next_walk;

	btree = S2BT(session);

	next_walk = NULL;
	WT_RET(__wt_tree_walk(session, &next_walk, NULL, WT_READ_PREV));
	if (next_walk == NULL)
		return (WT_NOTFOUND);

	page = next_walk->page;
	btree->last_recno = page->type == WT_PAGE_COL_VAR ? __col_var_last_recno(page) : __col_fix_last_recno(page);

	return (__wt_page_release(session, next_walk, 0));
}

/*
 * __btree_page_sizes --
 *	Verify the page sizes. Some of these sizes are automatically checked
 *	using limits defined in the API, don't duplicate the logic here.
 * ȷ��btree����page size�Ĵ�С���
 */
static int __btree_page_sizes(WT_SESSION_IMPL* session)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	uint64_t cache_size;
	uint32_t intl_split_size, leaf_split_size;
	const char **cfg;

	btree = S2BT(session);
	cfg = btree->dhandle->cfg;

	/*���cfg������Ϣ�е�allocation size, ������size������2��N�η�*/
	WT_RET(__wt_direct_io_size_check(session, cfg, "allocation_size", &btree->allocsize));
	if (!__wt_ispo2(btree->allocsize))
		WT_RET_MSG(session, EINVAL, "the allocation size must be a power of two");

	/*���internal page�����ռ��С��leaf page������page size*/
	WT_RET(__wt_direct_io_size_check(session, cfg, "internal_page_max", &btree->maxintlpage));
	WT_RET(__wt_direct_io_size_check(session, cfg, "leaf_page_max", &btree->maxleafpage));
	if (btree->maxintlpage < btree->allocsize || btree->maxintlpage % btree->allocsize != 0 ||
		btree->maxleafpage < btree->allocsize || btree->maxleafpage % btree->allocsize != 0)
		WT_RET_MSG(session, EINVAL, "page sizes must be a multiple of the page allocation size (%" PRIu32 "B)", btree->allocsize);

	/*ȷ�������ڴ�page�Ĵ�С*/
	WT_RET(__wt_config_gets(session, cfg, "memory_page_max", &cval));
	btree->maxmempage = WT_MAX((uint64_t)cval.val, 50 * (uint64_t)btree->maxleafpage);
	cache_size = S2C(session)->cache_size;
	if (cache_size > 0)
		btree->maxmempage = WT_MIN(btree->maxmempage, cache_size / 4);

	/*ȷ��split internal page��split leaf page�Ŀռ��С*/
	WT_RET(__wt_config_gets(session, cfg, "split_pct", &cval));
	btree->split_pct = (int)cval.val;
	intl_split_size = __wt_split_page_size(btree, btree->maxintlpage);
	leaf_split_size = __wt_split_page_size(btree, btree->maxleafpage);

	/*ȷ��deep min/max child������С*/
	if (__wt_config_gets(session, cfg, "split_deepen_min_child", &cval) == WT_NOTFOUND || cval.val == 0)
		btree->split_deepen_min_child = WT_SPLIT_DEEPEN_MIN_CHILD_DEF;
	else
		btree->split_deepen_min_child = (u_int)cval.val;
	if (__wt_config_gets(session, cfg, "split_deepen_per_child", &cval) == WT_NOTFOUND || cval.val == 0)
		btree->split_deepen_per_child = WT_SPLIT_DEEPEN_PER_CHILD_DEF;
	else
		btree->split_deepen_per_child = (u_int)cval.val;

	/*ȷ��internal page key/value max*/
	WT_RET(__wt_config_gets(session, cfg, "internal_key_max", &cval));
	btree->maxintlkey = (uint32_t)cval.val;
	if (btree->maxintlkey == 0) {
		WT_RET(__wt_config_gets(session, cfg, "internal_item_max", &cval));
		btree->maxintlkey = (uint32_t)cval.val;
	}

	/*ȷ��leaf page key/value max*/
	WT_RET(__wt_config_gets(session, cfg, "leaf_key_max", &cval));
	btree->maxleafkey = (uint32_t)cval.val;
	WT_RET(__wt_config_gets(session, cfg, "leaf_value_max", &cval));
	btree->maxleafvalue = (uint32_t)cval.val;

	if (btree->maxleafkey == 0 && btree->maxleafvalue == 0) {
		WT_RET(__wt_config_gets(session, cfg, "leaf_item_max", &cval));
		btree->maxleafkey = (uint32_t)cval.val;
		btree->maxleafvalue = (uint32_t)cval.val;
	}

	/*����max internal/leaf key�Ĵ�С*/
	if (btree->maxintlkey == 0 || btree->maxintlkey > intl_split_size / 10)
		btree->maxintlkey = intl_split_size / 10;
	if (btree->maxleafkey == 0)
		btree->maxleafkey = leaf_split_size / 10;
	if (btree->maxleafvalue == 0)
		btree->maxleafvalue = leaf_split_size / 2;

	return 0;
}


