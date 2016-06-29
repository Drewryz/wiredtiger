/************************************************************************
* LSM TREE�Ĵ������򿪡��رա�compact�Ȳ����Ķ���
************************************************************************/
#include "wt_internal.h"

static int __lsm_tree_cleanup_old(WT_SESSION_IMPL *, const char*);
static int __lsm_tree_open_check(WT_SESSION_IMPL *, WT_LSM_TREE *);
static int __lsm_tree_open(WT_SESSION_IMPL *, const char *, WT_LSM_TREE **);
static int __lsm_tree_set_name(WT_SESSION_IMPL *, WT_LSM_TREE *, const char *);

/*�ͷ��ڴ��е�lsm tree����*/
static int __lsm_tree_discard(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree, int final)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	u_int i;

	WT_UNUSED(final);

	/*��connection�е�lsm queue���Ƴ����󣬷�ֹ������������*/
	if(F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)){
		/*�������session handler list lock*/
		WT_ASSERT(session, final || F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));
		TAILQ_REMOVE(&S2C(session)->lsmqh, lsm_tree, q);
	}

	/*��ֹ�Ƚ�������*/
	if (lsm_tree->collator_owned && lsm_tree->collator->terminate != NULL)
		WT_TRET(lsm_tree->collator->terminate(lsm_tree->collator, &session->iface));

	__wt_free(session, lsm_tree->name);
	__wt_free(session, lsm_tree->config);
	__wt_free(session, lsm_tree->key_format);
	__wt_free(session, lsm_tree->value_format);
	__wt_free(session, lsm_tree->collator_name);
	__wt_free(session, lsm_tree->bloom_config);
	__wt_free(session, lsm_tree->file_config);

	/*�ͷŵ�lsm tree�Ķ�д������*/
	WT_TRET(__wt_rwlock_destroy(session, &lsm_tree->rwlock));

	/*�ͷ�chunks����ռ�õ��ڴ�*/
	for(i = 0; i < lsm_tree->nchunks; i++){
		if ((chunk = lsm_tree->chunk[i]) == NULL)
			continue;

		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);
	}
	__wt_free(session, lsm_tree->chunk);

	/*�ͷ�old chunks*/
	for(i = 0; i < lsm_tree->nold_chunks; i ++){
		chunk = lsm_tree->old_chunks[i];
		WT_ASSERT(session, chunk != NULL);

		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);
	}
	__wt_free(session, lsm_tree->old_chunks);
	/*�ͷ�lsm tree����*/
	__wt_free(session, lsm_tree);
	
	return ret;
}

/*�ر�lsm tree*/
static int __lsm_tree_close(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree)
{
	WT_DECL_RET;
	int i;

	/*��lsm tree active״̬���������ֹmerger����,ֻ����active״̬�²Ż����merge*/
	F_CLR(lsm_tree, WT_LSM_TREE_ACTIVE);

	/*�������г��ԣ�ֱ��lsm treeû������ģ������Ϊֹ*/
	for(i = 0; lsm_tree->refcnt > 1 || lsm_tree->queue_ref > 0; ++i){
		/*�൱��spin wait*/
		if (i % 1000 == 0) {
			/*�������������������ڵȴ��������lsm tree������,����flush/drop file�� */
			WT_WITHOUT_LOCKS(session, ret = __wt_lsm_manager_clear_tree(session, lsm_tree));
			WT_RET(ret);
		}

		__wt_yield();
	}

	return 0;
}

/*�������ȹرգ����ͷţ����е�lsm tree ����, һ������ͣ��ʱ����*/
int __wt_lsm_tree_close_all(WT_SESSION_IMPL* session)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	/*�𲽴�lsmqh������ȡ��lsm tree���ͷ�,һ���������ݿ�shtting down���̵��ã����ý���������*/
	while((lsm_tree = TAILQ_FIRST(&S2C(session)->lsmqh)) != NULL){
		/*���ü������ӣ���ʾ���ͷ����lsm_tree*/
		WT_ATOMIC_ADD4(lsm_tree->refcnt, 1);
		WT_TRET(__lsm_tree_close(session, lsm_tree));
		WT_TRET(__lsm_tree_discard(session, lsm_tree, 1));
	}

	return ret;
}

/*�޸�lsm tree name*/
static int __lsm_tree_set_name(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, const char *uri)
{
	if (lsm_tree->name != NULL)
		__wt_free(session, lsm_tree->name);

	WT_RET(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = lsm_tree->name + strlen("lsm:");

	return 0;
}

/*��lsm tree��filename������һ��bloom uri,��ͨ��retp����*/
int __wt_lsm_tree_bloom_name(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id, const char **retp)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_buf_fmt(session, tmp, "file:%s-%06" PRIu32 ".bf", lsm_tree->filename, id));

	WT_ERR(__wt_strndup(session, tmp->data, tmp->size, retp));

err:	
	__wt_scr_free(session, &tmp);
	return ret;
}

/*����chunk name*/
int __wt_lsm_tree_chunk_name(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id, const char **retp)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_buf_fmt(session, tmp, "file:%s-%06" PRIu32 ".lsm", lsm_tree->filename, id));
	WT_ERR(__wt_strndup(session, tmp->data, tmp->size, retp));

