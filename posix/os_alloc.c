/***************************************************************************
*��װ�ڴ���亯��
***************************************************************************/

#include "wt_internal.h"

int __wt_calloc(WT_SESSION_IMPL *session, size_t number, size_t size, void *retp)
{
	void* p;

	WT_ASSERT(session, number != 0 && seize != 0);

	if(session != NULL)
		WT_STAT_FAST_CONN_INCR(session, memory_allocation);

	p = calloc(number, size);
	if(p == NULL)
		WT_RET_MSG(session, __wt_errno(), "memory allocation");

	*(void **)retp = p;

	return 0;
}

/*bytes_to_allocate�ط���*/
int __wt_realloc(WT_SESSION_IMPL* session, size_t* bytes_allocated_ret, size_t bytes_to_allocate, void* retp)
{
	void* p;
	size_t bytes_allocated;

	p = *(void **)retp;
	bytes_allocated = (bytes_allocated_ret == NULL) ? 0 : *bytes_allocated_ret;
	/*�Բ�������У��*/
	WT_ASSERT(session, (p == NULL && bytes_allocated == 0) ||
		(p != NULL && (bytes_allocated_ret == NULL || bytes_allocated != 0)));

	WT_ASSERT(session, bytes_to_allocate != 0);
	WT_ASSERT(session, bytes_allocated < bytes_to_allocate);

	if(session != NULL){
		/*����ͳ����Ϣ����*/
		if(p == NULL)
			WT_STAT_FAST_CONN_INCR(session, memory_allocation);
		else
			WT_STAT_FAST_CONN_INCR(session, memory_grow);
	}

	/*�����ڴ��ط���*/
	p = realloc(p, bytes_to_allocate);
	if(p == NULL)
		WT_RET_MSG(session, __wt_errno(), "memory allocation");

	/*���·�����ڴ�������ռ���г�ʼ��*/
	memset((uint8_t *)p + bytes_allocated, 0, bytes_to_allocate - bytes_allocated);

	if(bytes_allocated_ret != NULL)
		*bytes_allocated_ret = bytes_to_allocate;

	*(void **)retp = p;

	return 0;
}

/*��session->buffer_alignment�����ط���,���磺buffer_alignment = 16,��ôһ���ǰ�16�ı������з���*/
int __wt_realloc_aligned(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp)
{
	WT_DECL_RET;

	if (session != NULL && S2C(session)->buffer_alignment > 0) {
		void *p, *newp;
		size_t bytes_allocated;

		/*
		* Sometimes we're allocating memory and we don't care about the
		* final length -- bytes_allocated_ret may be NULL.
		*/
		p = *(void **)retp;
		bytes_allocated = (bytes_allocated_ret == NULL) ? 0 : *bytes_allocated_ret;

		WT_ASSERT(session,
			(p == NULL && bytes_allocated == 0) ||
			(p != NULL &&
			(bytes_allocated_ret == NULL || bytes_allocated != 0)));

		WT_ASSERT(session, bytes_to_allocate != 0);
		WT_ASSERT(session, bytes_allocated < bytes_to_allocate);

		/*
		* We are going to allocate an aligned buffer.  When we do this
		* repeatedly, the allocator is expected to start on a boundary
		* each time, account for that additional space by never asking
		* for less than a full alignment size.  The primary use case
		* for aligned buffers is Linux direct I/O, which requires that
		* the size be a multiple of the alignment anyway.
		* ��IO�������,��������С�ǰ�buffer_alignment���룬������ĵ�ַҲ��buffer_alignment����
		* Ӧ����Ϊ������direct I/O
		*/
		bytes_to_allocate = WT_ALIGN(bytes_to_allocate, S2C(session)->buffer_alignment);

		if (session != NULL)
			WT_STAT_FAST_CONN_INCR(session, memory_allocation);

		if ((ret = posix_memalign(&newp, S2C(session)->buffer_alignment, bytes_to_allocate)) != 0)
			WT_RET_MSG(session, ret, "memory allocation");

		if (p != NULL)
			memcpy(newp, p, bytes_allocated);

		__wt_free(session, p);
		p = newp;

		/* Clear the allocated memory (see above). */
		memset((uint8_t *)p + bytes_allocated, 0,
			bytes_to_allocate - bytes_allocated);

		/* Update caller's bytes allocated value. */
		if (bytes_allocated_ret != NULL)
			*bytes_allocated_ret = bytes_to_allocate;

		*(void **)retp = p;
		return (0);
	}
	else{
		return __wt_realloc(session, bytes_allocated_ret, bytes_to_allocate, retp);
	}
}

int __wt_strndup(WT_SESSION_IMPL* session, const void* str, size_t len ,void* retp)
{
	void *p;

	if (str == NULL){
		*(void **)retp = NULL;
		return 0;
	}

	/*����һ���ַ����ռ䣬���ҽ�str���Ƹ��µĿռ�*/
	WT_RET(__wt_calloc(session, len + 1, 1, &p));
	memcpy(p, str, len);
	*(void**)retp = p;

	return 0;
}

int __wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
{
	return (__wt_strndup(session, str, (str == NULL) ? 0 : strlen(str), retp));
}

void __wt_free_int(WT_SESSION_IMPL *session, const void *p_arg)
{
	void* p = *(void **)p_arg;
	if(p == NULL)
		return ;

	*(void **)p_arg = NULL;

	if (session != NULL)
		WT_STAT_FAST_CONN_INCR(session, memory_free);

	free(p);
}


