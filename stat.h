/*************************************************************************
*wiredtiger��״̬��Ϣͳ��
**************************************************************************/

struct __wt_stats
{
	const char* *desc;					/*ͳ��ֵ����*/
	uint64_t	v;						/*ͳ��ֵ*/
};



#define WT_STAT(stats, fld)						((stats)->fld.v)

/*ԭ���Լ�*/
#define WT_STAT_ATOMIC_DECRV(stats, fld, value) do{					\
	(void)(WT_ATOMIC_SUB8(WT_STAT(stats, fld), (value)));			\
}while(0)
#define WT_STAT_ATOMIC_DECR(stats, fld) WT_STAT_ATOMIC_DECRV(stats, fld, 1)

/*ԭ���Լ�*/
#define WT_STAT_ATOMIC_INCRV(stats, fld, value) do{					\
	(void)WT_ATOMIC_ADD8(WT_STAT(stats, fld), (value));				\
} while(0)
#define	WT_STAT_ATOMIC_INCR(stats, fld) WT_ATOMIC_ADD(WT_STAT(stats, fld), 1)

/*����û�о�������*/
#define	WT_STAT_DECRV(stats, fld, value) do {						\
	(stats)->fld.v -= (value);										\
} while (0)
#define	WT_STAT_DECR(stats, fld) WT_STAT_DECRV(stats, fld, 1)

/*�ӣ�û�о�������*/
#define	WT_STAT_INCRV(stats, fld, value) do {						\
	(stats)->fld.v += (value);										\
} while (0)
#define	WT_STAT_INCR(stats, fld) WT_STAT_INCRV(stats, fld, 1)

#define	WT_STAT_SET(stats, fld, value) do {							\
	(stats)->fld.v = (uint64_t)(value);								\
} while (0)



