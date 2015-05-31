/*************************************************************************
*����LOG�Ľṹ(LOG Sequence Number)�ͺ�
*************************************************************************/

#define	WT_LOG_FILENAME	"WiredTigerLog"				/* Log���ļ���*/
#define	WT_LOG_PREPNAME	"WiredTigerPreplog"			/* LogԤ������ļ���*/
#define	WT_LOG_TMPNAME	"WiredTigerTmplog"			/* Log��ʱ�ļ���*/


#define LOG_ALIGN						128
#define WT_LOG_SLOT_BUF_INIT_SIZE		(64 * 1024)

/*��ʼ��LSN*/
#define WT_INIT_LSN(l) do{				\
	(l)->file = 1;						\
	(l)->offset = 0;					\
}while(0)

/*��lsn����Ϊ���ֵ*/
#define WT_MAX_LSN(l) do{				\
	(l)->file = UINT32_MAX;				\
	(l)->offset = UINT64_MAX;			\
}while(0)

/*��lsn���Ϊ0*/
#define	WT_ZERO_LSN(l)	do {			\
	(l)->file = 0;						\
	(l)->offset = 0;					\
}while(0)

/*�ж�LSN�Ƿ�Ϊ��ʼλ��*/
#define	WT_IS_INIT_LSN(l)				((l)->file == 1 && (l)->offset == 0)
/*�ж�LSN�Ƿ�Ϊ���ֵλ��*/
#define	WT_IS_MAX_LSN(l)				((l)->file == UINT32_MAX && (l)->offset == INT64_MAX)

#define	LOGC_KEY_FORMAT					WT_UNCHECKED_STRING(IqI)
#define	LOGC_VALUE_FORMAT				WT_UNCHECKED_STRING(qIIIuu)

/*����WT_LOG_RECORD��ͷƫ��*/
#define	LOG_SKIP_HEADER(data)			((const uint8_t *)(data) + offsetof(WT_LOG_RECORD, record))
/*���LOG_REC�����ݳ���*/
#define	LOG_REC_SIZE(size)				((size) - offsetof(WT_LOG_RECORD, record))

/* LSN֮��Ĵ�С�ȽϺ�
 * lsn1 > lsn2,����1
 * lsn1 == lsn2,����0
 * lsn1 < lsn2,����-1
 */
#define	LOG_CMP(lsn1, lsn2)								\
	((lsn1)->file != (lsn2)->file ?						\
	((lsn1)->file < (lsn2)->file ? -1 : 1) :			\
	((lsn1)->offset != (lsn2)->offset ?					\
	((lsn1)->offset < (lsn2)->offset ? -1 : 1) : 0))

/* LOG SLOT��״̬
 * Possible values for the consolidation array slot states:
 * (NOTE: Any new states must be > WT_LOG_SLOT_DONE and < WT_LOG_SLOT_READY.)
 *
 * < WT_LOG_SLOT_DONE	- threads are actively writing to the log.
 * WT_LOG_SLOT_DONE		- all activity on this slot is complete.
 * WT_LOG_SLOT_FREE		- slot is available for allocation.
 * WT_LOG_SLOT_PENDING	- slot is transitioning from ready to active.
 * WT_LOG_SLOT_WRITTEN	- slot is written and should be processed by worker.
 * WT_LOG_SLOT_READY	- slot is ready for threads to join.
 * > WT_LOG_SLOT_READY	- threads are actively consolidating on this slot.
 */
#define	WT_LOG_SLOT_DONE	0
#define	WT_LOG_SLOT_FREE	1
#define	WT_LOG_SLOT_PENDING	2
#define	WT_LOG_SLOT_WRITTEN	3
#define	WT_LOG_SLOT_READY	4

/*slot flags��ֵ��ʾ*/
#define	SLOT_BUF_GROW	0x01			/* Grow buffer on release */
#define	SLOT_BUFFERED	0x02			/* Buffer writes */
#define	SLOT_CLOSEFH	0x04			/* Close old fh on release */
#define	SLOT_SYNC		0x08			/* Needs sync on release */
#define	SLOT_SYNC_DIR	0x10			/* Directory sync on release */

#define	SLOT_INIT_FLAGS	(SLOT_BUFFERED)

/*�Ƿ���slot indexֵ���൱��-1*/
#define	SLOT_INVALID_INDEX	0xffffffff