err:	
	__wt_scr_free(session, &tmp);
	return (ret);
}

/*����chunk��chunk size*/
int __wt_lsm_tree_set_chunk_size(WT_SESSION_IMPL *session, WT_LSM_CHUNK *chunk)
{
	wt_off_t size;
	const char *filename;

	filename = chunk->uri;

	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(session, EINVAL, "Expected a 'file:' URI: %s", chunk->uri);

	WT_RET(__wt_filesize_name(session, filename, &size));

	chunk->size = (uint64_t)size;

	return 0;
}

/*�����һ��lsm chunk�ļ���*/
static int __lsm_tree_cleanup_old(WT_SESSION_IMPL* session, const char* uri)
{
	WT_DECL_RET;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_drop), "force", NULL };
	int exists;

	WT_RET(__wt_exist(session, uri + strlen("file:"), &exists));
	if (exists)
		WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_drop(session, uri, cfg));

	return ret;
}

/*��ʼ��һ��lsm tree��chunk��Ϣ, ��Ҫ������chunk name,����ʱ���������schema��*/
int __wt_lsm_tree_setup_chunk(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));
	/*���chunk����ʱ���*/
	WT_RET(__wt_epoch(session, &chunk->create_ts));

	WT_RET(__wt_lsm_tree_chunk_name(session, lsm_tree, chunk->id, &chunk->uri));

	/*���chunkǰ���Ѿ�ʹ�ù�����ǰ���ȥ�Ĳ����ļ�ɾ��*/
	if (chunk->id > 1)
		WT_RET(__lsm_tree_cleanup_old(session, chunk->uri));

	/*��schema�㴴��chunk*/
	return __wt_schema_create(session, chunk->uri, lsm_tree->file_config);
}

/*��ʼ��lsm tree��bloom filter */
int __wt_lsm_tree_setup_bloom(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_DECL_RET;

	if(chunk->bloom_uri == NULL)
		WT_RET(__wt_lsm_tree_bloom_name(session, lsm_tree, chunk->id, &chunk->bloom_uri));
	/*�����chunkԭ����bloom filter�ļ�*/
	WT_RET(__lsm_tree_cleanup_old(session, chunk->bloom_uri));

	return ret;
}

/*����һ��LSM TREE��������meta��Ϣд�뵽meta��������*/
int __wt_lsm_tree_create(WT_SESSION_IMPL *session, const char *uri, int exclusive, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_create), config, NULL };
	char *tmpconfig;
	
	/*�ж�uri��Ӧ��LSM TREE�����Ƿ���meta��������,����Ѿ��ڹ������У�˵��lsm tree��Ӧ�Ѿ�����*/
	WT_WITH_DHANDLE_LOCK(session, uri, 0, &lsm_tree);
	ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree);
	if(ret == 0){
		__wt_lsm_tree_release(session, lsm_tree);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	/*����URI��Ӧ��meta��Ϣ�Ƿ���ڣ�������ڣ�˵��lsm treeҲ�Ѿ�����*/
	if (__wt_metadata_search(session, uri, &tmpconfig) == 0) {
		__wt_free(session, tmpconfig);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	/*����ǿ����½�һ��lsm tree����ô�����ȴ������ļ���ȡ��meta��Ϣ������metaд��meta������*/
	WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
	if (WT_STRING_MATCH("r", cval.str, cval.len))
		WT_RET_MSG(session, EINVAL,
		"LSM trees cannot be configured as column stores");

	WT_RET(__wt_calloc_one(session, &lsm_tree));

	WT_ERR(__lsm_tree_set_name(session, lsm_tree, uri));

	WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_ERR(__wt_strndup(
		session, cval.str, cval.len, &lsm_tree->key_format));
	WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_ERR(__wt_strndup(
		session, cval.str, cval.len, &lsm_tree->value_format));

	WT_ERR(__wt_config_gets_none(session, cfg, "collator", &cval));
	WT_ERR(__wt_strndup(
		session, cval.str, cval.len, &lsm_tree->collator_name));

	WT_ERR(__wt_config_gets(session, cfg, "lsm.auto_throttle", &cval));
	if (cval.val)
		F_SET(lsm_tree, WT_LSM_TREE_THROTTLE);
	else
		F_CLR(lsm_tree, WT_LSM_TREE_THROTTLE);
	WT_ERR(__wt_config_gets(session, cfg, "lsm.bloom", &cval));
	FLD_SET(lsm_tree->bloom, (cval.val == 0 ? WT_LSM_BLOOM_OFF : WT_LSM_BLOOM_MERGED));
	WT_ERR(__wt_config_gets(session, cfg, "lsm.bloom_oldest", &cval));
	if (cval.val != 0)
		FLD_SET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST);

	if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF) &&
		FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST))
		WT_ERR_MSG(session, EINVAL,
		"Bloom filters can only be created on newest and oldest "
		"chunks if bloom filters are enabled");

	WT_ERR(__wt_config_gets(session, cfg, "lsm.bloom_config", &cval));
	if (cval.type == WT_CONFIG_ITEM_STRUCT) {
		cval.str++;
		cval.len -= 2;
	}
	WT_ERR(__wt_config_check(session,
		WT_CONFIG_REF(session, session_create), cval.str, cval.len));
	WT_ERR(__wt_strndup(
		session, cval.str, cval.len, &lsm_tree->bloom_config));

	WT_ERR(__wt_config_gets(session, cfg, "lsm.bloom_bit_count", &cval));
	lsm_tree->bloom_bit_count = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm.bloom_hash_count", &cval));
	lsm_tree->bloom_hash_count = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm.chunk_count_limit", &cval));
	lsm_tree->chunk_count_limit = (uint32_t)cval.val;
	if (cval.val == 0)
		F_SET(lsm_tree, WT_LSM_TREE_MERGES);
	else
		F_CLR(lsm_tree, WT_LSM_TREE_MERGES);
	WT_ERR(__wt_config_gets(session, cfg, "lsm.chunk_max", &cval));
	lsm_tree->chunk_max = (uint64_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm.chunk_size", &cval));
	lsm_tree->chunk_size = (uint64_t)cval.val;
	if (lsm_tree->chunk_size > lsm_tree->chunk_max)
		WT_ERR_MSG(session, EINVAL, "Chunk size (chunk_size) must be smaller than or equal to the maximum chunk size (chunk_max)");

	WT_ERR(__wt_config_gets(session, cfg, "lsm.merge_max", &cval));
	lsm_tree->merge_max = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm.merge_min", &cval));
	lsm_tree->merge_min = (uint32_t)cval.val;
	if (lsm_tree->merge_min > lsm_tree->merge_max)
		WT_ERR_MSG(session, EINVAL, "LSM merge_min must be less than or equal to merge_max");

	/*�Ƚ�lsm tree��meta��Ϣд�뵽meta�������У����رյ���ʱ������lsm tree����*/
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,"%s,key_format=u,value_format=u,memory_page_max=%" PRIu64,
		config, 2 * lsm_tree->chunk_max));
	WT_ERR(__wt_strndup(session, buf->data, buf->size, &lsm_tree->file_config));
	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	/*��������ʱ�Ķ���*/
	ret = __lsm_tree_discard(session, lsm_tree, 0);
	lsm_tree = NULL;

	/*���´�uri��Ӧ��lsm tree���󣬲�����Ϊ��״̬*/
	if (ret == 0)
		WT_WITH_DHANDLE_LOCK(session, ret = __lsm_tree_open(session, uri, &lsm_tree));
	if (ret == 0)
		__wt_lsm_tree_release(session, lsm_tree);

	if(0){
err:
		WT_TRET(__lsm_tree_discard(session, lsm_tree, 0));
	}
	__wt_scr_free(session, &buf);

	return ret;
}

