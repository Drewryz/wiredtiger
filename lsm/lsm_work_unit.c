/*****************************************************************************
*
*****************************************************************************/

#include "wt_internal.h"

static int __lsm_bloom_create(WT_SESSION_IMPL* , WT_LSM_TREE* , WT_LSM_CHUNK*, u_int);
static int __lsm_discard_handle(WT_SESSION_IMPL*, const char* , const char*);

/*��lsm tree�е�chunksָ�����鿽����cookie->chunk_array��*/
static int __lsm_copy_chunks(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree, WT_LSM_WORKER_COOKIE* cookie, int old_chunks)
{
	WT_DECL_RET;
	u_int i, nchunks;
	size_t alloc;

	cookie->nchunks = 0;

	/*���lsm tree�Ķ���Ȩ�ޣ���Ϊֻ�ǿ���ָ�룬����ʹ��spin read/write lock*/
	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));

	if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
		return (__wt_lsm_tree_readunlock(session, lsm_tree));

	nchunks = old_chunks ? lsm_tree->nold_chunks : lsm_tree->nchunks;
	alloc = old_chunks ? lsm_tree->old_alloc : lsm_tree->chunk_alloc;

	if (cookie->chunk_alloc < alloc)
		WT_ERR(__wt_realloc(session, &cookie->chunk_alloc, alloc, &cookie->chunk_array));

	if (nchunks > 0)
		memcpy(cookie->chunk_array, old_chunks ? lsm_tree->old_chunks : lsm_tree->chunk, nchunks * sizeof(*cookie->chunk_array));

	/*Ϊÿһ��chunk����һ�����ü���,�ڴ�array��ɾ����ʱ���ٵݼ�*/
	for (i = 0; i < nchunks; i++)
		WT_ATOMIC_ADD4(cookie->chunk_array[i]->refcnt, 1);

err:
	WT_TRET(__wt_lsm_tree_readunlock(session, lsm_tree));

	if(ret == 0)
		cookie->nchunks = nchunks;

	return ret;
}

/*ȷ��һ������flush��chunk*/
int __wt_lsm_get_chunk_to_flush(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, int force, WT_LSM_CHUNK **chunkp)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, *evict_chunk, *flush_chunk;
	u_int i;

	*chunkp = NULL;
	chunk = evict_chunk = flush_chunk = NULL;

	WT_ASSERT(session, lsm_tree->queue_ref > 0);
	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));

	if(!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE) || lsm_tree->nchunks == 0)
		return __wt_lsm_tree_readunlock(session, lsm_tree);

	for(i = 0; i < lsm_tree->nchunks; i ++){
		chunk = lsm_tree->chunk[i];
		
		if(F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)){
			/*
			 * Normally we don't want to force out the last chunk.
			 * But if we're doing a forced flush on behalf of a
			 * compact, then we want to include the final chunk.
			 */
			if (evict_chunk == NULL && !chunk->evicted && !F_ISSET(chunk, WT_LSM_CHUNK_STABLE))
				evict_chunk = chunk;
		}
		else if (flush_chunk == NULL && chunk->switch_txn != 0 && (force || i < lsm_tree->nchunks - 1)){
			flush_chunk = chunk;
		}
	}

	/*ȷ����Ҫflush��chunk*/
	if (evict_chunk != NULL && flush_chunk != NULL) {
		chunk = (__wt_random(session->rnd) & 1) ? evict_chunk : flush_chunk;
		WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
	} 
	else
		chunk = (evict_chunk != NULL) ? evict_chunk : flush_chunk;

	if (chunk != NULL) {
		WT_ERR(__wt_verbose(session, WT_VERB_LSM, "Flush%s: return chunk %u of %u: %s", force ? " w/ force" : "",
			i, lsm_tree->nchunks, chunk->uri));

		/*�������ü���*/
		(void)WT_ATOMIC_ADD4(chunk->refcnt, 1);
	}

err:
	WT_RET(__wt_lsm_tree_readunlock(session, lsm_tree));
	*chunkp = chunk;
	return ret;
}

/*��cookie�е�chunkȫ��ȥ�����ü���*/
static void __lsm_unpin_chunks(WT_SESSION_IMPL* session, WT_LSM_WORKER_COOKIE* cookie)
{
	u_int i;

	for (i = 0; i < cookie->nchunks; i++) {
		if (cookie->chunk_array[i] == NULL)
			continue;

		WT_ASSERT(session, cookie->chunk_array[i]->refcnt > 0);
		WT_ATOMIC_SUB4(cookie->chunk_array[i]->refcnt, 1);
	}

	/* Ensure subsequent calls don't double decrement. */
	cookie->nchunks = 0;
}

