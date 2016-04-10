
#include "wt_internal.h"

static int __session_dhandle_sweep(WT_SESSION_IMPL* session);


/*Ϊsession����һ��dhandle cache*/
static int __session_add_dhandle(WT_SESSION_IMPL* session, WT_DATA_HANDLE_CACHE **dhandle_cachep)
{
	WT_DATA_HANDLE_CACHE *dhandle_cache;
	uint64_t bucket;

	/*����һ��dhandle cache*/
	WT_RET(__wt_calloc_one(session, &dhandle_cache));
	dhandle_cache->dhandle = session->dhandle;

	/*��cache���뵽session cache list����*/
	bucket = dhandle_cache->dhandle->name_hash % WT_HASH_ARRAY_SIZE;
	SLIST_INSERT_HEAD(&session->dhandles, dhandle_cache, l);
	SLIST_INSERT_HEAD(&session->dhhash[bucket], dhandle_cache, hashl);

	if(dhandle_cachep != NULL)
		*dhandle_cachep = dhandle_cache;

	WT_ATOMIC_ADD4(session->dhandle->session_ref, 1);

	/*ɾ�����Ѿ��رջ��߲��õ�dhandle*/
	return __session_dhandle_sweep(session);
}

/*���Ի��session��ǰdhandle��lock*/
int __wt_session_lock_dhandle(WT_SESSION_IMPL *session, uint32_t flags)
{
	enum { NOLOCK, READLOCK, WRITELOCK } locked;
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	uint32_t special_flags;

	btree = S2BT(session);
	dhandle = session->dhandle;
	locked = NOLOCK;

	/*����Ƿ���Ҫ���´�btree file�Ĳ���*/
	special_flags = LF_ISSET(WT_BTREE_SPECIAL_FLAGS);
	WT_ASSERT(session, special_flags == 0 || LF_ISSET(WT_DHANDLE_EXCLUSIVE));

	/*�Ƕ�ռʽ���������Ի��write lock,�����ǻ��д���󣬻�����д��WT_DHANDLE_EXCLUSIVE��ʾ����ֹ�����߳�ͬ����ȡwrite lock��ɵȴ�*/
	if (LF_ISSET(WT_DHANDLE_EXCLUSIVE)){
		if (LF_ISSET(WT_DHANDLE_LOCK_ONLY) || special_flags == 0) {
			WT_RET(__wt_try_writelock(session, dhandle->rwlock));
			F_SET(dhandle, WT_DHANDLE_EXCLUSIVE);
			locked = WRITELOCK;
		}
	}
	else if(F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS))
		return EBUSY;
	else{
		WT_RET(__wt_readlock(session, dhandle->rwlock));
		locked = READLOCK;
	}
	/*����ǲ���Ҫ���´�btree�Ĳ�����dhandle��open״̬��ֱ�ӷ��ؼ���,��Ϊǰ���Ѿ������lock*/
	if (LF_ISSET(WT_DHANDLE_LOCK_ONLY) || (F_ISSET(dhandle, WT_DHANDLE_OPEN) && special_flags == 0))
		return 0;

	switch (locked) {
	case NOLOCK:
		break;
	case READLOCK:
		WT_RET(__wt_readunlock(session, dhandle->rwlock));
		break;
	case WRITELOCK:
		F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);
		WT_RET(__wt_writeunlock(session, dhandle->rwlock));
		break;
	}

	/* Treat an unopened handle just like a non-existent handle. */
	return (WT_NOTFOUND);
}

/*�ͷ�session��Ӧdhanle����*/
int __wt_session_release_btree(WT_SESSION_IMPL *session)
{
	enum { NOLOCK, READLOCK, WRITELOCK } locked;
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	btree = S2BT(session);
	dhandle = session->dhandle;

	locked = F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) ? WRITELOCK : READLOCK;
	if (F_ISSET(dhandle, WT_DHANDLE_DISCARD_CLOSE)) {
		/*
		 * If configured to discard on last close, trade any read lock
		 * for an exclusive lock. If the exchange succeeds, setup for
		 * discard. It is expected acquiring an exclusive lock will fail
		 * sometimes since the handle may still be in use: in that case
		 * we're done.
		 * �����WT_DHANDLE_DISCARD_CLOSE״̬����readlock����writelock,
		 * ����״̬���ó�WT_DHANDLE_EXCLUSIVE
		 */
		if (locked == READLOCK) {
			locked = NOLOCK;
			WT_ERR(__wt_readunlock(session, dhandle->rwlock));
			ret = __wt_try_writelock(session, dhandle->rwlock);
			if (ret != 0) {
				if (ret == EBUSY)
					ret = 0;
				goto err;
			}
			locked = WRITELOCK;
			F_CLR(dhandle, WT_DHANDLE_DISCARD_CLOSE);
			F_SET(dhandle, WT_DHANDLE_DISCARD | WT_DHANDLE_EXCLUSIVE); /*��ʾ�ر�btree file*/
		}
	}

	/*
	 * If we had special flags set, close the handle so that future access
	 * can get a handle without special flags.
	 */
	if (F_ISSET(dhandle, WT_DHANDLE_DISCARD) || F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS)) {
		WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));
		F_CLR(dhandle, WT_DHANDLE_DISCARD);

		WT_TRET(__wt_conn_btree_sync_and_close(session, 0, 0));
	}

	if (F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE))
		F_CLR(dhandle, WT_DHANDLE_EXCLUSIVE);

	/*�ͷ���*/