/*ͨ��lsm tree��uri name�ҵ���Ӧ��lsm���ڴ����*/
static int __lsm_tree_find(WT_SESSION_IMPL *session, const char *uri, int exclusive, WT_LSM_TREE **treep)
{
	WT_LSM_TREE *lsm_tree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));

	/*�ж�uri��Ӧ��lsm tree�����Ƿ��Ѿ���*/
	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if(strcmp(uri, lsm_tree->name) == 0){
			/*�Ѿ���������������ʹ��lsm tree����,��������exclusive����ռ������*/
			if ((exclusive && lsm_tree->refcnt > 0) || lsm_tree->exclusive)
				return (EBUSY);

			if(exclusive){
				/*������������*/
				if (!WT_ATOMIC_CAS1(lsm_tree->exclusive, 0, 1))
					return (EBUSY);

				if (!WT_ATOMIC_CAS4(lsm_tree->refcnt, 0, 1)) {
					lsm_tree->exclusive = 0;
					return (EBUSY);
				}
			}
			else{
				/*�������ü���*/
				(void)WT_ATOMIC_ADD4(lsm_tree->refcnt, 1);

				/*����ڴ˿��������������ԣ�������ѯ������æ*/
				if (lsm_tree->exclusive) {
					WT_ASSERT(session, lsm_tree->refcnt > 0);
					(void)WT_ATOMIC_SUB4(lsm_tree->refcnt, 1);

					return (EBUSY);
				}
			}
			*treep = lsm_tree;
		}

		return WT_NOTFOUND;
}

/*���lsm tree��������Ϣ�Ƿ����*/
static int __lsm_tree_open_check(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_CONFIG_ITEM cval;
	uint64_t maxleafpage, required;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_create), lsm_tree->file_config, NULL };

	WT_RET(__wt_config_gets(session, cfg, "leaf_page_max", &cval));
	maxleafpage = (uint64_t)cval.val;
	/*
	 * Three chunks, plus one page for each participant in up to three concurrent merges.
	 */
	required = 3 * lsm_tree->chunk_size + 3 * (lsm_tree->merge_max * maxleafpage);
	if(S2C(session)->cache_size < required){ /*�������õ�cache size����ӡһ��������Ϣ��������һ������ֵ*/
		WT_RET_MSG(session, EINVAL,
			"LSM cache size %" PRIu64 " (%" PRIu64 "MB) too small, "
			"must be at least %" PRIu64 " (%" PRIu64 "MB)",
			S2C(session)->cache_size,
			S2C(session)->cache_size / WT_MEGABYTE,
			required, required / WT_MEGABYTE);
	}

	return 0;
}

