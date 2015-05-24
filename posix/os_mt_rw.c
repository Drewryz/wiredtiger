/*****************************************************************
*wt_rwlock_t�Ľṹ������mutex.h��
*��rw_lock��ʵ�֣�ԭ�����£�
*���writers = users, ��readers = users����ʾ����ͬʱs-lockҲ���Ի��x-lock,������free״̬
*���writers = users, readers != users, ����ϲ�����
*���readers = users, writes != users,��ʾrwlock���ų�read lock�����Ի��s-lock,���ܻ��x-lock, ���߳�����ռ��s-lock
*���readers != users, writes != users,��ʾ���߳�����waiting lock,����һ���߳�����ռ��x-lock
*������Щֵ�ĸı����CAS����������Ϻ�ȡ��
*****************************************************************/

int __wt_rwlock_alloc(WT_SESSION_IMPL* session, WT_RWLOCK** rwlockp, const char* name)
{
	WT_RWLOCK* rwlock;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: alloc %s", name));
	WT_RET(__wt_calloc_one(session, &rwlock));

	rwlock->name = name;
	*rwlockp = rwlock;

	return 0;
}

int __wt_try_readlock(WT_SESSION_IMPL* session, WT_RWLOCK* rwlock)
{
	wt_rwlock_t* l;
	uint64_t old, new, pad, users, writers;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: try_readlock %s", rwlock->name));
	l = &rwlock->rwlock;
	pad = l->s.pad;
	users = l->s.users;
	writers = l->s.writers;

	/*ֻ���ж�readers�Ƿ��user��һ�µģ������һֱ�ģ���ʾ���Ի��s-lock*/
	old = (pad << 48) + (users << 32) + (users << 16) + writers;
	/*ͬʱ��reader��users + 1,��Ϊ��try lock,���Ա���ԭ���Ե����*/
	new = (pad << 48) + ((users + 1) << 32) + ((users + 1) << 16) + writers;

	return (WT_ATOMIC_CAS_VAL8(l->u, old, new) == old ? 0 : EBUSY);
}

int __wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t* l;
	uint64_t me;
	uint16_t val;
	int pause_cnt;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;
	/*��users + 1*/
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	/*���user + 1֮ǰ��user��ֵ*/
	val = (uint16_t)(me >> 32);

	/*�ж�+1֮ǰ��usersֵ�Ƿ��readersһ�£������һ�£���ʾ��x-lock�ų�*/
	for(pause_cnt = 0; val != l->s.readers;){
		/*��ֹCPU��������,�������ڴ����̶߳�rwlock����ʱ��*/
		if(++pause_cnt < 1000)
			WT_PAUSE();
		else
			__wt_sleep(0, 10);
	}

	/*���s-lock,��readers���óɺ�usersһ�£������������������߳�Ҳ���Ի��s-lock,����û��ԭ�Ӳ�������Ϊ��ʹ�ж���߳���
	�����forѭ���еȴ�������reader�Ǵ���+1�ģ�ֻ����һ��reader +1,�Ż�����һ���ȴ����߳��˳������ѭ��*/
	++ l->s.readers;

	return 0;
}

int __wt_readunlock(WT_SESSION_IMPL* session, WT_RWLOCK* rwlock)
{
	wt_rwlock_t *l;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: read unlock %s", rwlock->name));

	/*�����ڵȴ���x-lock�����Ȩ�������п��ܶ���߳�ͬʱADD,������ԭ�Ӳ���*/
	l = &rwlock->rwlock;
	WT_ATOMIC_ADD2(l->s.writers, 1);
}

int __wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t old, new, pad, readers, users;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: try_writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;
	pad = l->s.pad;
	readers = l->s.readers;
	users = l->s.users;

	/*ֻ��Ҫ�Ƚ�users��writers�Ƿ�һ�£����һֱ���Ϳ��Ի��x-lock*/
	old = (pad << 48) + (users << 32) + (readers << 16) + users;
	/*��users + 1,��Ϊx-lock����read��writer�ģ����������Ĳ���Ҫ+1*/
	new = (pad << 48) + ((users + 1) << 32) + (readers << 16) + users;

	return (WT_ATOMIC_CAS_VAL8(l->u, old, new) == old ? 0 : EBUSY);
}

int __wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t me;
	uint16_t val;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	val = (uint16_t)(me >> 32); /*��ȡ+1֮ǰ��users*/
	/*���+1֮ǰ��users��writersһ�£����Ի��x-lock*/
	while(val != l->s.writers)
		WT_PAUSE();

	return 0;
}

int __wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l, copy;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: writeunlock %s", rwlock->name));

	/*ԭ���Զ�readers��writersͬʱ + 1��ʹ�õȴ������߳̿��Թ�ƽ�ĵõ���*/
	l = &rwlock->rwlock;
	copy = *l;

	/*��ֹ��copy��ֵ���Ż���������ӱ������ϣ����ܻ�ֱ����l������copy,��������������ԭ���Եĸ���*/
	WT_BARRIER();

	++copy.s.writers;
	++copy.s.readers;

	/*ԭ���Ը���readers��writers*/
	l->us = copy.us;

	return 0;
}

int __wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);

	*rwlockp = NULL;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: destroy %s", rwlock->name));

	__wt_free(session, rwlock);
	return (0);
}


/******************************************************************/
