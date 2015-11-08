/***********************************************************************
*BTREE�����ݽṹ����
***********************************************************************/

/*btree�İ汾��֧�ַ�Χ*/
#define	WT_BTREE_MAJOR_VERSION_MIN	1	/* Oldest version supported */
#define	WT_BTREE_MINOR_VERSION_MIN	1

#define	WT_BTREE_MAJOR_VERSION_MAX	1	/* Newest version supported */
#define	WT_BTREE_MINOR_VERSION_MAX	

/*btree�Ľڵ����ռ��С512M������˵��1M����������*/
#define WT_BTREE_PAGE_SIZE_MAX		(512 * WT_MEGABYTE)

/*��ʽ�洢ʱ�������Ŀռ�ռ�ô�С��4G - 1KB*/
#define	WT_BTREE_MAX_OBJECT_SIZE	(UINT32_MAX - 1024)

#define	WT_BTREE_MAX_ADDR_COOKIE	255	/* Maximum address cookie */

/*ҳ���������������ҳ����1000�����ϲ������ɾ����¼����ô��Ҫ��ҳ�����飿*/
#define WT_BTREE_DELETE_THRESHOLD	1000

#define	WT_SPLIT_DEEPEN_MIN_CHILD_DEF	10000

#define	WT_SPLIT_DEEPEN_PER_CHILD_DEF	100

/* Flags values up to 0xff are reserved for WT_DHANDLE_* */
#define	WT_BTREE_BULK			0x00100	/* Bulk-load handle */
#define	WT_BTREE_NO_EVICTION	0x00200	/* Disable eviction */
#define	WT_BTREE_NO_HAZARD		0x00400	/* Disable hazard pointers */
#define	WT_BTREE_SALVAGE		0x00800	/* Handle is for salvage */
#define	WT_BTREE_UPGRADE		0x01000	/* Handle is for upgrade */
#define	WT_BTREE_VERIFY			0x02000	/* Handle is for verify */

#define WT_BTREE_SPECIAL_FLAGS	(WT_BTREE_BULK | WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)

/*btree�ṹ*/
struct __wt_btree
{
	WT_DATA_HANDLE*			dhandle;

	WT_CKPT*				ckpt;				/*checkpoint��Ϣ�ṹָ��*/

	enum{
		BTREE_COL_FIX = 1,						/*��ʽ�����洢*/
		BTREE_COL_VAR = 2,						/*��ʽ�߳��洢*/
		BTREE_ROW	  = 3,						/*��ʽ�洢*/
	} type;

	const char*				key_format;			/*key��ʽ��*/
	const char*				value_format;		/*value��ʽ��*/

	uint8_t					bitcnt;				/*����field�ĳ���*/
	WT_COLLATOR*			collator;			/*�д洢ʱ�ıȽ���*/
	int						collator_owned;		/*������ֵΪ1����ʾ�Ƚ�����Ҫ����free*/

	uint32_t				id;					/*log file ID*/

	uint32_t				key_gap;			/*�д洢ʱ��keyǰ׺��Χ����*/

	uint32_t				allocsize;			/* Allocation size */
	uint32_t				maxintlpage;		/* Internal page max size */
	uint32_t				maxintlkey;			/* Internal page max key size */
	uint32_t				maxleafpage;		/* Leaf page max size */
	uint32_t				maxleafkey;			/* Leaf page max key size */
	uint32_t				maxleafvalue;		/* Leaf page max value size */
	uint64_t				maxmempage;			/* In memory page max size */

	void*					huffman_key;		/*keyֵ�Ļ���������*/
	void*					huffman_value;		/*valueֵ�Ļ���������*/

	enum{
		CKSUM_ON			= 1,		
		CKSUM_OFF			= 2,
		CKSUM_UNCOMPRESSED	= 3,
	} checksum;									/*checksum����*/

	u_int					dictionary;			/*slots�ֵ�*/
	int						internal_key_truncate;
	int						maximum_depth;		/*����������*/
	int						prefix_compression; /*ǰ׺ѹ������*/
	u_int					prefix_compression_min;

	u_int					split_deepen_min_child;/*ҳsplitʱ���ٵ�entry����*/
	u_int					split_deepen_per_child;/*ҳslpitʱbtree�����ӵ�ƽ��entry����*/
	int						split_pct;
	WT_COMPRESSOR*			compressor;			/*ҳ����ѹ����*/

	WT_RWLOCK*				ovfl_lock;

	uint64_t				last_recno;			/*��ʽ�洢ʱ���ļ�¼���*/
	WT_REF					root;				/*btree root�ĸ��ڵ���*/
	int						modified;			/*btree�޸ı�ʾ*/
	int						bulk_load_ok;		/*�Ƿ�����btreeռ�ÿռ�����*/

	WT_BM*					bm;					/*block manager���*/
	u_int					block_header;		/*blockͷ���ȣ�=WT_PAGE_HEADER_BYTE_SIZE*/

	uint64_t				checkpoint_gen;
	uint64_t				rec_max_txn;		/*���ɼ�����ID*/
	uint64_t				write_gen;

	WT_REF*					evict_ref;			
	uint64_t				evict_priority;		/*ҳ���ڴ�����̭�����ȼ�*/
	u_int					evict_walk_period;	/* Skip this many LRU walks */
	u_int					evict_walk_skips;	/* Number of walks skipped */
	volatile uint32_t		evict_busy;			/* Count of threads in eviction */

	int						checkpointing;		/*�Ƿ�����checkpoint*/

	WT_SPINLOCK				flush_lock;

	uint32_t				flags;
};

/*���ݻָ���cookie�ṹ*/
struct __wt_salvage_cookie
{
	uint64_t				missing;
	uint64_t				skip;
	uint64_t				take;
	int						done;
};

/**********************************************************************/