/*��һ��lsm tree,��ʵ���ǽ�lsm tree�����ڴ���*/
static int __lsm_tree_open(WT_SESSION_IMPL* session, const char* uri, WT_LSM_TREE** treep)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	conn = S2C(session);
	lsm_tree = NULL;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED));

	/* Start the LSM manager thread if it isn't running. */
	if (WT_ATOMIC_CAS4(conn->lsm_manager.lsm_workers, 0, 1))
		WT_RET(__wt_lsm_manager_start(session));

	/*����lsm tree����*/
	if(ret = __lsm_tree_find(session, uri, 0, treep) != WT_NOTFOUND)
		return ret;

	/*����open lsm tree����*/
	WT_RET(__wt_calloc_one(session, &lsm_tree));
	WT_ERR(__wt_rwlock_alloc(session, &lsm_tree->rwlock, "lsm tree"));
	WT_ERR(__lsm_tree_set_name(session, lsm_tree, uri));
	WT_ERR(__wt_lsm_meta_read(session, lsm_tree));

	WT_ERR(__lsm_tree_open_check(session, lsm_tree));

	lsm_tree->dsk_gen = 1;
	lsm_tree->refcnt = 1;
	lsm_tree->queue_ref = 0;
	/*����flush��ʱ���*/
	WT_ERR(__wt_epoch(session, &lsm_tree->last_flush_ts));

	/*����lsm tree��״̬*/
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);
	F_SET(lsm_tree, WT_LSM_TREE_ACTIVE | WT_LSM_TREE_OPEN);

	*treep = lsm_tree;

	if (0) {
err:		WT_TRET(__lsm_tree_discard(session, lsm_tree, 0));
	}

	return ret;
}

/*ͨ��uri����lsm tree��Ϣ�����û���ҵ���Ӧ��lsm tree����ͨ��meta��Ϣopenһ��lsm tree����*/
int __wt_lsm_tree_get(WT_SESSION_IMPL *session, const char *uri, int exclusive, WT_LSM_TREE **treep)
{
	WT_DECL_RET;
	WT_ASSERT(session, F_ISSET(session, uri, exclusive, treep));
	ret = __lsm_tree_find(session, uri, exclusive, treep);
	if(ret == WT_NOTFOUND)
		ret = __lsm_tree_open(session, uri, treep);

	return ret;
}

/*releaseһ��lsm tree����һ����ʹ��(wt_lsm_tree_open)�����*/
void __wt_lsm_tree_release(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree)
{
	WT_ASSERT(session, lsm_tree->refcnt > 1);
	if (lsm_tree->exclusive)
		lsm_tree->exclusive = 0;
	(void)WT_ATOMIC_SUB4(lsm_tree->refcnt, 1);
}

/* How aggressively to ramp up or down throttle due to level 0 merging */
#define	WT_LSM_MERGE_THROTTLE_BUMP_PCT	(100 / lsm_tree->merge_max)
/* Number of level 0 chunks that need to be present to throttle inserts */
#define	WT_LSM_MERGE_THROTTLE_THRESHOLD	(2 * lsm_tree->merge_min)
/* Minimal throttling time */
#define	WT_LSM_THROTTLE_START			20

#define	WT_LSM_MERGE_THROTTLE_INCREASE(val)	do {				\
	(val) += ((val) * WT_LSM_MERGE_THROTTLE_BUMP_PCT) / 100;	\
	if ((val) < WT_LSM_THROTTLE_START)							\
	(val) = WT_LSM_THROTTLE_START;								\
} while (0)

#define	WT_LSM_MERGE_THROTTLE_DECREASE(val)	do {				\
	(val) -= ((val) * WT_LSM_MERGE_THROTTLE_BUMP_PCT) / 100;	\
	if ((val) < WT_LSM_THROTTLE_START)							\
	(val) = 0;													\
} while (0)