err:	
	switch (locked) {
	case NOLOCK:
		break;
	case READLOCK:
		WT_TRET(__wt_readunlock(session, dhandle->rwlock));
		break;
	case WRITELOCK:
		WT_TRET(__wt_writeunlock(session, dhandle->rwlock));
		break;
	}

	session->dhandle = NULL;
	return ret;
}

/*������cfg������file�����һ��checkpoint��Ӧ��btree����,������*/
int __wt_session_get_btree_ckpt(WT_SESSION_IMPL *session, const char *uri, const char *cfg[], uint32_t flags)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	int last_ckpt;
	const char *checkpoint;

	last_ckpt = 0;
	checkpoint = NULL;

	WT_RET_NOTFOUND_OK(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
	if(cval.len != 0){
		if (WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
			last_ckpt = 1;
retry:			
			WT_RET(__wt_meta_checkpoint_last_name(session, uri, &checkpoint));
		} 
		else
			WT_RET(__wt_strndup(session, cval.str, cval.len, &checkpoint));
	}

	/*
	 * There's a potential race: we get the name of the most recent unnamed
	 * checkpoint, but if it's discarded (or locked so it can be discarded)
	 * by the time we try to open it, we'll fail the open.  Retry in those
	 * cases, a new "last" checkpoint should surface, and we can't return an
	 * error, the application will be justifiably upset if we can't open the
	 * last checkpoint instance of an object.
	 *
	 * The check against WT_NOTFOUND is correct: if there was no checkpoint
	 * for the object (that is, the object has never been in a checkpoint),
	 * we returned immediately after the call to search for that name.
	 */
	ret = __wt_session_get_btree(session, uri, checkpoint, cfg, flags);
	__wt_free(session, checkpoint);

	/*���open btreeʱ���ز����ڻ���æ���������ԣ��п�������û��ͬ��*/
	if (last_ckpt && (ret == WT_NOTFOUND || ret == EBUSY))
		goto retry;
	return ret;
}

/*��dhandle_cache��session��cache��ɾ�������ͷ�dhandle_cache����*/
static void __session_discard_btree(WT_SESSION_IMPL* session, WT_DATA_HANDLE_CACHE* dhandle_cache)
{
	uint64_t bucket;

	bucket = dhandle_cache->dhandle->name_hash % WT_HASH_ARRAY_SIZE;
	SLIST_REMOVE(&session->dhandles, dhandle_cache, __wt_data_handle_cache, l);
	SLIST_REMOVE(&session->dhhash[bucket],dhandle_cache, __wt_data_handle_cache, hashl);

	(void)WT_ATOMIC_SUB4(dhandle_cache->dhandle->session_ref, 1);

	__wt_overwrite_and_free(session, dhandle_cache);
}

/*�ر�session��cache�����е�dhandle cache����*/
void __wt_session_close_cache(WT_SESSION_IMPL* session)
{
	WT_DATA_HANDLE_CACHE *dhandle_cache;

	while ((dhandle_cache = SLIST_FIRST(&session->dhandles)) != NULL)
		__session_discard_btree(session, dhandle_cache);
}

/*����session cache������û�д򿪵�dhandle*/
static int __session_dhandle_sweep(WT_SESSION_IMPL* session)
{
WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DATA_HANDLE_CACHE *dhandle_cache, *dhandle_cache_next;
	time_t now;

	conn = S2C(session);

	/*
	 * Periodically sweep for dead handles; if we've swept recently, don't
	 * do it again.
	 */
	WT_RET(__wt_seconds(session, &now));
	if (now - session->last_sweep < conn->sweep_interval)
		return 0;
	session->last_sweep = now;

	WT_STAT_FAST_CONN_INCR(session, dh_session_sweeps);

	dhandle_cache = SLIST_FIRST(&session->dhandles);
	while (dhandle_cache != NULL) {
		dhandle_cache_next = SLIST_NEXT(dhandle_cache, l);
		dhandle = dhandle_cache->dhandle;
		if (dhandle != session->dhandle && dhandle->session_inuse == 0 && now - dhandle->timeofdeath > conn->sweep_idle_time) {
			WT_STAT_FAST_CONN_INCR(session, dh_session_handles);
			__session_discard_btree(session, dhandle_cache);
		}
		dhandle_cache = dhandle_cache_next;
	}
	return 0;
}

/*����dhandle name��checkpoint name�ҵ���Ӧ��dhandle,����dhandle���뵽dhandle cache����*/
static int __session_dhandle_find(WT_SESSION_IMPL* session, const char* uri, const char* checkpoint, uint32_t flags)
{
	WT_RET(__wt_conn_dhandle_find(session, uri, checkpoint, flags));
	return __session_add_dhandle(session, NULL);
}

