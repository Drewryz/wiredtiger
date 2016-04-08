
#include "wt_internal.h"

/*һ��meta �����Ĺ켣��¼���൱����־�����Ը�������켣����unroll����*/
typedef struct __wt_meta_track{
	enum{
		WT_ST_EMPTY,
		WT_ST_CHECKPOINT,
		WT_ST_FILEOP,
		WT_ST_LOCK,
		WT_ST_REMOVE,
		WT_ST_SET
	} op;
	char *a, *b;
	WT_DATA_HANDLE* dhandle;
	int created;
}WT_META_TRACK;

/*�ڽ��м�¼meta trackʱ�����meta track���������㣬��Ҫ��������*/
static int __meta_track_next(WT_SESSION_IMPL* session, WT_META_TRACK** trkp)
{
	size_t offset, sub_off;

	if (session->meta_track_next == NULL)
		session->meta_track_next = session->meta_track;

	/*���㵱ǰtrackʵ�����ڵ�ƫ��λ�ã����Ը������ƫ��ȷ���Ƿ���Ҫ�������*/
	offset = WT_PTRDIFF(session->meta_track_next, session->meta_track);
	sub_off = WT_PTRDIFF(session->meta_track_sub, session->meta_track);
	if (offset == session->meta_track_alloc){
		WT_RET(__wt_realloc(session, &session->meta_track_alloc, WT_MAX(2 * session->meta_track_alloc, 20 * sizeof(WT_META_TRACK)), &session->meta_track));

		/*����ȷ�� track next��track sub��λ��*/
		session->meta_track_next = (uint8_t *)session->meta_track + offset;
		if (session->meta_track_sub != NULL)
			session->meta_track_sub = (uint8_t *)session->meta_track + sub_off;
	}

	WT_ASSERT(session, session->meta_track_next != NULL);

	/*�Ƿ�Ҫ����trackʵ�����룬���Ҫ����track next��Ӧ��ʵ��*/
	if (trkp != NULL){
		*trkp = session->meta_track_next;
		session->meta_track_next = *trkp + 1;
	}

	return 0;
}

/*�ͷŵ�session��Ӧ��meta track*/
void __wt_meta_track_discard(WT_SESSION_IMPL* session)
{
	__wt_free(session, session->meta_track);
	session->meta_track_next = NULL;
	session->meta_track_alloc = 0;
}

/*����meta track*/
int __wt_meta_track_on(WT_SESSION_IMPL* session)
{
	if (session->meta_track_nest++ == 0)
		WT_RET(__meta_track_next(session, NULL));

	return 0;
}

/*����meta track�в�����unroll���൱�ڲ����ع���*/
static int __meta_track_apply(WT_SESSION_IMPL *session, WT_META_TRACK *trk, int unroll)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	int tret;

	/*Unlock handles and complete checkpoints regardless of whether we are unrolling.*/
	if (!unroll && trk->op != WT_ST_CHECKPOINT && trk->op != WT_ST_LOCK)
		goto free;

	switch (trk->op) {
	case WT_ST_EMPTY:	/* Unused slot */
		break;
	case WT_ST_CHECKPOINT:	/* Checkpoint, see above */
		if (!unroll) {
			btree = trk->dhandle->handle;
			bm = btree->bm;
			WT_WITH_DHANDLE(session, trk->dhandle, WT_TRET(bm->checkpoint_resolve(bm, session)));
		}
		break;

	case WT_ST_LOCK:	/* Handle lock, see above */
		if (unroll && trk->created)
			F_SET(trk->dhandle, WT_DHANDLE_DISCARD);
		WT_WITH_DHANDLE(session, trk->dhandle, WT_TRET(__wt_session_release_btree(session)));
		break;

	case WT_ST_FILEOP:	/* File operation */
		if (trk->a != NULL && trk->b != NULL && (tret = __wt_rename(session, trk->b + strlen("file:"), trk->a + strlen("file:"))) != 0) {
			__wt_err(session, tret,"metadata unroll rename %s to %s", trk->b, trk->a);
			WT_TRET(tret);
		}
		else if (trk->a == NULL) {
			if ((tret = __wt_remove(session, trk->b + strlen("file:"))) != 0) {
				__wt_err(session, tret, "metadata unroll create %s", trk->b);
				WT_TRET(tret);
			}
		}
		/*
		* We can't undo removes yet: that would imply
		* some kind of temporary rename and remove in
		* roll forward.
		*/
		break;
	case WT_ST_REMOVE:	/* Remove trk.a */
		if ((tret = __wt_metadata_remove(session, trk->a)) != 0) {
			__wt_err(session, tret, "metadata unroll remove: %s", trk->a);
			WT_TRET(tret);
		}
		break;
	case WT_ST_SET:		/* Set trk.a to trk.b */
		if ((tret = __wt_metadata_update(session, trk->a, trk->b)) != 0) {
			__wt_err(session, tret, "metadata unroll update %s to %s", trk->a, trk->b);
			WT_TRET(tret);
		}
		break;
		WT_ILLEGAL_VALUE(session);
	}

