/*******************************************************************
*���밲ȫ����,��Ҫ���ڱ����ڼ���ʾ����ASSERT����ֹ����ʱ����
*******************************************************************/

#undef ALIGN_CHECK
#undef SIZE_CHECK

/*���cond == false,����������ʾ����*/
#define	WT_STATIC_ASSERT(cond)	(void)sizeof(char[1 - 2 * !(cond)])

/*sizeof(type) != eʱ������������ʾ����*/
#define SIZE_CHECK(type, e) do{ \
	char __check_##type[1 - 2 * !(sizeof(type) == (e))];		\
	(void)__check##type;	\
}while(0)

/*������type����2����������*/
#define ALIGN_CHECK(type, a) WT_STATIC_ASSERT(WT_ALIGN(sizeof(type), (a)) == sizeof(type))

static inline void __wt_verify_build(void)
{
	/*���ָ���Ľṹ�����*/
	//SIZE_CHECK(WT_BLOCK_DESC, WT_BLOCK_DESC_SIZE);
	//SIZE_CHECK(WT_REF, WT_REF_SIZE);

	/*64Ϊ����ϵͳ���,��֧������λ�Ĳ���ϵͳ*/
	WT_STATIC_ASSERT(sizeof(size_t) >= 8);
	WT_STATIC_ASSERT(sizeof(wt_off_t) == 8);
}

#undef ALIGN_CHECK
#undef SIZE_CHECK

/******************************************************************/



