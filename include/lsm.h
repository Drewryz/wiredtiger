/**************************************************************************
*LSM�������ݽṹ����
**************************************************************************/

struct __wt_lsm_worker_cookie
{
	WT_LSM_CHUNK**		chunk_array;		/*һ��wt_lsm_chunk������*/
	size_t				chunk_alloc;		/*chunk array�ռ䳤��*/
	u_int				nchunks;			/*chunk array����Ч��Ԫ�ĸ���*/
};

#define	WT_LSM_WORKER_RUN	0x01

/*LSM�߳���Ĳ�������*/
struct __wt_lsm_worker_args
{
	WT_SESSION_IMPL	*session;		/* Session */
	WT_CONDVAR*	work_cond;			/* Owned by the manager */
	wt_thread_t	tid;				/* Thread id */
	u_int		id;					/* My manager slot id */
	uint32_t	type;				/* Types of operations handled */
	uint32_t	flags;				/* workers flags */
};

/*LSM TREE�ı�ʶ����*/
#define	WT_CLSM_ACTIVE			0x01    /* Incremented the session count */
#define	WT_CLSM_ITERATE_NEXT    0x02    /* Forward iteration */
#define	WT_CLSM_ITERATE_PREV    0x04    /* Backward iteration */
#define	WT_CLSM_MERGE           0x08    /* Merge cursor, don't update */
#define	WT_CLSM_MINOR_MERGE		0x10    /* Minor merge, include tombstones */
#define	WT_CLSM_MULTIPLE        0x20    /* Multiple cursors have values for the current key */
#define	WT_CLSM_OPEN_READ		0x40    /* Open for reads */
#define	WT_CLSM_OPEN_SNAPSHOT	0x80    /* Open for snapshot isolation */

/*LSM TREE cursor�ṹ����*/
struct __wt_cursor_lsm
{
	WT_CURSOR		iface;

	WT_LSM_TREE*	lsm_tree;
	uint64_t		dsk_gen;

	u_int			nchunks;					/* Number of chunks in the cursor */
	u_int			nupdates;					/* Updates needed (including snapshot isolation checks). */
	WT_BLOOM**		blooms;						/* Bloom filter handles. bloom filter���������� */
	size_t			bloom_alloc;				/* bloom array����Ŀռ䳤��*/

	WT_CURSOR**		cursors;					/* Cursor handles. */
	size_t			cursor_alloc;

	WT_CURSOR*		current;     				/* The current cursor for iteration */
	WT_LSM_CHUNK*	primary_chunk;				/* The current primary chunk */

	uint64_t*		switch_txn;					/* Switch txn for each chunk */
	size_t			txnid_alloc;

	u_int			update_count;				/* Updates performed. */

	uint32_t		flags;
};

/*LSM tree chunk ��ʶ��ֵ����*/
#define	WT_LSM_CHUNK_BLOOM		0x01
#define	WT_LSM_CHUNK_MERGING	0x02
#define	WT_LSM_CHUNK_ONDISK		0x04
#define	WT_LSM_CHUNK_STABLE		0x08

/*LSM CHUNK�ṹ����*/
struct  __wt_lsm_chunk
{
	const char*		uri;						/* data source��uri */
	const char*		bloom_uri;					/* ��Ӧ��bloom filter */
	struct timespec create_ts;					/* ����ʱ��� */
	uint64_t		count;						/* ��chunk�洢�ļ�¼���� */		
	uint64_t		size;						/* chunk�Ŀռ��С */
	
	uint64_t		switch_txn;

	uint32_t		id;							/* ID used to generate URIs */
	uint32_t		generation;					/* Merge generation */
	uint32_t		refcnt;						/* Number of worker thread references */
	uint32_t		bloom_busy;					/* Number of worker thread references */

	int8_t			empty;						/* 1/0: checkpoint missing */
	int8_t			evicted;					/* 1/0: in-memory chunk was evicted */

	uint32_t		flags;
};

/*LSM�Ĳ�����ʶ*/
#define	WT_LSM_WORK_BLOOM	0x01				/* Create a bloom filter */
#define	WT_LSM_WORK_DROP	0x02				/* Drop unused chunks */
#define	WT_LSM_WORK_FLUSH	0x04				/* Flush a chunk to disk */
#define	WT_LSM_WORK_MERGE	0x08				/* Look for a tree merge */
#define	WT_LSM_WORK_SWITCH	0x10				/* Switch to new in-memory chunk */

#define	WT_LSM_WORK_FORCE	0x0001				/* Force operation */

/*lsm�Ĳ�����Ԫ����LSM �и���Ӧ�Ĳ�����Ԫ�б�*/
struct __wt_lsm_work_unit {
	TAILQ_ENTRY(__wt_lsm_work_unit) q;			/* Worker unit queue */
	uint32_t	type;							/* Type of operation */
	uint32_t	flags;							/* Flags for operation */
	WT_LSM_TREE* lsm_tree;
};

#define	WT_LSM_MAX_WORKERS	20
#define	WT_LSM_MIN_WORKERS	3

