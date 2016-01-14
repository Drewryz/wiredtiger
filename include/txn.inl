
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









