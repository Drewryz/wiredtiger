/***********************************************************************************************
����GCC�����������Ժ����ͺ�
***********************************************************************************************/

#define WT_SIZET_FMT		"zu"

/*����gccһ���ֽڶ�������*/
#define WT_COMPILER_TYPE_ALIGN(x)			__attribute__((align(x)))

/*����һ��__packed__�����struct���,��ʵ���ǲ�����*/
#define WT_PACKED_STRUCT_BEGIN(name)		struct __attribute__ ((__packed__)) name {
#define WT_PACKED_STRUCT_END				}

#define WT_GCC_FUNC_ATTRIBUTE(x)
#define WT_GCC_FUNC_DECL_ATTRIBUTE(x) __attribute__(x)

/*ԭ�Ӳ�����װ, WT_STATIC_ASSERTֻ���������������õ�*/

#define __WT_ATOMIC_ADD(v, val, n)			(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_add_and_fetch(&(v), val))

#define __WT_ATOMIC_FETCH_ADD(v, val, n)	(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_fetch_and_add(&(v), val))

/*���v == old,��v��ֵΪnew,�������һ��ֵ��һ��bool���͵�ֵ,��ֵ�ɹ�����һ��TRUEֵ*/
#define __WT_ATOMIC_CAS(v, old, new, n)		(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_val_compare_and_swap(&(v), old, new) == (old))
/*���v == old,��v��ֵΪnew*/
#define	__WT_ATOMIC_CAS_VAL(v, old, new, n)	(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_val_compare_and_swap(&(v), old, new))
/*��val��ֵ��v,������v����֮ǰ��ֵ��*/
#define __WT_ATOMIC_STORE(v, val, n)		(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_lock_test_and_set(&(v), val))

#define __WT_ATOMIC_SUB(v, val, n)			(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_sub_and_fetch(&(v), val))

/*�Ը�����������ԭ�Ӳ�����*/
/*int8_t*/
#define	WT_ATOMIC_ADD1(v, val)				__WT_ATOMIC_ADD(v, val, 1)
#define	WT_ATOMIC_FETCH_ADD1(v, val)		__WT_ATOMIC_FETCH_ADD(v, val, 1)
#define	WT_ATOMIC_CAS1(v, old, new)			__WT_ATOMIC_CAS(v, old, new, 1)
#define	WT_ATOMIC_CAS_VAL1(v, old, new)		__WT_ATOMIC_CAS_VAL(v, old, new, 1)
#define	WT_ATOMIC_STORE1(v, val)			__WT_ATOMIC_STORE(v, val, 1)
#define	WT_ATOMIC_SUB1(v, val)				__WT_ATOMIC_SUB(v, val, 1)

/*int16_t*/
#define	WT_ATOMIC_ADD2(v, val)				__WT_ATOMIC_ADD(v, val, 2)
#define	WT_ATOMIC_FETCH_ADD2(v, val)		__WT_ATOMIC_FETCH_ADD(v, val, 2)
#define	WT_ATOMIC_CAS2(v, old, new)			__WT_ATOMIC_CAS(v, old, new, 2)
#define	WT_ATOMIC_CAS_VAL2(v, old, new)		__WT_ATOMIC_CAS_VAL(v, old, new, 2)
#define	WT_ATOMIC_STORE2(v, val)			__WT_ATOMIC_STORE(v, val, 2)
#define	WT_ATOMIC_SUB2(v, val)				__WT_ATOMIC_SUB(v, val, 2)

/*int32_t*/
#define	WT_ATOMIC_ADD4(v, val)				__WT_ATOMIC_ADD(v, val, 4)
#define	WT_ATOMIC_FETCH_ADD4(v, val)		__WT_ATOMIC_FETCH_ADD(v, val, 4)
#define	WT_ATOMIC_CAS4(v, old, new)			__WT_ATOMIC_CAS(v, old, new, 4)
#define	WT_ATOMIC_CAS_VAL4(v, old, new)		__WT_ATOMIC_CAS_VAL(v, old, new, 4)
#define	WT_ATOMIC_STORE4(v, val)			__WT_ATOMIC_STORE(v, val, 4)
#define	WT_ATOMIC_SUB4(v, val)				__WT_ATOMIC_SUB(v, val, 4)

/*int64_t*/
#define	WT_ATOMIC_ADD8(v, val)				__WT_ATOMIC_ADD(v, val, 8)
#define	WT_ATOMIC_FETCH_ADD8(v, val)		__WT_ATOMIC_FETCH_ADD(v, val, 8)
#define	WT_ATOMIC_CAS8(v, old, new)			__WT_ATOMIC_CAS(v, old, new, 8)
#define	WT_ATOMIC_CAS_VAL8(v, old, new)		__WT_ATOMIC_CAS_VAL(v, old, new, 8)
#define	WT_ATOMIC_STORE8(v, val)			__WT_ATOMIC_STORE(v, val, 8)
#define	WT_ATOMIC_SUB8(v, val)				__WT_ATOMIC_SUB(v, val, 8)

/*�ڴ����Ϻ궨��*/

/*��������,��ֹ�����������Ż�ָ��*/
#define WT_BARRIER()						__asm__ volatile("" ::: "memory")

/*�û��ָ��PAUSE����ֹ��������CPU,�������ڿ�ѭ�����У�����PAUSEָ���ܷ�ֹCPU�˳�ѭ����ʱ̫����쳣ָ������*/
#define WT_PAUSE()							__asm__ volatile("pause\n" ::: "memory")

/*����CPU����*/
#if defined(x86_64) || defined(__x86_64__) /*64λCPU*/
#define WT_FULL_BARRIER() do{ __asm__ volatile("mfence" ::: "memory");}while(0)
#define WT_READ_BARRIER() do{ __asm__ volatile("lfence" ::: "memory");}while(0)
#define WT_WRITE_BARRIER() do{ __asm__ volatile("sfence" ::: "memory");}while(0)
#elif defined(i386) || defined(__i386__) /*32ΪCPU*/
#define WT_FULL_BARRIER() do{__asm__ volatile ("lock; addl $0, 0(%%esp)" ::: "memory");}while(0)
#define WT_READ_BARRIER() WT_FULL_BARRIER()
#define WT_WRITE_BARRIER() WT_FULL_BARRIER()
#else /*������CPU��ֱ����ʾ�������*/
#error "No write barrier implementation for this hardware"
#endif
/**********************************************************************************************/




