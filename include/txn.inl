
static inline int __wt_txn_id_check(WT_SESSION_IMPL* session);
static inline void  __wt_txn_read_last(WT_SESSION_IMPL* session);

/*��txn�����л��һ���������__wt_txn_op����*/
static inline int __txn_next_op(WT_SESSION_IMPL* session, WT_TXN_OP** opp)
{
	WT_TXN* txn;
	txn = &session->txn;
	*opp = NULL;

	/*��ִ�и���֮ǰ��ȷ��session��Ӧ�������Ѿ���һ������ID*/
	WT_RET(__wt_txn_id_check(session));
	WT_ASSERT(session, F_ISSET(txn, TXN_HAS_ID));

	/*�������Ĳ��������Ƿ����㹻���������һ��txn operation*/
	WT_RET(__wt_realloc_def(session, &txn->mod_alloc, txn->mod_count + 1, &txn->mod));

	/*���һ��txn_op����������Ų�����*/
	*opp = &txn->mod[txn->mod_count++];
	WT_CLEAR(**opp);
	(*opp)->fileid = S2BT(session)->id;

	return 0;
}

/*
* __wt_txn_unmodify --
*	If threads race making updates, they may discard the last referenced
*	WT_UPDATE item while the transaction is still active.  This function
*	removes the last update item from the "log".
*/
/*undo txn�����һ��modify����*/
static inline void __wt_txn_unmodify(WT_SESSION_IMPL* session)
{
	WT_TXN *txn;
	txn = &session->txn;

	if (F_ISSET(txn, TXN_HAS_ID)){
		WT_ASSERT(session, txn->mod_count > 0);
		txn->mod_count--;
	}
}

/*��session��Ӧ������Ĳ����б����һ��update����,���������������������*/
static inline int __wt_txn_modify(WT_SESSION_IMPL* session, WT_UPDATE* upd)
{
	WT_DECL_RET;
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = F_ISSET(session, WT_SESSION_LOGGING_INMEM) ? TXN_OP_INMEM : TXN_OP_BASIC;
	op->u.upd = upd;
	upd->txnid = session->txn.id;

	return ret;
}

/*���һ���޸�REF����Ĳ�������session��Ӧ������*/
static inline int __wt_txn_modify_ref(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = TXN_OP_REF;
	op->u.ref = ref;
	return __wt_txn_log_op(session, NULL);
}

/*�ж�����ID�Ƿ��ϵͳ�����е������ǿɼ���*/
static inline int __wt_txn_visible_all(WT_SESSION_IMPL* session, uint64_t id)
{
	WT_BTREE *btree;
	WT_TXN_GLOBAL *txn_global;
	uint64_t checkpoint_snap_min, oldest_id;

	txn_global = &S2C(session)->txn_global;
	btree = S2BT_SAFE(session);

	/*
	* Take a local copy of ID in case they are updated while we are checking visibility.
	* ����Ŀ���Ϊ���ڱȽϵĹ��̷�ֹcheckpoint_snap_min �� oldest_id�����ı�
	*/
	checkpoint_snap_min = txn_global->checkpoint_snap_min;
	oldest_id = txn_global->oldest_id;

	/*
	* If there is no active checkpoint or this handle is up to date with
	* the active checkpoint it's safe to ignore the checkpoint ID in the
	* visibility check.
	*/
	if (checkpoint_snap_min != WT_TXN_NONE
		&& (btree == NULL || btree->checkpoint_gen != txn_global->checkpoint_gen)
		&& TXNID_LT(checkpoint_snap_min, oldest_id))
		oldest_id = checkpoint_snap_min;

	return (TXNID_LT(id, oldest_id));
}

/*�������id�Ƿ�Ե�ǰsession��Ӧ����ɼ�*/
static inline int __wt_txn_visible(WT_SESSION_IMPL* session, uint64_t id)
{
	WT_TXN *txn;
	txn = &session->txn;

	/*
	* Eviction only sees globally visible updates, or if there is a
	* checkpoint transaction running, use its transaction.
	*/
	if (txn->isolation == TXN_ISO_EVICTION)
		return __wt_txn_visible_all(session, id);

	/*id�Ǹ�����������ID���κ����񶼲��ɼ�*/
	if (id == WT_TXN_ABORTED)
		return 0;

	/* Changes with no associated transaction are always visible. */
	if (id == WT_TXN_NONE)
		return 1;

	/*
	* Read-uncommitted transactions see all other changes.
	*
	* All metadata reads are at read-uncommitted isolation.  That's
	* because once a schema-level operation completes, subsequent
	* operations must see the current version of checkpoint metadata, or
	* they may try to read blocks that may have been freed from a file.
	* Metadata updates use non-transactional techniques (such as the
	* schema and metadata locks) to protect access to in-flight updates.
	*/
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED || session->dhandle == session->meta_dhandle)
		return 1;

	/*ID��session��Ӧ�����Լ���ID���ǿɼ���*/
	if (id == txn->id)
		return 1;

	/*
	* TXN_ISO_SNAPSHOT, TXN_ISO_READ_COMMITTED: the ID is visible if it is
	* not the result of a concurrent transaction, that is, if was
	* committed before the snapshot was taken.
	*
	* The order here is important: anything newer than the maximum ID we
	* saw when taking the snapshot should be invisible, even if the
	* snapshot is empty.
	*/
	if (TXNID_LE(txn->snap_max, id))
		return 0;
	if (txn->snapshot_count == 0 || TXNID_LT(id, txn->snap_min))
		return 1;

	return (bsearch(&id, txn->snapshot, txn->snapshot_count, sizeof(uint64_t), __wt_txnid_cmp) == NULL);
}

