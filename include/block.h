/**************************************************************************
*����wiredtiger��block(��ռ�)����ӿ�
**************************************************************************/
#define WT_BLOCK_INVALID_OFFSET			0


struct __wt_extlist
{
	char*					name;	

	uint64_t				bytes;					/*�ֽ���*/
	uint32_t				entries;				/*ʵ�����������Կ����Ǽ�¼����*/

	wt_off_t				offset;					/*�������ļ��е���ʼλ��*/
	uint32_t				cksum;					/*extlist���ݵ�checksum*/
	uint32_t				size;					/*extlist��size�ܺ�*/
	int						track_size;				/*skip listά�������ݴ�С*/

	WT_EXT*					last;					/*off���������һ��ext����*/

	WT_EXT*					off[WT_SKIP_MAXDEPTH];	/*ext skip list����*/
	WT_SIZE*				sz[WT_SKIP_MAXDEPTH];	/*size skip list�Ķ���*/
};

/*����ĵ�Ԫ����*/
struct __wt_ext
{
	wt_off_t				off;					/*extent���ļ��ļ���Ӧ��ƫ����*/
	wt_off_t				size;					/*extent�Ĵ�С*/
	uint8_t					depth;					/*skip list�����*/

	WT_EXT*					next[0];				/* Offset, size skiplists */
};

/*һ��block size������Ԫ*/
struct __wt_size
{
	wt_off_t				size;
	uint8_t					depth;
	WT_EXT*					off[WT_SKIP_MAXDEPTH];
	WT_SIZE*				next[WT_SKIP_MAXDEPTH];
};

/*��������Ͳ�ѭ��*/
#define	WT_EXT_FOREACH(skip, head)							\
	for ((skip) = (head)[0];								\
	(skip) != NULL; (skip) = (skip)->next[0])

/*������depth��ѭ��*/
#define	WT_EXT_FOREACH_OFF(skip, head)						\
	for ((skip) = (head)[0];								\
	(skip) != NULL; (skip) = (skip)->next[(skip)->depth])

#define	WT_BM_CHECKPOINT_VERSION	1					/* Checkpoint format version */
#define	WT_BLOCK_EXTLIST_MAGIC		71002				/* Identify a list */


struct __wt_block_ckpt
{
	uint8_t					version;		/*block checkpoint��ϵͳ�汾��*/
	wt_off_t				root_offset;	/*block checkpoint��Ϣ��ʼλ��ƫ��*/
	uint32_t				root_cksum;		/*block checkpoint��Ϣ��checksum*/
	uint32_t				root_size;		/*block checkpoint��Ϣ�Ĵ�С*/

	WT_EXTLIST				alloc;			/* Extents allocated */
	WT_EXTLIST				avail;			/* Extents available */
	WT_EXTLIST				discard;		/* Extents discarded */

	wt_off_t				file_size;		/*checkpoint file size*/
	uint64_t				ckpt_size;		/* Checkpoint byte count*/

	WT_EXTLIST				ckpt_avail;		/* Checkpoint free'd extents */

	WT_EXTLIST				ckpt_alloc;		/* Checkpoint archive */
	WT_EXTLIST				ckpt_discard;	/* Checkpoint archive */
};

