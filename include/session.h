/***************************************************************
*SESSION�Ķ���
***************************************************************/

/*session��cahce*/
struct __wt_data_handle_cache
{
	WT_DATA_HANDLE* dhandle;

	SLIST_ENTRY(__wt_data_handle_cache) l;
	SLIST_ENTRY(__wt_data_handle_cache) hashl;
};

struct __wt_hazard
{
	WT_PAGE* page;
};

/*���session��Ӧ��connection*/
#define	S2C(session)	  ((WT_CONNECTION_IMPL *)(session)->iface.connection)
/*�ж�session�Ƿ�ΪNULL�������ΪNULL���������Ӧ��connection*/
#define	S2C_SAFE(session) ((session) == NULL ? NULL : S2C(session))

/* Get the btree for a session */
#define	S2BT(session)	   ((WT_BTREE *)(session)->dhandle->handle)
#define	S2BT_SAFE(session) ((session)->dhandle == NULL ? NULL : S2BT(session))

/*����session�ṹ, WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT)*/
struct __wt_session_impl
{
	WT_SESSION				iface;
	void*					lang_private;
	u_int					active;							/*��0��ʾsession���ڱ�ռ��*/
	const char*				name;
	const char*				lastop;

	uint32_t				id;								/*session id,��session array�е�ƫ��*/

	WT_CONDVAR*				cond;
	WT_EVENT_HANDLER*		event_handler;
	WT_DATA_HANDLE*			dhandle;

	/*����һ��__dhandles�Ľṹ��list,�ڲ�list�ĵ�Ԫ__wt_data_handle_cache*/
	SLIST_HEAD(__dhandles, __wt_data_handle_cache) dhandles;
	time_t					last_sweep;
	
	WT_CURSOR*				cursor;
	TAILQ_HEAD(__cursors, __wt_cursor) cursors;

	WT_CURSOR_BACKUP*		bkp_cursor;
	WT_COMPACT*				compact;

	WT_DATA_HANDLE*			meta_handle;
	void*					meta_track;
	void*					meta_track_next;
	void*					meta_track_sub;

#define	WT_META_TRACKING(session)	(session->meta_track_next != NULL)

	SLIST_HEAD(__tables, __wt_table) tables;

	WT_ITEM**				scratch;
	u_int					scratch_alloc;
	size_t					scratch_cached;

	WT_ITEM					err;

	//TODO:WT_TXN_ISOLATION		isolation;
	//WT_TXN					txn;

	u_int					ncursors;

	void*					block_manager;
	int	(*block_manager_cleanup)(WT_SESSION_IMPL *);

	struct{
		WT_DATA_HANDLE*		dhandle;
		const char*			name;
	} *ckpt_handle;
	u_int					ckpt_handle_next;
	size_t					ckpt_handle_allocated;

	void*					reconcile;
	int	(*reconcile_cleanup)(WT_SESSION_IMPL *);

	int						compaction;
	uint32_t				flags;

/*����s->rnd��s��ʵλ�õ�ƫ����*/
#define WT_SESSION_CLEAR_SIZE(s) (WT_PTRDIFF(&(s)->rnd[0], s))
	uint32_t				rnd[2];		/*������Ĳ���״̬*/

	SLIST_HEAD(__dhandles_hash, __wt_data_handle_cache) *dhhash;
	SLIST_HEAD(__tables_hash, __wt_table) *tablehash;

	struct __wt_split_stash
	{
		uint64_t			split_gen;
		void*				p;
		size_t				len;
	} *split_stash;

	size_t					split_stash_cnt;
	size_t					split_stash_alloc;

	uint64_t				split_gen;

#define	WT_SESSION_FIRST_USE(s)		((s)->hazard == NULL)
#define WT_HAZARD_INCR		10

	uint32_t				hazard_size;
	uint32_t				nhazard;
	WT_HAZARD*				hazard;
};
/**************************************************************/