/*�������������õ����Զ��ύ����ô������ʼ�������*/
static inline int __wt_txn_autocommit_check(WT_SESSION_IMPL* session)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_AUTOCOMMIT)) {
		F_CLR(txn, TXN_AUTOCOMMIT);
		return (__wt_txn_begin(session, NULL));
	}
	return 0;
}

/*���½�һ������ID*/
static inline uint64_t __wt_txn_new_id(WT_SESSION_IMPL* session)
{
	/*
	* We want the global value to lead the allocated values, so that any
	* allocated transaction ID eventually becomes globally visible.  When
	* there are no transactions running, the oldest_id will reach the
	* global current ID, so we want post-increment semantics.  Our atomic
	* add primitive does pre-increment, so adjust the result here.
	*/
	return (WT_ATOMIC_ADD8(S2C(session)->txn_global.current, 1) - 1);
}

/*������Ķ�д֮ǰ���ж�evict cache���ڴ�������ᷢ��evict����*/
static inline int __wt_txn_idle_cache_check(WT_SESSION_IMPL* session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = &(S2C(session))->txn_global.states[session->id];

	/*
	* Check the published snap_min because read-uncommitted never sets
	* TXN_HAS_SNAPSHOT.
	*/
	if (F_ISSET(txn, TXN_RUNNING) && !F_ISSET(txn, TXN_HAS_ID) && txn_state->snap_min == WT_TXN_NONE)
		WT_RET(__wt_cache_full_check(session));

	return 0;
}
/*���cache�ڴ��Ƿ����ˣ�������ˣ�evict lru page,��Ϊtxn����һ��ȫ��Ψһ��ID*/
static inline int __wt_txn_id_check(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;

	WT_ASSERT(session, F_ISSET(txn, TXN_RUNNING));

	/*������ʼǰ�ȼ��cache���ڴ�ռ���ʣ�ȷ��cache���㹻���ڴ���������*/
	WT_RET(__wt_txn_idle_cache_check(session));

	if (!F_ISSET(txn, TXN_HAS_ID)){
		conn = S2C(session);
		txn_global = &conn->txn_global;
		txn_state = &txn_global->states[session->id];

		WT_ASSERT(session, txn_state->id == WT_TXN_NONE);

		/*��������ID�����������cas���ж��߳̾�������ID*/
		do{
			txn_state->id = txn->id = txn_global->current;
		} while (!WT_ATOMIC_CAS8(txn_global->current, txn->id, txn->id + 1));

		if (txn->id == WT_TXN_ABORTED)
			WT_RET_MSG(session, ENOMEM, "Out of transaction IDs");

		F_SET(txn, TXN_HAS_ID);
	}

	return 0;
}

/*���upd��Ԫ�Ƿ������session������ִ��*/
static inline int __wt_txn_update_check(WT_SESSION_IMPL* session, WT_UPDATE* upd)
{
	WT_TXN *txn;
	txn = &session->txn;

	if (txn->isolation == TXN_ISO_SNAPSHOT){
		while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
			if (upd->txnid != WT_TXN_ABORTED) { /*update��session��ǰ���񲻿ɼ�������update��txnid��˵���г�ͻ����upd��rollback*/
				WT_STAT_FAST_DATA_INCR(session, txn_update_conflict);
				return WT_ROLLBACK;
			}
			upd = upd->next;
		}
	}

	return 0;
}

/*
 * __wt_txn_read_last --
 *	Called when the last page for a session is released.
 * �ص���__wt_txn_release_snapshot�������
 */
static inline void __wt_txn_read_last(WT_SESSION_IMPL* session)
{
	WT_TXN *txn;
	txn = &session->txn;

	if (!F_ISSET(txn, TXN_RUNNING) || txn->isolation != TXN_ISO_SNAPSHOT)
		__wt_txn_release_snapshot(session);
}

/*
* __wt_txn_cursor_op --
*	Called for each cursor operation.
*/
static inline void __wt_txn_cursor_op(WT_SESSION_IMPL* session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/*
	* If there is no transaction running (so we don't have an ID), and no
	* snapshot allocated, put an ID in the global table to prevent any
	* update that we are reading from being trimmed to save memory.  Do a
	* read before the write because this shared data is accessed a lot.
	*
	* !!!
	* Note:  We are updating the global table unprotected, so the
	* oldest_id may move past this ID if a scan races with this
	* value being published.  That said, read-uncommitted operations
	* always take the most recent version of a value, so for that version
	* to be freed, two newer versions would have to be committed.	Putting
	* this snap_min ID in the table prevents the oldest ID from moving
	* further forward, so that once a read-uncommitted cursor is
	* positioned on a value, it can't be freed.
	*/
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED && !F_ISSET(txn, TXN_HAS_ID) && TXNID_LT(txn_state->snap_min, txn_global->last_running))
		txn_state->snap_min = txn_global->last_running;

	if (txn->isolation != TXN_ISO_READ_UNCOMMITTED && !F_ISSET(txn, TXN_HAS_SNAPSHOT))
		__wt_txn_refresh(session, 1);
}

/*�ж�sessionִ�е������Ƿ�������ϵͳ�������������ִ�е�����*/
static inline int __wt_txn_am_oldest(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	uint64_t id;
	uint32_t i, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	if (txn->id == WT_TXN_NONE)
		return 0;

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = txn_global->states; i < session_cnt; i++, s++){
		id = s->id;
		if (id != WT_TXN_NONE && TXNID_LT(id, txn->id))
			return 0;
	}

	return 1;
}

















