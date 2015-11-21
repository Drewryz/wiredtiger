/****************************************************************
* btree��״̬ͳ��
****************************************************************/
#include "wt_internal.h"

static int  __stat_page(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *cst);
static void __stat_page_col_var(WT_PAGE *page, WT_DSRC_STATS *cst);
static void __stat_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *cst);
static void __stat_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *cst);

/*��ʼ��btree��ͳ��״̬��Ϣ*/
int __wt_btree_stat_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_DSRC_STATS *stats;
	WT_REF *next_walk;

	btree = S2BT(session);
	bm = btree->bm;
	stats = &btree->dhandle->stats;

	WT_RET(bm->stat(bm, session, stats));

	WT_STAT_SET(stats, btree_fixed_len, btree->bitcnt);
	WT_STAT_SET(stats, btree_maximum_depth, btree->maximum_depth);
	WT_STAT_SET(stats, btree_maxintlpage, btree->maxintlpage);
	WT_STAT_SET(stats, btree_maxintlkey, btree->maxintlkey);
	WT_STAT_SET(stats, btree_maxleafpage, btree->maxleafpage);
	WT_STAT_SET(stats, btree_maxleafkey, btree->maxleafkey);
	WT_STAT_SET(stats, btree_maxleafvalue, btree->maxleafvalue);
	/*�������Ҫͳ��ȫ������Ϣ������Ϊֹ����*/
	if (!F_ISSET(cst, WT_CONN_STAT_ALL))
		return 0;

	/*ͳ�Ƽ���������*/
	WT_STAT_SET(stats, btree_column_deleted, 0);
	WT_STAT_SET(stats, btree_column_fix, 0);
	WT_STAT_SET(stats, btree_column_internal, 0);
	WT_STAT_SET(stats, btree_column_variable, 0);
	WT_STAT_SET(stats, btree_entries, 0);
	WT_STAT_SET(stats, btree_overflow, 0);
	WT_STAT_SET(stats, btree_row_internal, 0);
	WT_STAT_SET(stats, btree_row_leaf, 0);

	/*�������е�page������page����Ϣ��ͳ��*/
	next_walk = NULL;
	while ((ret = __wt_tree_walk(session, &next_walk, NULL, 0)) == 0 && next_walk != NULL){
		WT_WITH_PAGE_INDEX(session, ret = __stat_page(session, next_walk->page, stats));
		WT_RET(ret);
	}

	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*ͳ��page�е���Ϣ����Ҫ��������������*/
static int __stat_page(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *stats)
{
	switch (page->type){
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(stats, btree_column_fix);
		WT_STAT_INCRV(stats, btree_entries, page->pg_fix_entries);
		break;

	case WT_PAGE_COL_INT:
		WT_STAT_INCR(stats, btree_column_internal);
		break;

	case WT_PAGE_COL_VAR:
		__stat_page_col_var(page, stats);
		break;

	case WT_PAGE_ROW_INT:
		__stat_page_row_int(session, page, stats);
		break;

	case WT_PAGE_ROW_LEAF:
		__stat_page_row_leaf(session, page, stats);
		break;

		WT_ILLEGAL_VALUE(session);
	}

	return 0;
}

/*����һ��WT_PAGE_COL_VAR page��ͳ����Ϣ,��Ҫ��Ϣ�У����ɾ����k/v��,���page��k/v����k/v����*/
static void __stat_page_col_var(WT_PAGE* page, WT_DSRC_STATS* stats)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint64_t deleted_cnt, entry_cnt, ovfl_cnt;
	uint32_t i;
	int orig_deleted;

	unpack = &_unpack;
	deleted_cnt = entry_cnt = ovfl_cnt = 0;

	WT_STAT_INCR(stats, btree_column_variable);

	WT_COL_FOREACH(page, cip, i){
		if ((cell = WT_COL_PTR(page, cip)) == NULL){ /*cell��ɾ��*/
			orig_deleted = 1;
			++deleted_cnt;
		}
		else{
			orig_deleted = 0;
			
			__wt_cell_unpack(cell, unpack);
			if (unpack->type == WT_CELL_ADDR_DEL)
				orig_deleted = 1;
			else
				entry_cnt += __wt_cell_rle(unpack);

			if (unpack->ovfl)
				++ovfl_cnt;
		}

		/*ɨ��insert list*/
		WT_SKIP_FOREACH(ins, WT_COL_UPDATE(page, cip)){
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (!orig_deleted) { /*��¼���Ϊɾ����ɾ���ļ�������Ҫ+1*/
					++deleted_cnt;
					--entry_cnt;
				}
			}
			else if (orig_deleted) {
				--deleted_cnt;
				++entry_cnt;
			}
		}
	}

	/*ɨ�����һ��insert list*/
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page))
		if (WT_UPDATE_DELETED_ISSET(ins->upd))
			++deleted_cnt;
		else
			++entry_cnt;

	/*��ͳ�Ƶ���Ϣ����stats��*/
	WT_STAT_INCRV(stats, btree_column_deleted, deleted_cnt);
	WT_STAT_INCRV(stats, btree_entries, entry_cnt);
	WT_STAT_INCRV(stats, btree_overflow, ovfl_cnt);
}

/*ͳ���д洢��internal page����Ϣ,��Ҫ��ͳ���������*/
static void __stat_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *stats)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	uint32_t i, ovfl_cnt;

	btree = S2BT(session);
	ovfl_cnt = 0;

	WT_STAT_INCR(stats, btree_row_internal);

	if (page->dsk != NULL){
		WT_CELL_FOREACH(btree, page->dsk, cell, &unpack, i) {
			__wt_cell_unpack(cell, &unpack);
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL) /*keyֵ�����������*/
				++ovfl_cnt;
		}

		WT_STAT_INCRV(stats, btree_overflow, ovfl_cnt);
	}
}

/*ͳ���д洢��Ҷ��ҳ��Ϣ*/
static void __stat_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS *stats)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t entry_cnt, i, ovfl_cnt;

	btree = S2BT(session);
	entry_cnt = ovfl_cnt = 0;

	WT_STAT_INCR(stats, btree_row_leaf);

	/*ͳ��insert list�е���Ч��K/V*/
	WT_SKIP_FOREACH(ins, WT_ROW_INSERT_SMALLEST(page)){
		if (!WT_UPDATE_DELETED_ISSET(ins->upd))
			++entry_cnt;
	}

	/*����page�����е�k/v����������ͳ��*/
	WT_ROW_FOREACH(page, rip, i) {
		upd = WT_ROW_UPDATE(page, rip);
		if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
			++entry_cnt;

		if (upd == NULL && (cell = __wt_row_leaf_value_cell(page, rip, NULL)) != NULL && __wt_cell_type(cell) == WT_CELL_VALUE_OVFL)
			++ovfl_cnt;

		/* Walk K/V pairs inserted after the on-page K/V pair. */
		WT_SKIP_FOREACH(ins, WT_ROW_INSERT(page, rip))
			if (!WT_UPDATE_DELETED_ISSET(ins->upd))
				++entry_cnt;
	}

	/*ֱ��ȥ�������е�over flow���͵�key�Ǻ����ѵģ����ǿ���ɨ��page->dsk��cell��������������*/
	if (page->dsk != NULL){
		WT_CELL_FOREACH(btree, page->dsk, cell, &unpack, i) {
			__wt_cell_unpack(cell, &unpack);
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
				++ovfl_cnt;
		}
	}

	WT_STAT_INCRV(stats, btree_entries, entry_cnt);
	WT_STAT_INCRV(stats, btree_overflow, ovfl_cnt);
}



