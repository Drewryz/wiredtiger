
/*�����к�*/
static inline void __cursor_set_recno(WT_CURSOR_BTREE* cbt, uint64_t v)
{
	cbt->iface.recno = v;
	cbt->recno = v;
}

/*����CURSOR_BTREE�ṹ*/
static inline void __cursor_pos_clear(WT_CURSOR_BTREE* cbt)
{
	cbt->recno = 0;

	cbt->ins = NULL;
	cbt->ins_head = NULL;
	cbt->ins_stack[0] = NULL;

	cbt->cip_saved = NULL;
	cbt->rip_saved = NULL;

	/*���flags��WT_CBT_ACTIVEֵ*/
	F_CLR(cbt, ~WT_CBT_ACTIVE);
}

/*����Ƿ���������cursor�����session,����û�У����cache�Ƿ������ģ���������ģ�ֱ�ӷ���һ������*/
static inline int __cursor_enter(WT_SESSION_IMPL *session)
{
	/*
	* If there are no other cursors positioned in the session, check
	* whether the cache is full.
	*/
	if (session->ncursors == 0)
		WT_RET(__wt_cache_full_check(session));
	++session->ncursors;
	return (0);
}

/*��session��Ӧ��cursors��Ϊ��Ч,����û�м���״̬��cursor�����ǽ��ͷ�����Ϊ�˶������snapshot*/
static inline void __cursor_leave(WT_SESSION_IMPL* session)
{
	/*
	 * Decrement the count of active cursors in the session.  When that
	 * goes to zero, there are no active cursors, and we can release any
	 * snapshot we're holding for read committed isolation.
	 */

	WT_ASSERT(session, session->ncursors > 0);
	-- session->ncursors;
	if(session->ncursors == 0){
		__wt_txn_read_last(session);
	}
}

/*����һ��CURSOR_BTREE*/
static inline int __curfile_enter(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_RET(__cursor_enter(session));
	F_SET(cbt, WT_CBT_ACTIVE);

	return 0;
}

/*��cbt���и�λ*/
static inline int __curfile_leave(WT_CURSOR_BTREE* cbt)
{
	WT_SESSION_IMPL* session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	/*���cursor_btree��״̬��active�������������ACTIVE��ʶ*/
	if(F_ISSET(cbt, WT_CBT_ACTIVE)){
		__cursor_leave(session);
		F_CLR(cbt, WT_CBT_ACTIVE);
	}

	/*�����ĳ��page��ɾ��̫��ļ�¼����ô�����ͷ�cursorʱ�᳢��evict���page*/
	if(cbt->ref != NULL && cbt->page_deleted_count > WT_BTREE_DELETE_THRESHOLD)
		__wt_page_evict_soon(cbt->ref->page);

	cbt->page_deleted_count = 0;

	WT_RET(__wt_page_release(session, cbt->ref, 0));
	cbt->ref = NULL;

	return 0;
}

/*��session��dhandle��in-use������ +��������տ�ʼΪ0ʱ�������������������timeofdeath����Ϊ0*/
static inline void __wt_cursor_dhandle_incr_use(WT_SESSION_IMPL* session)
{
	WT_DATA_HANDLE* dhandle;

	dhandle = session->dhandle;

	if(WT_ATOMIC_ADD4(dhandle->session_inuse, 1) == 1 && dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*��session��dhandle��in-use������-1*/
static inline void __wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE *dhandle;

	dhandle = session->dhandle;

	/* If we close a handle with a time of death set, clear it. */
	WT_ASSERT(session, dhandle->session_inuse > 0);
	if (WT_ATOMIC_SUB4(dhandle->session_inuse, 1) == 0 && dhandle->timeofdeath != 0)
		dhandle->timeofdeath = 0;
}

/*��cbt���г�ʼ������������*/
static inline int __cursor_func_init(WT_CURSOR_BTREE* cbt, int reenter)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL*)(cbt->iface.session);
	if(reenter){ /*��cbt���г���*/
		WT_RET(__curfile_leave(cbt));
	}

	/* If the transaction is idle, check that the cache isn't full. */
	WT_RET(__wt_txn_idle_cache_check(session));

	/*����cbt����*/
	if(!F_ISSET(cbt, WT_CBT_ACTIVE)){
		WT_RET(__curfile_enter(cbt));
	}
	/*Ϊһ��readcommited�������snapshot�������ύ�������޸ĵ����ݶԱ�����ɼ�*/
	__wt_txn_cursor_op(session);

	return 0;
}

/*��cbt��������*/
static inline int __cursor_reset(WT_CURSOR_BTREE* cbt)
{
	WT_DECL_RET;
	/*��cbt���г�����������״̬��λ*/
	ret = __curfile_leave(cbt);
	__cursor_pos_clear(cbt);

	return ret;
}

/*����һ����(row)��Ҷ��page��slot��KV��ֵ�ԣ�ֵ�Ǵ���cursor��value��*/
static inline __cursor_row_slot_return(WT_CURSOR_BTREE* cbt, WT_ROW* rip, WT_UPDATE* upd)
{
	WT_BTREE *btree;
	WT_ITEM *kb, *vb;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	void* copy;


	session = (WT_SESSION_IMPL *)(cbt->iface.session);
	btree = S2BT(session);
	page = cbt->ref->page;

	unpack = NULL;

	kb = &(cbt->iface.key);
	vb = &(cbt->iface.value);
	/*���key��ָ��*/
	copy = WT_ROW_KEY_COPY(rip);

	/*
	 * Get a key: we could just call __wt_row_leaf_key, but as a cursor
	 * is running through the tree, we may have additional information
	 * here (we may have the fully-built key that's immediately before
	 * the prefix-compressed key we want, so it's a faster construction).
	 *
	 * First, check for an immediately available key.
	 */
	if(__wt_row_leaf_key_info(page, copy, NULL, &cell, &kb->data, &kb->size))
		goto value;

	if(btree->huffman_key != NULL)
		goto slow;

	unpack = &_unpack;
	__wt_cell_unpack(cell, unpack);
	if(unpack->type == WT_CELL_KEY && cbt->rip_saved != NULL && cbt->rip_saved == rip - 1) {
		WT_ASSERT(session, cbt->tmp.size >= unpack->prefix);

		cbt->tmp.size = unpack->prefix;
		WT_RET(__wt_buf_grow(session, &cbt->tmp, cbt->tmp.size + unpack->size));
		cbt->tmp.size += unpack->size;
	}
	else{
		/*
		 * Call __wt_row_leaf_key_work instead of __wt_row_leaf_key: we
		 * already did __wt_row_leaf_key's fast-path checks inline.
		 */
slow:
		WT_RET(__wt_row_leaf_key_work(session, page, rip, &cbt->tmp, 0));
	}

	kb->data = cbt->tmp.data;
	kb->size = cbt->tmp.size;
	cbt->rip_saved = rip;

value:
	if(upd != NULL){
		vb->data = WT_UPDATE_DATA(upd);
		vb->size = upd->size;
		return 0;
	}

	if(__wt_row_leaf_value(page, rip, vb))
		return 0;

	if ((cell = __wt_row_leaf_value_cell(page, rip, unpack)) == NULL) {
		vb->data = "";
		vb->size = 0;
		return (0);
	}

	unpack = &_unpack;
	__wt_cell_unpack(cell, unpack);

	return __wt_page_cell_data_ref(session, cbt->ref->page, unpack, vb);
}