/*���lsm tree�Ƿ���Ҫ����chunk switch�������Ҫ������__wt_lsm_tree_switch����switch�����__wt_lsm_tree_switch
 * æ״̬�����¼���һ��switch�źŵ�lsm manager��*/
int __wt_lsm_work_switch(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT **entryp, int *ran)
{
	WT_DECL_RET;
	WT_LSM_WORK_UNIT *entry;

	entry = *entryp;
	*ran = 0;
	*entryp = NULL;

	if (F_ISSET(entry->lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
		WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_switch(session, entry->lsm_tree));
		/* Failing to complete the switch is fine */
		if (ret == EBUSY) {
			if (F_ISSET(entry->lsm_tree, WT_LSM_TREE_NEED_SWITCH))
				WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, entry->lsm_tree));

			ret = 0;
		} 
		else
			*ran = 1;
	}

err:
	__wt_lsm_manager_free_work_unit(session, entry);
	return ret;
}

/*������chunk�н���һ��bloom filter���ݣ�һ������chunk���̵�ʱ�����*/
int __wt_lsm_work_bloom(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	u_int i, merge;

	WT_CLEAR(cookie);

	/*����lsm_tree�е�chunk���ü���*/
	WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, 0));

	merge = 0;
	/*�����е�chunk����bloom filter*/
	for(i = 0; i < cookie.nchunks; i ++){
		chunk = cookie.chunk_array[i];

		/*�������ڴ����chunk*/
		if(!F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) || F_ISSET(chunk, WT_LSM_CHUNK_BLOOM | WT_LSM_CHUNK_MERGING) 
			|| chunk->generation > 0 || chunk->count == 0)
			continue;

		if (WT_ATOMIC_CAS4(chunk->bloom_busy, 0, 1)) {
			/*����bloom filter*/
			if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
				ret = __lsm_bloom_create(session, lsm_tree, chunk, (u_int)i);
				if (ret == 0)
					merge = 1;
			}

			chunk->bloom_busy = 0;
			break;
		}
	}

	/*����һ��chunk merge����,Ϊʲô�ᷢ��һ��merge�أ�����Ҫ��ϸ����*/
	if (merge)
		WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));

err:
	/*ɾ��chunks�����ü���*/
	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);

	return ret;
}

/*��chunk�ϵ�����flush��������*/
int __wt_lsm_checkpoint_chunk(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_DECL_RET;
	WT_TXN_ISOLATION saved_isolation;

	/*���chunk�Ѿ���������checkpoint�����Ǳ��뽫������Ϊ������״̬*/
	if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !F_ISSET(chunk, WT_LSM_CHUNK_STABLE) && !chunk->evicted) {
			if ((ret = __lsm_discard_handle(session, chunk->uri, NULL)) == 0)
				chunk->evicted = 1;
			else if (ret == EBUSY)
				ret = 0;
			else
				WT_RET_MSG(session, ret, "discard handle");
	}

	if(F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)){
		WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker %s already on disk", chunk->uri));
		return 0;
	}

	/*�������������ڲ������chunk����ôcheckpoint��Ҫ��ͣ*/
	__wt_txn_update_oldest(session);

	if (chunk->switch_txn == WT_TXN_NONE || !__wt_txn_visible_all(session, chunk->switch_txn)) {
		WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker %s: running transaction, return", chunk->uri));
		return 0;
	}

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker flushing %s", chunk->uri));

	/*��������ĸ������Ա�ʶ*/
	if ((ret = __wt_session_get_btree(session, chunk->uri, NULL, NULL, 0)) == 0) {
		saved_isolation = session->txn.isolation;
		session->txn.isolation = TXN_ISO_EVICTION;
		ret = __wt_cache_op(session, NULL, WT_SYNC_WRITE_LEAVES);
		session->txn.isolation = saved_isolation;
		WT_TRET(__wt_session_release_btree(session));
	}

	WT_RET(ret);

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker checkpointing %s", chunk->uri));

	WT_WITH_SCHEMA_LOCK(session,
		ret = __wt_schema_worker(session, chunk->uri, __wt_checkpoint, NULL, NULL, 0));

	if (ret != 0)
		WT_RET_MSG(session, ret, "LSM checkpoint");

	/* Now the file is written, get the chunk size. */
	WT_RET(__wt_lsm_tree_set_chunk_size(session, chunk));

	/* Update the flush timestamp to help track ongoing progress. */
	WT_RET(__wt_epoch(session, &lsm_tree->last_flush_ts));

	/* Lock the tree, mark the chunk as on disk and update the metadata. */
	WT_RET(__wt_lsm_tree_writelock(session, lsm_tree));
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);

	ret = __wt_lsm_meta_write(session, lsm_tree);
	++lsm_tree->dsk_gen;

	/* Update the throttle time. */
	__wt_lsm_tree_throttle(session, lsm_tree, 1);
	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (ret != 0)
		WT_RET_MSG(session, ret, "LSM metadata write");

	WT_RET(__wt_session_get_btree(session, chunk->uri, NULL, NULL, 0));
	__wt_btree_evictable(session, 1);
	WT_RET(__wt_session_release_btree(session));

	/* Make sure we aren't pinning a transaction ID. */
	__wt_txn_release_snapshot(session);

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker checkpointed %s", chunk->uri));

	/* Schedule a bloom filter create for our newly flushed chunk. */
	if (!FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF))
		WT_RET(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_BLOOM, 0, lsm_tree));
	else
		WT_RET(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));

	return 0;
}

