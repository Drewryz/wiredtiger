/**************************************************************************
*����os���һЩ�����ͺ�
**************************************************************************/

typedef enum 
{
	WT_FHANDLE_APPEND,
	WT_FHANDLE_READ, 
	WT_FHANDLE_WRITE
} WT_FHANDLE_MODE;

#define	WT_FOPEN_APPEND		"ab"
#define	WT_FOPEN_READ		"rb"
#define	WT_FOPEN_WRITE		"wb"

#define WT_FOPEN_FIXED		0x1

#define WT_DIR_ENTRY		32

#define	WT_DIRLIST_EXCLUDE	0x1		/* Exclude files matching prefix */
#define	WT_DIRLIST_INCLUDE	0x2		/* Include files matching prefix */


/*���Ե���ϵͳ���ã����ʧ�ܣ��������10��,�������ܻ�����*/
#define WT_SYSCALL_RETRY(call, ret) do {		\
	int __retry;								\
	for(__retry = 0; __retry < 10; ++__retry){	\
		if((call) == 0){						\
			(ret) = 0;							\
			break;								\
		}										\
		switch((ret) == __wt_errno()){			\
		case 0:									\
			(ret) = WT_ERROR;					\
			break;								\
		case EAGAIN:							\
		case EBUSY:								\
		case EINTR:								\
		case EIO:								\
		case EMFILE:							\
		case ENFILE:							\
		case ENOSPC:							\
			__wt_sleep(0L, 500000L);			\
			continue;							\
		default:								\
			break;								\
		}										\
		break;									\
	}											\
}while(0)					

/*����ʱ����λΪ΢��*/
#define	WT_TIMEDIFF(end, begin)									\
	(1000000000 * (uint64_t)((end).tv_sec - (begin).tv_sec) +	\
	(uint64_t)(end).tv_nsec - (uint64_t)(begin).tv_nsec)

#define	WT_TIMECMP(t1, t2)						\
	((t1).tv_sec < (t2).tv_sec ? -1 :			\
	(t1).tv_sec == (t2.tv_sec) ?				\
	(t1).tv_nsec < (t2).tv_nsec ? -1 :			\
	(t1).tv_nsec == (t2).tv_nsec ? 0 : 1 : 1)


enum __wt_file_extend
{
	WT_FALLOCATE_AVAILABLE,
	WT_FALLOCATE_NOT_AVAILABLE,
	WT_FALLOCATE_POSIX,
	WT_FALLOCATE_STD,
	WT_FALLOCATE_SYS 
};

struct __wt_fh
{
	char*			name;
	uint64_t		name_hash;
	SLIST_ENTRY(__wt_fh) l;
	SLIST_ENTRY(__wt_fh) hashl;

	u_int			ref;

	wt_off_t		size;	
	wt_off_t		extend_size;
	wt_off_t		extend_len;

	int				fd;

	int				diect_io;
	__wt_file_extend fallocate_available;
	int				fallocate_requires_locking;
};