/*ͨ��LRU Cache״̬����checkpoint/merge��ʱ����ֵ��������chunk����ƽ��ʱ��*/
void __wt_lsm_tree_throttle(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, int decrease_only)
{
	WT_LSM_CHUNK *last_chunk, **cp, *ondisk, *prev_chunk;
	uint64_t cache_sz, cache_used, oldtime, record_count, timediff;
	uint32_t in_memory, gen0_chunks;

	/* lsm tree��chunk̫�٣��������lsm tree����ֵ���� */
	if (lsm_tree->nchunks < 3) {
		lsm_tree->ckpt_throttle = lsm_tree->merge_throttle = 0;
		return;
	}

	cache_sz = S2C(session)->cache_size;

	/*ȷ�����һ�����̵�chunk���ڴ���chunk�ĸ���*/
	record_count = 1;
	gen0_chunks = in_memory = 0;
	ondisk = NULL;
	for (cp = lsm_tree->chunk + lsm_tree->nchunks - 1; cp >= lsm_tree->chunk; --cp)
		if (!F_ISSET(*cp, WT_LSM_CHUNK_ONDISK)) { /*�����ڴ���chunk�������ڴ��м�¼��*/
			record_count += (*cp)->count;
			++in_memory;
		} else {
			/*ȷ�����һ�����̵�chunk*/
			if (ondisk == NULL && ((*cp)->generation == 0 && !F_ISSET(*cp, WT_LSM_CHUNK_STABLE)))
				ondisk = *cp;

			/*���chunkû�н��й�merge����*/
			if ((*cp)->generation == 0 && !F_ISSET(*cp, WT_LSM_CHUNK_MERGING))
				++gen0_chunks;
		}

	last_chunk = lsm_tree->chunk[lsm_tree->nchunks - 1];

	/*�ڴ��е�chunk����̫�ٻ�����Ҫcheckpoint*/
	if(!F_ISSET(lsm_tree, WT_LSM_TREE_THROTTLE) || in_memory <= 3)
		lsm_tree->ckpt_throttle = 0;
	else if(decrease_only)
		;
	else if(ondisk == NULL) /*û��chunk���̣���ôcheckpoint ʱ����ֵ�����Ŵ�2��*/
		lsm_tree->ckpt_throttle = WT_MAX(WT_LSM_THROTTLE_START, 2 * lsm_tree->ckpt_throttle);
	else{
		/*�ڴ���chunk������¼������ʱ��������λ��΢��*/
		WT_ASSERT(session, WT_TIMECMP(last_chunk->create_ts, ondisk->create_ts) >= 0);
		timediff = WT_TIMEDIFF(last_chunk->create_ts, ondisk->create_ts);
		lsm_tree->ckpt_throttle = (in_memory - 2) * timediff / (20 * record_count);

		/*�ж��ڴ��е�chunkռ�õ��ڴ��Ƿ񳬹���cache��������̶ȣ������������checkpoint ��ֵ�ŵ�5��,����checkpoint�Ľ���*/
		cache_used = in_memory * lsm_tree->chunk_size * 2;
		if (cache_used > cache_sz * 0.8)
			lsm_tree->ckpt_throttle *= 5;
	}

	/*lsm tree ��chunk��merge��������merge, ��ô��Ҫ����merge throttle*/
	if (F_ISSET(lsm_tree, WT_LSM_TREE_MERGES)) {
		/*û�г���merge�����chunk,��ô����merge���ٶ�*/
		if (lsm_tree->nchunks < lsm_tree->merge_max)
			lsm_tree->merge_throttle = 0;
		else if (gen0_chunks < WT_LSM_MERGE_THROTTLE_THRESHOLD) /*û�в�����chunk��С��merge��chunk����Сֵ������merge���ٶ�*/
			WT_LSM_MERGE_THROTTLE_DECREASE(lsm_tree->merge_throttle);
		else if (!decrease_only)
			WT_LSM_MERGE_THROTTLE_INCREASE(lsm_tree->merge_throttle);
	}

	/*checkpoint��merge��Ƶ�ʲ�С��1��*/
	lsm_tree->ckpt_throttle = WT_MIN(1000000, lsm_tree->ckpt_throttle);
	lsm_tree->merge_throttle = WT_MIN(1000000, lsm_tree->merge_throttle);

	/*�����chunk�����ˣ���ô����chunk �����ƽ��ʱ��,��ûŪ����Ϊʲô����������ģ�*/
	if (in_memory > 1 && ondisk != NULL) {
		prev_chunk = lsm_tree->chunk[lsm_tree->nchunks - 2];
		WT_ASSERT(session, prev_chunk->generation == 0);

		WT_ASSERT(session, WT_TIMECMP(last_chunk->create_ts, prev_chunk->create_ts) >= 0);
		timediff = WT_TIMEDIFF(last_chunk->create_ts, prev_chunk->create_ts);

		WT_ASSERT(session, WT_TIMECMP(prev_chunk->create_ts, ondisk->create_ts) >= 0);
		oldtime = WT_TIMEDIFF(prev_chunk->create_ts, ondisk->create_ts);
		if (timediff < 10 * oldtime)
			lsm_tree->chunk_fill_ms = (3 * lsm_tree->chunk_fill_ms + timediff / 1000000) / 4;
	}
}