/*Ϊchunk����һ��bloom filter*/
static int __lsm_bloom_create(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree, WT_LSM_CHUNK* chunk, u_int chunk_off)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src;
	WT_DECL_RET;
	WT_ITEM key;
	uint64_t insert_count;

	WT_RET(__wt_lsm_tree_setup_bloom(session, lsm_tree, chunk));

	bloom = NULL;

	++lsm_tree->merge_progressing;

	/*��������ʼ��һ��bloom filter*/
	WT_RET(__wt_bloom_create(session, chunk->bloom_uri, lsm_tree->bloom_config, chunk->count, 
								lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));

	/*����һ��lsm tree cursor*/
	WT_ERR(__wt_open_cursor(session, lsm_tree->name, NULL, NULL, &src));
	F_SET(src, WT_CURSTD_RAW);
	WT_ERR(__wt_clsm_init_merge(src, chunk_off, chunk->id, 1));

	F_SET(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_CACHE_CHECK);
	/*����bloom filter����*/
	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		WT_ERR(src->get_key(src, &key));
		WT_ERR(__wt_bloom_insert(bloom, &key));
	}

	WT_ERR_NOTFOUND_OK(ret);
	WT_TRET(src->close(src));

	/*��bloom filterд�뵽session��Ӧ��Ԫ������*/
	WT_TRET(__wt_bloom_finalize(bloom));
	WT_ERR(ret);

	F_CLR(session, WT_SESSION_NO_CACHE);

	/*У���¹�����bloom filter�Ƿ���ȷ*/
	WT_CLEAR(key);
	WT_ERR_NOTFOUND_OK(__wt_bloom_get(bloom, &key));

	WT_ERR(__wt_verbose(session, WT_VERB_LSM,
		"LSM worker created bloom filter %s. Expected %" PRIu64 " items, got %" PRIu64, chunk->bloom_uri, chunk->count, insert_count));

	/*д��bloom filter����*/
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	ret = __wt_lsm_meta_write(session, lsm_tree);
	++lsm_tree->dsk_gen;
	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (ret != 0)
		WT_ERR_MSG(session, ret, "LSM bloom worker metadata write");

err:
	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));

	F_CLR(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_CACHE_CHECK);

	return ret;
}

/*���Դ�cache�з���һ��handler��Ӧ������*/
static int __lsm_discard_handle(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
	WT_RET(__wt_session_get_btree(session, uri, checkpoint, NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

	F_SET(session->dhandle, WT_DHANDLE_DISCARD);
	return __wt_session_release_btree(session);
}

/*ɾ����lsm tree��һ���ļ�*/
static int __lsm_drop_file(WT_SESSION_IMPL* session, const char* uri)
{
	WT_DECL_RET;
	const char *drop_cfg[] = {
		WT_CONFIG_BASE(session, session_drop), "remove_files=false", NULL
	};

	/*��cache�������Ѿ�����checkpoint������*/
	WT_RET(__lsm_discard_handle(session, uri, WT_CHECKPOINT));
}


