/********************************************************************
*ԭ�Ӳ����Ļ���ȴ������
********************************************************************/

/*��v���ó�val֮ǰ�����е�CPU STORES�����������*/
#define WT_PUBLISH(v, val) do{ \
	WT_WRITE_BARRIER(); \
	(v) = (val); \
}while(0)

/*��v=val֮�󣬱�֤�������µ�valֵ*/
#define WT_ORDERED_READ(v, val) do{ \
	(v) = (val); \
	WT_READ_BARRIER(); \
}while(0)

/*��p��flags����maskλ���ˣ����õ����˺��ֵ*/
#define	F_ISSET_ATOMIC(p, mask)	((p)->flags_atomic & (uint8_t)(mask))

/*��p��flags����maskλ���ã����flags��load��store֮�䷢���˸ı䣬CAS1���ٴ�load,ֱ��CAS1�ɹ�Ϊֹ,Ӧ���ڶ��߳̾���״̬*/
#define	F_SET_ATOMIC(p, mask) do {																\
	uint8_t __orig;																				\
	do {																						\
	__orig = (p)->flags_atomic;																	\
	}while(!WT_ATOMIC_CAS1((p)->flags_atomic, __orig, __orig | (uint8_t)(mask)));				\
} while (0)

/*��p��flags���н���mask���ã����flags��maskλ�б��������������ˣ�ֱ�ӽ�ret���ó�EBUSY״̬��һ��Ӧ���ڶ��߳̾���״̬*/
#define F_CAS_ATOMIC(p, mask, ret) do{															\
	uint8_t __orig;																				\
	ret = 0;																					\
	do{																							\
		__orig = (p)->flags_atomic;																\
		if((__orig & (uint8_t)(mask)) != 0){													\
			ret = EBUSY;																		\
			break;																				\
		}																						\
	}while(!WT_ATOMIC_CAS1((p)->flags_atomic, __orig, __orig | (uint8_t)(mask)));				\
}while(0)

/*ԭ����ȡ��p��flags��maskλ,һ�����ڶ��߳̾���״̬*/
#define	F_CLR_ATOMIC(p, mask)	do {															\
	uint8_t __orig;																				\
	do {																						\
		__orig = (p)->flags_atomic;																\
	} while (!WT_ATOMIC_CAS1((p)->flags_atomic,	__orig, __orig & ~(uint8_t)(mask)));			\
} while (0)

/* Cache line alignment, CPU L1 cache�е�λ*/
#define	WT_CACHE_LINE_ALIGNMENT	64	

/********************************************************************/
