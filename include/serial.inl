
/*Confirm the page's write generation number won't wrap*/
static inline int __page_write_gen_wrapped_check(WT_PAGE* page)
{
	return (page->modify->write_gen > UINT32_MAX - WT_MILLION ? WT_RESTART : 0);
}

/*��ָ��skip list������һ��WT_INSERT entry*/
static inline int __insert_serial_func(WT_SESSION_IMPL *session, WT_INSERT_HEAD *ins_head,
	WT_INSERT ***ins_stack, WT_INSERT *new_ins, u_int skipdepth)
{
	u_int i;
	WT_UNUSED(session);

	/*���������飬ȷ��λ���Ƿ���skip list�е�λ��*/
	for (i = 0; i < skipdepth; i++){
		if(ins_stack[i] == NULL || (*ins_stack[i] != new_ins->next[i]))
			return (WT_RESTART);
		if (new_ins->next[i] == NULL && ins_head->tail[i] != NULL && ins_stack[i] != &ins_head->tail[i]->next[i])
			return (WT_RESTART);
	}

	/*��new ins���뵽ins_head skip list*/
	for (i = 0; i < skipdepth; i++){
		if (ins_head->tail[i] == NULL || ins_stack[i] == &ins_head->tail[i]->next[i])
			ins_head->tail[i] = new_ins;
		*ins_stack[i] = new_ins;
	}

	return 0;
}

/*Ϊcolumn store׷��һ��WT_INSERTֵ����׷��֮ǰ�ж�recno�Ƿ�����ˣ����û�з��䣬����һ��,����������Ǳ�lockס��*/
static inline int __col_append_serial_func(WT_SESSION_IMPL *session, WT_INSERT_HEAD *ins_head,
	WT_INSERT ***ins_stack, WT_INSERT *new_ins, uint64_t *recnop, u_int skipdepth)
{
	WT_BTREE* btree;
	uint64_t recno;
	u_int i;

	btree = S2BT(session);

	/*��ò����¼��recno��ţ�������û�з��䣬Ϊ�����һ�����,��׷�ӵ�skiplist�ĺ���*/
	recno = WT_INSERT_RECNO(new_ins);
	if(recno == 0){
		recno = WT_INSERT_RECNO(new_ins) = btree->last_recno + 1;
		WT_ASSERT(session, WT_SKIP_LAST(ins_head) == NULL || recno > WT_INSERT_RECNO(WT_SKIP_LAST(ins_head)));
		for (i = 0; i < skipdepth; i++)
			ins_stack[i] = ins_head->tail[i] == NULL ? &ins_head->head[i] : &ins_head->tail[i]->next[i];
	}

	WT_RET(__insert_serial_func(session, ins_head, ins_stack, new_ins, skipdepth));

	*recnop = recno;
	if(recno > btree->last_recno) /*�������ϵ�last recno*/
		btree->last_recno = recno;

	return 0;
}

/*���е�����һ��WT_INSERT entry��btree�ϣ���������Ǳ�lockס��*/
static inline int __wt_insert_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
	WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **new_insp,
	size_t new_ins_size, u_int skipdepth)
{
	WT_INSERT *new_ins = *new_insp;
	WT_DECL_RET;

	/* Clear references to memory we now own. */
	*new_insp = NULL;

	/* Check for page write generation wrap. */
	WT_RET(__page_write_gen_wrapped_check(page));

	/* Acquire the page's spinlock, call the worker function. */
	WT_PAGE_LOCK(session, page);
	ret = __insert_serial_func(session, ins_head, ins_stack, new_ins, skipdepth);
	WT_PAGE_UNLOCK(session, page);

	if(ret != 0){
		__wt_free(session, new_ins);
		return ret;
	}

	/*
	 * Increment in-memory footprint after releasing the mutex: that's safe
	 * because the structures we added cannot be discarded while visible to
	 * any running transaction, and we're a running transaction, which means
	 * there can be no corresponding delete until we complete.
	 */
	__wt_cache_page_inmem_incr(session, page, new_ins_size);

	/*���ó���page*/
	__wt_page_modify_set(session, page);

	return 0;
}

/*���и���*/
static inline int __wt_update_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
	WT_UPDATE **srch_upd, WT_UPDATE **updp, size_t upd_size)
{
	WT_DECL_RET;
	WT_UPDATE *obsolete, *upd = *updp;

	*updp = NULL;

	/* Check for page write generation wrap. */
	WT_RET(__page_write_gen_wrapped_check(page));
	/*
	 * Swap the update into place.  If that fails, a new update was added
	 * after our search, we raced.  Check if our update is still permitted,
	 * and if it is, do a full-barrier to ensure the update's next pointer
	 * is set before we update the linked list and try again.
	 * �����WT_WRITE_BARRIER������ÿ�λ�ȡupd->next��Ӧ�����µ�
	 */
	while(!WT_ATOMIC_CAS8(*srch_upd, upd->next, upd)){
		if ((ret = __wt_txn_update_check(session, upd->next = *srch_upd)) != 0) {
			/* Free unused memory on error. */
			__wt_free(session, upd);
			return (ret);
		}

		WT_WRITE_BARRIER();
	}

	/*�����ڴ�ռ��ͳ�ƣ�footprint memsize*/
	__wt_cache_page_inmem_incr(session, page, upd_size);

	/* Mark the page dirty after updating the footprint. */
	__wt_page_modify_set(session, page);

	if(upd->next != NULL){
		F_CAS_ATOMIC(page, WT_PAGE_SCANNING, ret);
		if(ret != 0)
			return 0;
		/*�����ڵ�update list,�ȷ��������ù��ڵ�upd��Ԫ*/
		obsolete = __wt_update_obsolete_check(session, upd->next);
		F_CLR_ATOMIC(page, WT_PAGE_SCANNING);
		if(obsolete != NULL)
			__wt_update_obsolete_free(session, page, obsolete);
	}

	return 0;
}