/*block manger����*/
struct __wt_bm {
						/* Methods */
	int (*addr_string)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, const uint8_t *, size_t);
	int (*addr_valid)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
	u_int (*block_header)(WT_BM *);
	int (*checkpoint)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, WT_CKPT *, int);
	int (*checkpoint_load)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t, uint8_t *, size_t *, int);
	int (*checkpoint_resolve)(WT_BM *, WT_SESSION_IMPL *);
	int (*checkpoint_unload)(WT_BM *, WT_SESSION_IMPL *);
	int (*close)(WT_BM *, WT_SESSION_IMPL *);
	int (*compact_end)(WT_BM *, WT_SESSION_IMPL *);
	int (*compact_page_skip)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t, int *);
	int (*compact_skip)(WT_BM *, WT_SESSION_IMPL *, int *);
	int (*compact_start)(WT_BM *, WT_SESSION_IMPL *);
	int (*free)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
	int (*preload)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
	int (*read)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, const uint8_t *, size_t);
	int (*salvage_end)(WT_BM *, WT_SESSION_IMPL *);
	int (*salvage_next)(WT_BM *, WT_SESSION_IMPL *, uint8_t *, size_t *, int *);
	int (*salvage_start)(WT_BM *, WT_SESSION_IMPL *);
	int (*salvage_valid)(WT_BM *, WT_SESSION_IMPL *, uint8_t *, size_t, int);
	int (*stat)(WT_BM *, WT_SESSION_IMPL *, WT_DSRC_STATS *stats);
	int (*sync)(WT_BM *, WT_SESSION_IMPL *, int);
	int (*verify_addr)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
	int (*verify_end)(WT_BM *, WT_SESSION_IMPL *);
	int (*verify_start)(WT_BM *, WT_SESSION_IMPL *, WT_CKPT *);
	int (*write) (WT_BM *,WT_SESSION_IMPL *, WT_ITEM *, uint8_t *, size_t *, int);
	int (*write_size)(WT_BM *, WT_SESSION_IMPL *, size_t *);

	WT_BLOCK*	block;			/* Underlying file */

	void*		map;					/* Mapped region */
	size_t		maplen;
	void*		mappingcookie;
	int			is_live;				/* The live system */
};

/*__wt_block�鶨��*/
struct __wt_block
{
	const char*				name;				/*block��Ӧ���ļ���*/
	uint64_t				name_hash;			/*�ļ���hash*/

	uint32_t				ref;				/*�������ü���*/
	WT_FH*					fh;					/*block�ļ���handler*/
	SLIST_ENTRY(__wt_block) l;
	SLIST_ENTRY(__wt_block) hashl;

	uint32_t				allocfirst;			/*���ļ���ʼ������д�ı�ʶ*/		
	uint32_t				allocsize;			/*�ļ�д�����ĳ���*/
	size_t					os_cache;			/*��ǰblock����os page cache�е������ֽ���*/
	size_t					os_cache_max;		/*����ϵͳ���ļ�����page cache���ֽ���*/
	size_t					os_cache_dirty;		/*��ǰ�����ݵ��ֽ���*/
	size_t					os_cache_dirty_max;	/*���������������ֽ���*/

	u_int					block_header;		/*block header�ĳ���*/
	WT_SPINLOCK				live_lock;			/*��live�ı�����*/
	WT_BLOCK_CKPT			live;				/*checkpoint����ϸ��Ϣ*/

	int						ckpt_inprogress;	/*�Ƿ����ڽ���checkpoint*/
	int						compact_pct_tenths;

	wt_off_t				slvg_off;

	int						verify;
	wt_off_t				verify_size;	/* Checkpoint's file size */
	WT_EXTLIST				verify_alloc;	/* Verification allocation list */
	uint64_t				frags;			/* Maximum frags in the file */
	uint8_t*				fragfile;		/* Per-file frag tracking list */
	uint8_t*				fragckpt;		/* Per-checkpoint frag tracking list */
};

#define WT_BLOCK_MAGIC				120897
#define WT_BLOCK_MAJOR_VERSION		1
#define WT_BLOCK_MINOR_VERSION		0

#define	WT_BLOCK_DESC_SIZE			16
/*WT BLOCK���ļ�����*/
struct __wt_block_desc
{
	uint32_t				magic;			/*ħ��У��ֵ*/
	uint16_t				majorv;			/*��汾*/
	uint16_t				minorv;			/*С�汾*/

	uint32_t				cksum;			/*blocks��checksum*/
	uint32_t				unused;			
};

#define WT_BLOCK_DATA_CKSUM			0x01
#define	WT_BLOCK_HEADER_SIZE		12
/*����block header�ṹ*/
struct __wt_block_header
{
	uint32_t				disk_size;		/*�ڴ����ϵ�page��С*/
	uint32_t				cksum;			/*checksum*/
	uint8_t					flags;	
	uint8_t					unused[3];		/*û���ã�������Ϊ��������*/
};

/*��λblock�����ݵĿ�ʼλ��*/
#define	WT_BLOCK_HEADER_BYTE_SIZE					(WT_PAGE_HEADER_SIZE + WT_BLOCK_HEADER_SIZE)
#define	WT_BLOCK_HEADER_BYTE(dsk)					((void *)((uint8_t *)(dsk) + WT_BLOCK_HEADER_BYTE_SIZE))

#define	WT_BLOCK_COMPRESS_SKIP	64