/*���ڴ����л�lsm tree*/
int __wt_lsm_tree_switch(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, *last_chunk;
	uint32_t chunks_moved, nchunks, new_id;
	int first_switch;

	WT_RET(__wt_lsm_tree_writelock(session, lsm_tree));

	nchunks = lsm_tree->nchunks;
	first_switch = (nchunks == 0 ? 1 : 0); /*����û���κ�chunk,�����ǵ�һ��switch*/

	last_chunk = NULL;
	if (!first_switch && (last_chunk = lsm_tree->chunk[nchunks - 1]) != NULL &&
		!F_ISSET(last_chunk, WT_LSM_CHUNK_ONDISK) && !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
		goto err;

	/* Update the throttle time. */
	__wt_lsm_tree_throttle(session, lsm_tree, 0);

	/*ȷ��һ���µ�chunk ID*/
	new_id = WT_ATOMIC_ADD4(lsm_tree->last, 1);

	WT_ERR(__wt_realloc_def(session, &lsm_tree->chunk_alloc, nchunks + 1, &lsm_tree->chunk));

	WT_ERR(__wt_verbose(session, WT_VERB_LSM,
		"Tree %s switch to: %" PRIu32 ", checkpoint throttle %" PRIu64
		", merge throttle %" PRIu64, lsm_tree->name, new_id, lsm_tree->ckpt_throttle, lsm_tree->merge_throttle));

	WT_ERR(__wt_calloc_one(session, &chunk));
	chunk->id = new_id;
	chunk->switch_txn = WT_TXN_NONE;
	lsm_tree->chunk[lsm_tree->nchunks++] = chunk;
	/*��ʼ��lsm tree chunk��*/
	WT_ERR(__wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));
	/*д��meta��Ϣ*/
	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	F_CLR(lsm_tree, WT_LSM_TREE_NEED_SWITCH);
	++lsm_tree->dsk_gen;

	lsm_tree->modified = 1;

	/*ȷ��һ���µ�����ID*/
	if (last_chunk != NULL && last_chunk->switch_txn == WT_TXN_NONE && !F_ISSET(last_chunk, WT_LSM_CHUNK_ONDISK))
		last_chunk->switch_txn = __wt_txn_new_id(session);

	/*����lsm tree ��������chunk �������ƣ���ô��Ҫchunk�������̭,����̭��chunk��������old chunks�б���*/
	if (lsm_tree->chunk_count_limit != 0 && lsm_tree->nchunks > lsm_tree->chunk_count_limit) {
		chunks_moved = lsm_tree->nchunks - lsm_tree->chunk_count_limit;
		/* Move the last chunk onto the old chunk list. */
		WT_ERR(__wt_lsm_tree_retire_chunks(session, lsm_tree, 0, chunks_moved));

		lsm_tree->nchunks -= chunks_moved;
		memmove(lsm_tree->chunk, lsm_tree->chunk + chunks_moved, lsm_tree->nchunks * sizeof(*lsm_tree->chunk));
		memset(lsm_tree->chunk + lsm_tree->nchunks, 0, chunks_moved * sizeof(*lsm_tree->chunk));

		/* ��յ�old chunks�еĶ���chunk����ֻ��������û���κ����ü�¼����ɾ����Ӧ���ļ�*/
		WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_DROP, 0, lsm_tree));
	}

err:	
	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	if (ret != 0)
		WT_PANIC_RET(session, ret, "Failed doing LSM switch");
	else if (!first_switch) /*������ǵ�һ��switch,����Ҫ����һ��flush���̵Ĳ���*/
		WT_RET(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
	return ret;
}

/*��chunks����ɵ�n��chunk�Ƶ�old chunks*/
int __wt_lsm_tree_retire_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int start_chunk, u_int nchunks)
{
	u_int i;

	WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);

	/* Setup the array of obsolete chunks. */
	WT_RET(__wt_realloc_def(session, &lsm_tree->old_alloc, lsm_tree->nold_chunks + nchunks, &lsm_tree->old_chunks));

	/* Copy entries one at a time, so we can reuse gaps in the list. */
	for (i = 0; i < nchunks; i++)
		lsm_tree->old_chunks[lsm_tree->nold_chunks++] = lsm_tree->chunk[start_chunk + i];

	return 0;
}

/*dropһ��lsm tree��*/
int __wt_lsm_tree_drop(WT_SESSION_IMPL* session, const char* name, const char* cfg[])
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	u_int i;
	int locked;

	locked = 0;

	/* Get the LSM tree. */
	WT_WITH_DHANDLE_LOCK(session, ret = __wt_lsm_tree_get(session, name, 1, &lsm_tree));
	WT_RET(ret);

	/*�ر�lsm tree�Ĺ���״̬,����������еĹ�������*/
	WT_ERR(__lsm_tree_close(session, lsm_tree));
	/*���lsm tree��д������ֹ�����������´�*/
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = 1;

	for(i = 0; i < lsm_tree->nchunks; i++){
		chunk = lsm_tree->chunk[i];
		/*��schema��ɾ������Ӧ����Ϣ������ bloom filter��Ϣ*/
		WT_ERR(__wt_schema_drop(session, chunk->uri, cfg));
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(__wt_schema_drop(session, chunk->bloom_uri, cfg));
	}
	
	/*ɾ�������е�old chunks schema*/
	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		if ((chunk = lsm_tree->old_chunks[i]) == NULL)
			continue;
		WT_ERR(__wt_schema_drop(session, chunk->uri, cfg));
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(__wt_schema_drop(session, chunk->bloom_uri, cfg));
	}

	locked = 0;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));
	ret = __wt_metadata_remove(session, name);

err:
	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	/*�ͷ�����chunks,������lsm tree����*/
	WT_WITH_DHANDLE_LOCK(session, WT_TRET(__lsm_tree_discard(session, lsm_tree, 0)));
	return (ret);
}

