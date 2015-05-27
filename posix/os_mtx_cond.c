/**************************************************************************
*�߳��ź�����װ
**************************************************************************/

#include "wt_internal.h"

/*����һ���ź���*/
int __wt_cond_alloc(WT_SESSION_IMPL *session, const char *name, int is_signalled, WT_CONDVAR **condp)
{
	WT_CONDVAR *cond;
	WT_DECL_RET;

	WT_RET(__wt_calloc_one(session, &cond));
	/*��cond�ĳ�ʼ��*/
	WT_ERR(pthread_mutex_init(&cond->mtx, NULL));
	WT_ERR(pthread_cond_init(&cond->cond, NULL));

	cond->name = name;
	cond->waiters = is_signalled ? -1 : 0;

	*condp = cond;

	return 0;

err:
	__wt_free(session, cond);
	return ret;
}

/*�ȴ��źŴ���*/
int __wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs)
{
	struct timespec ts;
	WT_DECL_RET;
	int locked;

	locked = 0;

	/*�źŵĵȴ��߼�����ԭ����+��,���+1֮ǰWAITERS = -1����ʾ�ź��Ѿ�����*/
	if (WT_ATOMIC_ADD4(cond->waiters, 1) == 0)
		return (0);

	if (session != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "wait %s cond (%p)", cond->name, cond));
		WT_STAT_FAST_CONN_INCR(session, cond_wait);
	}

	/*�Ȼ��cond->mtx*/
	WT_ERR(pthread_mutex_lock(&cond->mtx));
	locked = 1;

	if (usecs > 0) {
		WT_ERR(__wt_epoch(session, &ts));
		ts.tv_sec += (time_t) (((uint64_t)ts.tv_nsec + 1000 * usecs) / WT_BILLION);
		ts.tv_nsec = (long)(((uint64_t)ts.tv_nsec + 1000 * usecs) % WT_BILLION);
		ret = pthread_cond_timedwait(&cond->cond, &cond->mtx, &ts);
	} 
	else
		ret = pthread_cond_wait(&cond->cond, &cond->mtx);

	if (ret == EINTR || ret == ETIMEDOUT)
		ret = 0;
	
	/*ԭ���Լ���һ���ȴ���*/
	WT_ATOMIC_SUB4(cond->waiters, 1);

err:
	if (locked)
		WT_TRET(pthread_mutex_unlock(&cond->mtx));

	if(ret == 0)
		return 0;

	WT_RET_MSG(session, ret, "pthread_cond_wait");
}

int __wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;
	int locked = 0;

	if(session != NULL)
		WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "signal %s cond (%p)", cond->name, cond));

	/*����ź��Ѿ�signal*/
	if(cond->waiters == -1)
		return 0;

	/*�еȴ��߻���waiters��Ϊ0���п�����-1,�ڴ˹����У����waiters=0����ʾ�и�waiters��������ź�*/
	if (cond->waiters > 0 || !WT_ATOMIC_CAS4(cond->waiters, 0, -1)) {
		WT_ERR(pthread_mutex_lock(&cond->mtx));
		locked = 1;
		WT_ERR(pthread_cond_broadcast(&cond->cond));
	}

err:	
	if (locked)
		WT_TRET(pthread_mutex_unlock(&cond->mtx));

	if (ret == 0)
		return (0);

	WT_RET_MSG(session, ret, "pthread_cond_broadcast");
}

int __wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp)
{
	WT_CONDVAR* cond;
	WT_DECL_RET;

	cond = *condp;
	if(cond == NULL)
		return 0;

	/*�����ź�*/
	ret = pthread_cond_destroy(&cond->cond);
	WT_TRET(pthread_mutex_destroy(&cond->mtx));

	__wt_free(session, *condp);

	return ret;
}