#define LOG_FIRST_RECORD	log->allocate

#define	SLOT_ACTIVE			1
#define	SLOT_POOL			16

#define	WT_LOG_FORCE_CONSOLIDATE	0x01	/* Disable direct writes */

#define	WT_LOG_RECORD_COMPRESSED	0x01	/* Compressed except hdr */

typedef WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) struct 
{
	int64_t				slot_state;					/*slot״̬*/
	uint64_t			slot_group_size;			/*slot group size*/
	int32_t				slot_error;					/*slot�Ĵ�����ֵ*/

	uint32_t			slot_index;					/* Active slot index */
	wt_off_t			slot_start_offset;			/* Starting file offset */

	WT_LSN				slot_release_lsn;			/* Slot �ͷŵ�LSNֵ */
	WT_LSN				slot_start_lsn;				/* Slot ��ʼ��LSNֵ */
	WT_LSN				slot_end_lsn;				/* Slot ������LSNֵ */
	WT_FH*				slot_fh;					/* slot��Ӧ���ļ�handler */
	WT_ITEM				slot_buf;					/* Buffer for grouped writes */
	int32_t				slot_churn;					/* Active slots are scarce. */

	uint32_t			flags;						/* slot flags*/	
} WT_LOGSLOT;

/*WT_MYSLOT�ṹ*/
typedef struct 
{
	WT_LOGSLOT*			slot;
	wt_off_t			offset;
} WT_MYSLOT;

/*����WT_LOG�ṹ*/
typedef struct  
{
	uint32_t			allocsize;
	wt_off_t			log_written;

	/*log�ļ���ر���*/
	uint32_t			fileid;
	uint32_t			prep_fileid;
	uint32_t			prep_missed;

	WT_FH*				log_fh;				/*����ʹ�õ�log�ļ�handler*/
	WT_FH*				log_close_fh;		/*��һ�����رյ�log�ļ�handler*/
	WT_FH*				log_dir_fh;			/*logĿ¼�����ļ���handler*/

	/*ϵͳ��LSN����*/
	WT_LSN				alloc_lsn;					/* Next LSN for allocation */
	WT_LSN				ckpt_lsn;					/* Last checkpoint LSN */
	WT_LSN				first_lsn;					/* First LSN */
	WT_LSN				sync_dir_lsn;				/* LSN of the last directory sync */
	WT_LSN				sync_lsn;					/* LSN of the last sync */
	WT_LSN				trunc_lsn;					/* End LSN for recovery truncation */
	WT_LSN				write_lsn;					/* Last LSN written to log file */

	/*log������߳�ͬ��latch*/
	WT_SPINLOCK			log_lock;					/* Locked: Logging fields */
	WT_SPINLOCK			log_slot_lock;				/* Locked: Consolidation array */
	WT_SPINLOCK			log_sync_lock;				/* Locked: Single-thread fsync */

	WT_RWLOCK*			log_archive_lock;			/* Archive and log cursors */

	/* Notify any waiting threads when sync_lsn is updated. */
	WT_CONDVAR*			log_sync_cond;
	/* Notify any waiting threads when write_lsn is updated. */
	WT_CONDVAR*			log_write_cond;

	uint32_t			pool_index;
	WT_LOGSLOT*			slot_array[SLOT_ACTIVE];
	WT_LOGSLOT			slot_pool[SLOT_POOL];

	uint32_t			flags;
} WT_LOG;

/*wt record log�Ķ���*/
typedef struct  
{
	uint32_t			len;
	uint32_t			checksum;

	uint16_t			flags;
	uint8_t				unused[2];
	uint32_t			mem_len;
	uint8_t				record[0];
} WT_RECORD_LOG;

#define	WT_LOG_MAGIC			0x101064
#define	WT_LOG_MAJOR_VERSION	1
#define WT_LOG_MINOR_VERSION	0

struct __wt_log_desc
{
	uint32_t			log_magic;
	uint16_t			majorv;
	uint16_t			minorv;
	uint64_t			log_size;
};

struct __wt_log_rec_desc
{
	const char*			fmt;
	int					(*print)(WT_SESSION_IMPL* session, uint8_t** p, uint8_t* end);
};

struct __wt_log_op_desc
{
	const char*			fmt;
	int					(*print)(WT_SESSION_IMPL* session, uint8_t** p, uint8_t* end);
};