/*ͨ��dhandle name��checkpoint name�ҵ���Ӧ��dhande��btree file,����btree*/
int __wt_session_get_btree(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint, const char *cfg[], uint32_t flags)
{
	WT_DATA_HANDLE *dhandle;
	WT_DATA_HANDLE_CACHE *dhandle_cache;
	WT_DECL_RET;
	uint64_t bucket;

	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_NO_DATA_HANDLES));
	WT_ASSERT(session, !LF_ISSET(WT_DHANDLE_HAVE_REF));

	dhandle = NULL;

	/*��dhandle cache�в���*/
	bucket = __wt_hash_city64(uri, strlen(uri)) % WT_HASH_ARRAY_SIZE;
	SLIST_FOREACH(dhandle_cache, &session->dhhash[bucket], hashl) {
		dhandle = dhandle_cache->dhandle;
		if (strcmp(uri, dhandle->name) != 0)
			continue;
		if (checkpoint == NULL && dhandle->checkpoint == NULL)
			break;
		if (checkpoint != NULL && dhandle->checkpoint != NULL && strcmp(checkpoint, dhandle->checkpoint) == 0)
			break;
	}

	if(dhandle_cache != NULL)
		session->dhandle = dhandle;
	else{
		/*
		 * We didn't find a match in the session cache, now search the
		 * shared handle list and cache any handle we find.
		 */
		WT_WITH_DHANDLE_LOCK(session, ret = __session_dhandle_find(session, uri, checkpoint, flags));
		dhandle = (ret == 0) ? session->dhandle : NULL;
		WT_RET_NOTFOUND_OK(ret);
	}

	if (dhandle != NULL) {
		/* Try to lock the handle; if this succeeds, we're done. */
		if ((ret = __wt_session_lock_dhandle(session, flags)) == 0)
			goto done;

		/* Propagate errors we don't expect. */
		if (ret != WT_NOTFOUND && ret != EBUSY)
			return (ret);

		/*
		 * Don't try harder to get the btree handle if our caller
		 * hasn't allowed us to take the schema lock - they do so on
		 * purpose and will handle error returns.
		 */
		if (!F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) && F_ISSET(session, WT_SESSION_HANDLE_LIST_LOCKED | WT_SESSION_TABLE_LOCKED))
			return ret;

		/* We found the data handle, don't try to get it again. */
		LF_SET(WT_DHANDLE_HAVE_REF);
	}

	/*
	 * Acquire the schema lock and the data handle lock, find and/or
	 * open the handle.
	 *
	 * We need the schema lock for this call so that if we lock a handle in
	 * order to open it, that doesn't race with a schema-changing operation
	 * such as drop.
	 */
	WT_WITH_SCHEMA_LOCK(session, WT_WITH_DHANDLE_LOCK(session, ret = __wt_conn_btree_get(session, uri, checkpoint, cfg, flags)));
	WT_RET(ret);

	/*__wt_conn_btree_get�����л��½�dhandle,������½��ģ���ô���뽫dhandle���뵽dhandle cache����*/
	if (!LF_ISSET(WT_DHANDLE_HAVE_REF))
		WT_RET(__session_add_dhandle(session, NULL));

	WT_ASSERT(session, LF_ISSET(WT_DHANDLE_LOCK_ONLY) || F_ISSET(session->dhandle, WT_DHANDLE_OPEN));

done:	
	WT_ASSERT(session, LF_ISSET(WT_DHANDLE_EXCLUSIVE) == F_ISSET(session->dhandle, WT_DHANDLE_EXCLUSIVE));
	F_SET(session->dhandle, LF_ISSET(WT_DHANDLE_DISCARD_CLOSE));

	return 0;
}

/*����checkpoint��ȡ��session��Ӧdhandle��btree,����btree�е��ڴ�pageȫ��ˢ�����,������̻���
 *dhandle�Ķ�ռ��
 */
int __wt_session_lock_checkpoint(WT_SESSION_IMPL *session, const char *checkpoint)
{
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_DECL_RET;

	saved_dhandle = session->dhandle;

	/*
	 * Get the checkpoint handle exclusive, so no one else can access it
	 * while we are creating the new checkpoint.
	 */
	WT_ERR(__wt_session_get_btree(session, saved_dhandle->name, checkpoint, NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

	/*
	 * Flush any pages in this checkpoint from the cache (we are about to
	 * re-write the checkpoint which will mean cached pages no longer have
	 * valid contents).  This is especially noticeable with memory mapped
	 * files, since changes to the underlying file are visible to the in
	 * memory pages.
	 */
	WT_ERR(__wt_cache_op(session, NULL, WT_SYNC_DISCARD));

	/*
	 * We lock checkpoint handles that we are overwriting, so the handle
	 * must be closed when we release it.
	 */
	dhandle = session->dhandle;
	F_SET(dhandle, WT_DHANDLE_DISCARD);

	WT_ASSERT(session, WT_META_TRACKING(session));
	WT_ERR(__wt_meta_track_handle_lock(session, 0));

	/* Restore the original btree in the session. */
err:	session->dhandle = saved_dhandle;

	return ret;
}