/*LSM Tree Manager����*/
struct __wt_lsm_manager
{
	/*
	 * Queues of work units for LSM worker threads. We maintain three
	 * queues, to allow us to keep each queue FIFO, rather than needing
	 * to manage the order of work by shuffling the queue order.
	 * One queue for switches - since switches should never wait for other
	 *   work to be done.
	 * One queue for application requested work. For example flushing
	 *   and creating bloom filters.
	 * One queue that is for longer running operations such as merges.
	 */

	TAILQ_HEAD(__wt_lsm_work_switch_qh, __wt_lsm_work_unit)		switchqh;			/*�����Ĳ�������*/
	TAILQ_HEAD(__wt_lsm_work_app_qh, __wt_lsm_work_unit)		appqh;				/*�ϲ�Ӧ�õĲ������У���Ҫ��chunk flush,����bloom filter��*/
	TAILQ_HEAD(__wt_lsm_work_manager_qh, __wt_lsm_work_unit)	managerqh;			/*��ʱ���������磺merges chunk*/

	WT_SPINLOCK		switch_lock;	/* Lock for switch queue */
	WT_SPINLOCK		app_lock;		/* Lock for application queue */
	WT_SPINLOCK		manager_lock;	/* Lock for manager queue */
	WT_CONDVAR*		work_cond;		/* Used to notify worker of activity */

	uint32_t		lsm_workers;	/* Current number of LSM workers */
	uint32_t		lsm_workers_max;/* LSM��������workers��Ŀ*/

	WT_LSM_WORKER_ARGS lsm_worker_cookies[WT_LSM_MAX_WORKERS];
};

#define WT_LSM_AGGRESSIVE_THRESHOLD		5

#define	LSM_TREE_MAX_QUEUE				100

/*bloom filter�Ĵ�������*/
#define	WT_LSM_BLOOM_MERGED				0x00000001
#define	WT_LSM_BLOOM_OFF				0x00000002
#define	WT_LSM_BLOOM_OLDEST				0x00000004

/*lsm tree ��ʶֵ����*/
#define	WT_LSM_TREE_ACTIVE				0x01	/* Workers are active */
#define	WT_LSM_TREE_COMPACTING			0x02	/* Tree being compacted */
#define	WT_LSM_TREE_MERGES				0x04	/* Tree should run merges */
#define	WT_LSM_TREE_NEED_SWITCH			0x08	/* New chunk needs creating */
#define	WT_LSM_TREE_OPEN				0x10	/* The tree is open */
#define	WT_LSM_TREE_THROTTLE			0x20	/* Throttle updates */

/* lsm tree�ṹ���� */
struct __wt_lsm_tree
{
	const char *name, *config, *filename;				/*�����ַ������ļ����Լ�LSM TREE����*/
	const char *key_format, *value_format;				/*KV��ʽ�����ı�ʶ��*/
	const char *bloom_config, *file_config;				/*bloom filter���������õ�*/

	/*�Ƚ����ӿ�*/
	WT_COLLATOR*	collator;
	const char*		collator_name;
	int				collator_owned;

	int				refcnt;								/*�������ü���*/
	int8_t			exclusive;							/*������ʶ����lock���ʹ�ã�*/

	int				queue_ref;
	WT_RWLOCK*		rwlock;

	TAILQ_ENTRY(__wt_lsm_tree) q;

	WT_DSRC_STATS	stats;								/*LSMͳ����*/
	uint64_t		dsk_gen;

	uint64_t		ckpt_throttle;						/* Rate limiting due to checkpoints */
	uint64_t		merge_throttle;						/* Rate limiting due to merges */
	uint64_t		chunk_fill_ms;						/* Estimate of time to fill a chunk */
	struct timespec last_flush_ts;						/* Timestamp last flush finished */
	struct timespec work_push_ts;						/* Timestamp last work unit added */
	uint64_t		merge_progressing;					/* Bumped when merges are active */
	uint32_t		merge_syncing;						/* Bumped when merges are syncing */

	/* һЩ��������� */
	uint32_t		bloom_bit_count;
	uint32_t		bloom_hash_count;
	uint32_t		chunk_count_limit;					/* Limit number of chunks */
	uint64_t		chunk_size;
	uint64_t		chunk_max;							/* Maximum chunk a merge creates */
	u_int			merge_min, merge_max;				/* ���ٶ��ٸ�chunkһ��ϲ��������ٸ�*/

	u_int			merge_idle;							/* Count of idle merge threads */

	uint32_t		bloom;

	WT_LSM_CHUNK**	chunk;
	size_t			chunk_alloc;
	uint32_t		nchunks;
	uint32_t		last;
	int				modified;

	WT_LSM_CHUNK**	old_chunks;					/* Array of old LSM chunks */
	size_t			old_alloc;					/* Space allocated for old chunks */
	u_int			nold_chunks;				/* Number of old chunks */
	int				freeing_old_chunks;			/* Whether chunks are being freed */
	uint32_t		merge_aggressiveness;		/* Increase amount of work per merge */

	uint32_t		flags;
};

/*LSM tree������Դ�����ӿ�*/
struct __wt_lsm_data_source
{
	WT_DATA_SOURCE	iface;
	WT_RWLOCK*		rwlock;
};