/*�޸�lsm tree��uri��Ӧ��ϵ���������޸ĺ��meta��Ϣ*/
int __wt_lsm_tree_rename(WT_SESSION_IMPL* session, const char* olduri, const char* newuri, const char** cfg[])
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	const char *old;
	u_int i;
	int locked;

	old = NULL;
	locked = 0;

	/*ͨ��uri��ȡlsm tree����*/
	WT_WITH_DHANDLE_LOCK(session, ret = __wt_lsm_tree_get(session, olduri, 1, &lsm_tree));
	WT_RET(ret);

	/*ֹͣlsm ��������*/
	WT_ERR(__lsm_tree_close(session, lsm_tree));
	/*��ռд������ֹ���������*/
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = 1;

	/* Set the new name. */
	WT_ERR(__lsm_tree_set_name(session, lsm_tree, newuri));

	/*���¸���chunk��meta��Ϣ��schema��Ϣ*/
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		old = chunk->uri;
		chunk->uri = NULL;

		WT_ERR(__wt_lsm_tree_chunk_name(session, lsm_tree, chunk->id, &chunk->uri));
		WT_ERR(__wt_schema_rename(session, old, chunk->uri, cfg));
		__wt_free(session, old);

		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			old = chunk->bloom_uri;
			chunk->bloom_uri = NULL;
			WT_ERR(__wt_lsm_tree_bloom_name(session, lsm_tree, chunk->id, &chunk->bloom_uri));
			F_SET(chunk, WT_LSM_CHUNK_BLOOM);
			WT_ERR(__wt_schema_rename(session, old, chunk->uri, cfg));
			__wt_free(session, old);
		}
	}

	/*д��һ���µ�meta����ɾ���ɵĶ���*/
	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	locked = 0;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));
	WT_ERR(__wt_metadata_remove(session, olduri));

err:
	if(locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if(old != NULL)
		__wt_free(session, old);

	WT_WITH_DHANDLE_LOCK(session, WT_TRET(__lsm_tree_discard(session, lsm_tree, 0)));

	return ret;
}

/*���lsm tree��s-lock*/
int __wt_lsm_tree_readlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_RET(__wt_readlock(session, lsm_tree->rwlock));
	/* ��ֹschema lock������������Ϊ�˱ܿ�����������WT_SESSION_NO_CACHE_CHECK��WT_SESSION_NO_SCHEMA_LOCK����ѡ��
	 * Diagnostic: avoid deadlocks with the schema lock: if we need it for
	 * an operation, we should already have it.
	 */
	F_SET(session, WT_SESSION_NO_CACHE_CHECK | WT_SESSION_NO_SCHEMA_LOCK);
}
/*�ͷ�lsm tree��s-lock*/
int __wt_lsm_tree_readunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;

	F_CLR(session, WT_SESSION_NO_CACHE_CHECK | WT_SESSION_NO_SCHEMA_LOCK);

	if ((ret = __wt_readunlock(session, lsm_tree->rwlock)) != 0)
		WT_PANIC_RET(session, ret, "Unlocking an LSM tree");

	return 0;
}

/*���lsm tree��x-lock*/
int __wt_lsm_tree_writelock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_RET(__wt_writelock(session, lsm_tree->rwlock));
	
	F_SET(session, WT_SESSION_NO_CACHE_CHECK | WT_SESSION_NO_SCHEMA_LOCK);
	return 0;
}
/*�ͷ�lsm tree��s-lock*/
int __wt_lsm_tree_writeunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;

	F_CLR(session, WT_SESSION_NO_CACHE_CHECK | WT_SESSION_NO_SCHEMA_LOCK);

	if ((ret = __wt_writeunlock(session, lsm_tree->rwlock)) != 0)
		WT_PANIC_RET(session, ret, "Unlocking an LSM tree");
	return 0;
}

#define	COMPACT_PARALLEL_MERGES	5

