/***************************************************************************
*LSM tree��Ϣͳ��ʵ��
***************************************************************************/
#include "wt_internal.h"

/*��ʼ��lsm tree��ͳ����Ϣ*/
static int __curstat_lsm_init(WT_SESSION_IMPL* session, const char* uri, WT_CURSOR_STAT* cst)
{
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(uribuf);
	WT_DECL_RET;
	WT_DSRC_STATS *new, *stats;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	u_int i;
	int locked;
	char config[64];

	const char *cfg[] = {
		WT_CONFIG_BASE(session, session_open_cursor), NULL, NULL };

	const char *disk_cfg[] = {
		WT_CONFIG_BASE(session, session_open_cursor), 
		"checkpoint=" WT_CHECKPOINT, NULL, NULL };

	locked = 0;

	/*���uri��Ӧ��lsm������*/
	WT_WITH_DHANDLE_LOCK(session, ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree));
	WT_RET(ret);
	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));

	if(!F_ISSET(cst, WT_CONN_STAT_NONE)){
		snprintf(config, sizeof(config), "statistics=(%s%s%s)",
			F_ISSET(cst, WT_CONN_STAT_CLEAR) ? "clear," : "",
			F_ISSET(cst, WT_CONN_STAT_ALL) ? "all," : "",
			!F_ISSET(cst, WT_CONN_STAT_ALL) && F_ISSET(cst, WT_CONN_STAT_FAST) ? "fast," : "");

		cfg[1] = disk_cfg[1] = config;
	}

	/*���ͳ����Ϣ��handler*/
	stats = &cst->u.dsrc_stats;

	/*���lsm_tree�Ķ����������������̵߳�read*/
	WT_ERR(__wt_lsm_tree_readlock(session, lsm_tree));
	locked = 1;

	__wt_stat_init_dsrc_stats(stats);

	for(i = 0; i < lsm_tree->nchunks; i++){
		chunk = lsm_tree->chunk[i];
		/*��ʽ��chunk��Ϣ*/
		WT_ERR(__wt_buf_fmt(session, uribuf, "statistics:%s", chunk->uri));

		/*����һ����ȡͳ����Ϣ��stat cursor*/
		ret = __wt_curstat_open(session, uribuf->data, F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? disk_cfg : cfg, &stat_cursor);

		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			ret = __wt_curstat_open(session, uribuf->data, cfg, &stat_cursor);

		WT_ERR(ret);

		/*���stat cursor��ͷ*/
		new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
		/*����lsm_generation_max������ֵ*/
		WT_STAT_SET(new, lsm_generation_max, chunk->generation);

		/* ������ϲ���stat�� */
		__wt_stat_aggregate_dsrc_stats(new, stats);

		WT_ERR(stat_cursor->close(stat_cursor));

		if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			continue;

		/* Maintain a count of bloom filters. */
		WT_STAT_INCR(&lsm_tree->stats, bloom_count);
		/*��ȡbloom filter��ͳ����Ϣ*/
		WT_ERR(__wt_buf_fmt(session, uribuf, "statistics:%s", chunk->bloom_uri));
		WT_ERR(__wt_curstat_open(session, uribuf->data, cfg, &stat_cursor));

		new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
		/*����bloom filterͳ����Ϣ*/
		WT_STAT_SET(new,bloom_size, (chunk->count * lsm_tree->bloom_bit_count) / 8);
		WT_STAT_SET(new, bloom_page_evict, WT_STAT(new, cache_eviction_clean) + WT_STAT(new, cache_eviction_dirty));
		WT_STAT_SET(new, bloom_page_read, WT_STAT(new, cache_read));
		/*������ϲ���stat��*/
		__wt_stat_aggregate_dsrc_stats(new, stats);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

	WT_STAT_SET(stats, lsm_chunk_count, lsm_tree->nchunks);
	/*��lsm_stats�е�ͳ����Ϣ���ܵ�stat��*/
	__wt_stat_aggregate_dsrc_stats(&lsm_tree->stats, stats);

	/*����������ѡ����ն�Ӧ��lsm_tree*/
	if (F_ISSET(cst, WT_CONN_STAT_CLEAR))
		__wt_stat_refresh_dsrc_stats(&lsm_tree->stats);

	__wt_curstat_dsrc_final(cst);

err:	
	if (locked)
		WT_TRET(__wt_lsm_tree_readunlock(session, lsm_tree));

	__wt_lsm_tree_release(session, lsm_tree);
	__wt_scr_free(session, &uribuf);

	return ret;
}

/*��session��Ӧuri��LSM TREE����ͳ����Ϣ��ʼ��*/
int __wt_curstat_lsm_init(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR_STAT *cst)
{
	WT_DECL_RET;

	WT_WITH_SCHEMA_LOCK(session, ret = __curstat_lsm_init(session, uri, cst));
}