free:	trk->op = WT_ST_EMPTY;
	__wt_free(session, trk->a);
	__wt_free(session, trk->b);
	trk->dhandle = NULL;

	return (ret);
}

/*�ر�meta track*/
int __wt_meta_track_off(WT_SESSION_IMPL* session, int need_sync, int unroll)
{
	WT_DECL_RET;
	WT_META_TRACK *trk, *trk_orig;

	WT_ASSERT(session, WT_META_TRACKING(session) && session->meta_track_nest > 0);

	trk_orig = session->meta_track;
	trk = session->meta_track_next;

	/*����Ƕ�׵�trackϵ�в����� û�е����ϲ�track��ֱ�ӷ��ؼ���*/
	if (--session->meta_track_nest != 0)
		return 0;

	/* Turn off tracking for unroll. �ر�track ����*/
	session->meta_track_next = session->meta_track_sub = NULL;

	/*û���κ�meta op��meta track��,ֱ���˳�*/
	if (trk == trk_orig)
		return 0;

	while (--trk >= trk_orig)
		WT_TRET(__meta_track_apply(session, trk, unroll));

	if (unroll || ret != 0 || !need_sync || session->meta_dhandle == NULL)
		return ret;

	/* If we're logging, make sure the metadata update was flushed. */
	if (FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED)) {
		if (!FLD_ISSET(S2C(session)->txn_logsync, WT_LOG_DSYNC | WT_LOG_FSYNC))
			WT_WITH_DHANDLE(session, session->meta_dhandle,
			ret = __wt_txn_checkpoint_log(session, 0, WT_TXN_LOG_CKPT_SYNC, NULL));
	}
	else {
		WT_WITH_DHANDLE(session, session->meta_dhandle, ret = __wt_checkpoint(session, NULL));
		WT_RET(ret);
		WT_WITH_DHANDLE(session, session->meta_dhandle, ret = __wt_checkpoint_sync(session, NULL));
	}

	return ret;
}

int __wt_meta_track_sub_on(WT_SESSION_IMPL *session)
{
	WT_ASSERT(session, session->meta_track_sub == NULL);
	session->meta_track_sub = session->meta_track_next;
	return (0);
}

int __wt_meta_track_sub_off(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_META_TRACK *trk, *trk_orig;

	if (!WT_META_TRACKING(session) || session->meta_track_sub == NULL)
		return (0);

	trk_orig = session->meta_track_sub;
	trk = session->meta_track_next;

	/* Turn off tracking for unroll. */
	session->meta_track_next = session->meta_track_sub = NULL;

	while (--trk >= trk_orig)
		WT_TRET(__meta_track_apply(session, trk, 0));

	session->meta_track_next = trk_orig;
	return (ret);
}

/*
* __wt_meta_track_checkpoint --
*	Track a handle involved in a checkpoint.
*/
int __wt_meta_track_checkpoint(WT_SESSION_IMPL *session)
{
	WT_META_TRACK *trk;

	WT_ASSERT(session, session->dhandle != NULL);

	WT_RET(__meta_track_next(session, &trk));

	trk->op = WT_ST_CHECKPOINT;
	trk->dhandle = session->dhandle;
	return 0;
}
/*
* __wt_meta_track_insert --
*	Track an insert operation.
*/
int __wt_meta_track_insert(WT_SESSION_IMPL *session, const char *key)
{
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));
	trk->op = WT_ST_REMOVE;
	WT_RET(__wt_strdup(session, key, &trk->a));

	return 0;
}

/*
* __wt_meta_track_update --
*	Track a metadata update operation.
*/
int __wt_meta_track_update(WT_SESSION_IMPL *session, const char *key)
{
	WT_DECL_RET;
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));
	trk->op = WT_ST_SET;
	WT_RET(__wt_strdup(session, key, &trk->a));

	/*
	* If there was a previous value, keep it around -- if not, then this
	* "update" is really an insert.
	*/
	if ((ret =
		__wt_metadata_search(session, key, &trk->b)) == WT_NOTFOUND) {
		trk->op = WT_ST_REMOVE;
		ret = 0;
	}
	return ret;
}

/*
* __wt_meta_track_fileop --
*	Track a filesystem operation.
*/
int __wt_meta_track_fileop(WT_SESSION_IMPL *session, const char *olduri, const char *newuri)
{
	WT_META_TRACK *trk;

	WT_RET(__meta_track_next(session, &trk));
	trk->op = WT_ST_FILEOP;
	if (olduri != NULL)
		WT_RET(__wt_strdup(session, olduri, &trk->a));
	if (newuri != NULL)
		WT_RET(__wt_strdup(session, newuri, &trk->b));
	return 0;
}

/*
* __wt_meta_track_handle_lock --
*	Track a locked handle.
*/
int __wt_meta_track_handle_lock(WT_SESSION_IMPL *session, int created)
{
	WT_META_TRACK *trk;

	WT_ASSERT(session, session->dhandle != NULL);

	WT_RET(__meta_track_next(session, &trk));
	trk->op = WT_ST_LOCK;
	trk->dhandle = session->dhandle;
	trk->created = created;

	return 0;
}