/*��lsm stree��compact�����������������__wt_schema_worker*/
int __wt_lsm_compact(WT_SESSION_IMPL *session, const char *name, int *skip)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	time_t begin, end;
	uint64_t progress;
	int i, compacting, flushing, locked, ref;

	compacting = flushing = locked = ref = 0;
	chunk = NULL;

	if(!WT_PREFIX_MATCH(name, "lsm:"))
		return 0;

	/*��skip����Ϊ1����ʾ�Ѿ���ʼcompact,__wt_schema_worker����compactǰ��������ʾ*/
	*skip = 1;

	WT_WITH_DHANDLE_LOCK(session,ret = __wt_lsm_tree_get(session, name, 0, &lsm_tree));
	WT_RET(ret);

	if (!F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
		WT_ERR_MSG(session, EINVAL, "LSM compaction requires active merge threads");

	/*��¼��ʼʱ��*/
	WT_ERR(__wt_seconds(session, &begin));

	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = 1;

	/*�����merge ��ֵ����ֹͬʱmerge����*/
	lsm_tree->merge_throttle = 0;
	lsm_tree->merge_aggressiveness = 0;
	progress = lsm_tree->merge_progressing;

	/* If another thread started a compact on this tree, we're done. */
	if (F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING))
		goto err;

	/*�ж������һ��chunk�Ƿ��ڴ����ϣ���ô��Ҫ�����ø�switch ����ID*/
	if (lsm_tree->nchunks > 0 && (chunk = lsm_tree->chunk[lsm_tree->nchunks - 1]) != NULL) {
		if (chunk->switch_txn == WT_TXN_NONE)
			chunk->switch_txn = __wt_txn_new_id(session);
		/*
		* If we have a chunk, we want to look for it to be on-disk.
		* So we need to add a reference to keep it available.
		*/
		(void)WT_ATOMIC_ADD4(chunk->refcnt, 1);
		ref = 1;
	}

	locked = 0;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if(chunk != NULL){
		WT_ERR(__wt_verbose(session, WT_VERB_LSM,
			"Compact force flush %s flags 0x%" PRIx32
			" chunk %u flags 0x%" PRIx32, name, lsm_tree->flags, chunk->id, chunk->flags));
		flushing = 1;
		/*��chunk����flush���̲���*/
		WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, WT_LSM_WORK_FORCE, lsm_tree));
	}
	else{
		compacting = 1;
		progress = lsm_tree->merge_progressing;
		F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
		WT_ERR(__wt_verbose(session, WT_VERB_LSM, "COMPACT: Start compacting %s", lsm_tree->name));
	}

	/*�ȴ������߳���ɶ�Ӧ������*/
	while(F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE)){
		if(flushing){
			WT_ASSERT(session, chunk != NULL);
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) { /*chunk�Ѿ�������,�ɽ���compacting��*/
				WT_ERR(__wt_verbose(session, WT_VERB_LSM,
					"Compact flush done %s chunk %u.  Start compacting progress %" PRIu64, name, chunk->id, lsm_tree->merge_progressing));

				(void)WT_ATOMIC_SUB4(chunk->refcnt, 1);
				flushing = ref = 0;
				compacting = 1;
				F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
				progress = lsm_tree->merge_progressing;
			}
			else{
				WT_ERR(__wt_verbose(session, WT_VERB_LSM, "Compact flush retry %s chunk %u", name, chunk->id));
				WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, WT_LSM_WORK_FORCE, lsm_tree));
			}
		}

		/*����compacting״̬*/
		if (compacting && !F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING)) {
			if (lsm_tree->merge_aggressiveness < 10 || (progress < lsm_tree->merge_progressing) || lsm_tree->merge_syncing) {
				progress = lsm_tree->merge_progressing;
				F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
				lsm_tree->merge_aggressiveness = 10;
			} 
			else
				break;
		}

		__wt_sleep(1, 0);
		WT_ERR(__wt_seconds(session, &end));
		/*���г���ʱ���жϣ��������compactʱ�䣬ֱ���˳�*/
		if (session->compact->max_time > 0 && session->compact->max_time < (uint64_t)(end - begin)) {
			WT_ERR(ETIMEDOUT);
		}
		
		/*ÿ�η���5��merge����*/
		if (compacting){
			for (i = lsm_tree->queue_ref; i < COMPACT_PARALLEL_MERGES; i++) {
				lsm_tree->merge_aggressiveness = 10;
				WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));
			}
		}
	}

err:
	/*����refcount*/
	if (ref)
		(void)WT_ATOMIC_SUB4(chunk->refcnt, 1);

	/*�Ѿ����compact����������״̬���*/
	if (compacting) {
		F_CLR(lsm_tree, WT_LSM_TREE_COMPACTING);
		lsm_tree->merge_aggressiveness = 0;
	}

	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	WT_TRET(__wt_verbose(session, WT_VERB_LSM, "Compact %s complete, return %d", name, ret));
	__wt_lsm_tree_release(session, lsm_tree);

	return ret;
}

/*��lsm tree��ÿһ������һ��schema worker,��schema_worker����������������,�������������໥�ݹ�ģ�����uri�йؼ������б����*/
int __wt_lsm_tree_worker(WT_SESSION_IMPL *session, const char *uri,
	int (*file_func)(WT_SESSION_IMPL *, const char *[]),
	int (*name_func)(WT_SESSION_IMPL *, const char *, int *),
	const char *cfg[], uint32_t open_flags)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	u_int i;
	int exclusive, locked;

	locked = 0;
	exclusive = FLD_ISSET(open_flags, WT_DHANDLE_EXCLUSIVE) ? 1 : 0;
	WT_WITH_DHANDLE_LOCK(session, ret = __wt_lsm_tree_get(session, uri, exclusive, &lsm_tree));
	WT_RET(ret);

	/*��ò����Ƿ�����Ҫ��ռ��*/
	WT_ERR(exclusive ? __wt_lsm_tree_writelock(session, lsm_tree) : __wt_lsm_tree_readlock(session, lsm_tree));
	locked = 1;

	/*��ÿһ��chunk���в���ɨ��*/
	for(i = 0; i < lsm_tree->nchunks; i++){
		chunk = lsm_tree->chunk[i];
		if (file_func == __wt_checkpoint && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) /*chunk�Ѿ����̣�����checkpoint*/
			continue;
		/*ִ��schema worker����������lsm_tree����*/
		WT_ERR(__wt_schema_worker(session, chunk->uri, file_func, name_func, cfg, open_flags));

		if (name_func == __wt_backup_list_uri_append && F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(__wt_schema_worker(session, chunk->bloom_uri, file_func, name_func, cfg, open_flags));
	}
err:
	if (locked)
		WT_TRET(exclusive ? __wt_lsm_tree_writeunlock(session, lsm_tree) : __wt_lsm_tree_readunlock(session, lsm_tree));
	__wt_lsm_tree_release(session, lsm_tree);
}



