/*****************************************************************************
*
*****************************************************************************/
#include "wt_internal.h"

static int __lsm_manager_aggressive_update(WT_SESSION_IMPL* session, WT_LSM_TREE* tree);
static int __lsm_manager_run_server(WT_SESSION_IMPL* session);

static WT_THREAD_RET __lsm_worker_manager(void* args);

/*��ȡlsm manager��������Ϣ*/
int __wt_lsm_manager_config(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CONNECTION_IMPL *conn;
	WT_CONFIG_ITEM cval;

	conn = S2C(session);

	/*���merge��ֵ*/
	WT_RET(__wt_config_gets(session, cfg, "lsm_manager.merge", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_LSM_MERGE);

	/*���lsm manager����worker�߳���*/
	WT_RET(__wt_config_gets(session, cfg, "lsm_manager.worker_thread_max", &cval));
	if (cval.val)
		conn->lsm_manager.lsm_workers_max = (uint32_t)cval.val;

	return 0;
}

/*����lsm ��worker�߳�*/
static int __lsm_general_worker_start(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LSM_MANAGER *manager;
	WT_LSM_WORKER_ARGS *worker_args;

	conn = S2C(session);
	manager = &conn->lsm_manager;

	WT_ASSERT(session, manager->lsm_workers > 0);

	/*
	 * Start the worker threads or new worker threads if called via
	 * reconfigure. The LSM manager is worker[0].
	 * This should get more sophisticated in the future - only launching
	 * as many worker threads as are required to keep up with demand.
	 */

	for(; manager->lsm_workers < manager->lsm_workers_max; manager->lsm_workers ++){
		/*����worker�����в���*/
		worker_args = &manager->lsm_worker_cookies[manager->lsm_workers];
		worker_args->work_cond = manager->work_cond;
		worker_args->id = manager->lsm_workers;

		/*��һ��worker��������switch��drop������*/
		if(manager->lsm_workers == 1){
			worker_args->type = WT_LSM_WORK_DROP | WT_LSM_WORK_SWITCH;
		}
		else{
			worker_args->type = WT_LSM_WORK_BLOOM | WT_LSM_WORK_DROP | WT_LSM_WORK_FLUSH | WT_LSM_WORK_SWITCH;

			/*ÿ2���߳�֮�в�����һ��LSM merge����*/
			if (manager->lsm_workers % 2 == 0)
				FLD_SET(worker_args->type, WT_LSM_WORK_MERGE);
		}

		/*���������߳�*/
		F_SET(worker_args, WT_LSM_WORKER_RUN);
		WT_RET(__wt_lsm_worker_start(session, worker_args));
	}

	/*����߳���û�г���3������ô����һ���̼߳���flush����*/
	if (manager->lsm_workers_max == WT_LSM_MIN_WORKERS)
		FLD_SET(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);
	else
		FLD_CLR(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);

	return 0;
}

/*ֹͣlsm worker�߳�*/
static int __lsm_stop_workers(WT_SESSION_IMPL* session)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORKER_ARGS *worker_args;
	uint32_t i;

	manager = &S2C(session)->lsm_manager;

	WT_ASSERT(session, manager->lsm_workers != 0);
	for(i = manager->lsm_workers - 1; i >= manager->lsm_workers_max; i++){
		worker_args = &manager->lsm_worker_cookies[i];

		/*��args�е�������ȥ���������߳̾ͻ��ڴ�����������Ƴ�ִ����*/
		F_CLR(worker_args, WT_LSM_WORKER_RUN);
		WT_ASSERT(session, worker_args->tid != 0);
		/*�����߳�ִ�����˳�*/
		WT_RET(__wt_thread_join(session, worker_args->tid));
		worker_args->tid = 0;
		worker_args->type = 0;
		worker_args->flags = 0;
		manager->lsm_workers--;
	}

	/*����߳���û�г���3��������һ���̼߳���flush����*/
	if (manager->lsm_workers_max == WT_LSM_MIN_WORKERS)
		FLD_SET(manager->lsm_worker_cookies[1].type, WT_LSM_WORK_FLUSH);

	return 0;
}

/*���¶�ȡlsm manager�����ã����Ҷ�Ӧ�����̵߳�����*/
int __wt_lsm_manager_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_LSM_MANAGER *manager;
	uint32_t orig_workers;

	manager = &S2C(session)->lsm_manager;
	orig_workers = manager->lsm_workers_max;

	/*���¶�ȡ������Ϣ*/
	WT_RET(__wt_lsm_manager_config(session, cfg));

	/*work max��workers�������0*/
	if (manager->lsm_workers_max == 0)
		return 0;

	if (manager->lsm_workers == 0)
		return 0;

	/*����ԭ���������߳����ȴ��¶�ȡ���ú���߳�,�������ӵ��߳�*/
	if (manager->lsm_workers_max > orig_workers)
		return __lsm_general_worker_start(session);

	/*����ԭ�����߳����������õ��߳�������ôͣ��������߳�*/
	WT_ASSERT(session, manager->lsm_workers_max < orig_workers);
	WT_RET(__lsm_stop_workers(session));

	return 0;
}

int __wt_lsm_manager_start(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_SESSION_IMPL *worker_session;
	uint32_t i;

	manager = &S2C(session)->lsm_manager;

	/*����3���̣߳�1��manager�̣߳�1��switch�̺߳�һ��ͨ�õĲ����߳�*/
	WT_ASSERT(session, manager->lsm_workers_max > 2);

	/*����һ��session,��Ҫ����lsm worker�Ĳ���,�൱�ڳ�ʼ��worker args*/
	for(i = 0; i < WT_LSM_MAX_WORKERS; i++){
		WT_ERR(__wt_open_internal_session(S2C(session), "lsm-worker", 1, 0, &worker_session));
		worker_session->isolation = TXN_ISO_READ_UNCOMMITTED;
		manager->lsm_worker_cookies[i].session = worker_session;
	}

	/*����manager�߳�*/
	WT_ERR(__wt_thread_create(session, &manager->lsm_worker_cookies[0].tid, __lsm_worker_manager, &manager->lsm_worker_cookies[0]));

	F_SET(S2C(session), WT_CONN_SERVER_LSM);
	if(0){
err:
		/*�رյ�Ϊargs�򿪵�session*/
		for (i = 0;(worker_session = manager->lsm_worker_cookies[i].session) != NULL; i++)
			WT_TRET((&worker_session->iface)->close(&worker_session->iface, NULL));
	}

	return ret;
}

/*�ͷŵ�һ��lsm worker unit*/
void __wt_lsm_manager_free_work_unit(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT *entry)
{
	if (entry != NULL) {
		WT_ASSERT(session, entry->lsm_tree->queue_ref > 0);

		WT_ATOMIC_SUB4(entry->lsm_tree->queue_ref, 1);
		__wt_free(session, entry);
	}
}

/*��session��Ӧ��LSM manager��������*/
int __wt_lsm_manager_destroy(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *current, *next;
	WT_SESSION *wt_session;
	uint32_t i;
	uint64_t removed;

	conn = S2C(session);
	manager = &conn->lsm_manager;
	removed = 0;

	if(manager->lsm_workers > 0){
		/*�ȴ�lsm managerִ�������*/
		while (F_ISSET(conn, WT_CONN_SERVER_LSM))
			__wt_yield();

		/*���lsm �򿪵�hander*/
		ret = __wt_lsm_tree_close_all(session);
		/*�ȴ�lsm manager�˳�*/
		WT_TRET(__wt_thread_join(session, manager->lsm_worker_cookies[0].tid));
		manager->lsm_worker_cookies[0].tid = 0;

		/*�Ƴ�switch queue�е�����*/
		for (current = TAILQ_FIRST(&manager->switchqh); current != NULL; current = next) {
			next = TAILQ_NEXT(current, q);
			TAILQ_REMOVE(&manager->switchqh, current, q);
			++removed;
			__wt_lsm_manager_free_work_unit(session, current);
		}

		/*�Ƴ�app queue�е�����*/
		for (current = TAILQ_FIRST(&manager->appqh); current != NULL; current = next) {
			next = TAILQ_NEXT(current, q);
			TAILQ_REMOVE(&manager->appqh, current, q);
			++removed;
			__wt_lsm_manager_free_work_unit(session, current);
		}

		/*�Ƴ�manager queue�е�����*/
		for (current = TAILQ_FIRST(&manager->managerqh); current != NULL; current = next) {
			next = TAILQ_NEXT(current, q);
			TAILQ_REMOVE(&manager->managerqh, current, q);
			++removed;
			__wt_lsm_manager_free_work_unit(session, current);
		}

		/*�ر�����worker args��Ӧ��session*/
		for (i = 0; i < WT_LSM_MAX_WORKERS; i++) {
			wt_session = &manager->lsm_worker_cookies[i].session->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
		}
	}

	WT_STAT_FAST_CONN_INCRV(session, lsm_work_units_discarded, removed);

	/*ɾ����Ϣ���ж�Ӧ�ĸ���������*/
	__wt_spin_destroy(session, &manager->switch_lock);
	__wt_spin_destroy(session, &manager->app_lock);
	__wt_spin_destroy(session, &manager->manager_lock);

	WT_TRET(__wt_cond_destroy(session, &manager->work_cond));

	return ret;
}

/*���¼���lsm tree��merge_aggressiveness*/
static int __lsm_manager_aggressive_update(WT_SESSION_IMPL* session, WT_LSM_TREE* lsm_tree)
{
	struct timespec now;
	uint64_t chunk_wait, stallms;
	u_int new_aggressive;

	/*����flush���ʱ��,��λ����*/
	WT_RET(__wt_epoch(session, &now));
	stallms = WT_TIMEDIFF(now, lsm_tree->last_flush_ts) / WT_MILLION;

	if(lsm_tree->nchunks > 1){
		chunk_wait = stallms / (lsm_tree->chunk_fill_ms == 0 ? 10000 : lsm_tree->chunk_fill_ms);
	}
	else
		chunk_wait = 0;

	new_aggressive = (u_int)(chunk_wait / lsm_tree->merge_min);

	if (new_aggressive > lsm_tree->merge_aggressiveness) {
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
			"LSM merge %s got aggressive (old %u new %u), merge_min %d, %u / %" PRIu64,
			lsm_tree->name, lsm_tree->merge_aggressiveness,
			new_aggressive, lsm_tree->merge_min, stallms,
			lsm_tree->chunk_fill_ms));

		lsm_tree->merge_aggressiveness = new_aggressive;
	}

	return 0;
}

/*ֹͣ����lsm manager�߳�,��ǿ��ֹͣ*/
static int __lsm_manager_worker_shutdown(WT_SESSION_IMPL* session)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	u_int i;

	manager = &S2C(session)->lsm_manager;
	/*������lsm manager thread*/
	for (i = 1; i < manager->lsm_workers; i++) {
		WT_ASSERT(session, manager->lsm_worker_cookies[i].tid != 0);
		WT_TRET(__wt_cond_signal(session, manager->work_cond));
		WT_TRET(__wt_thread_join(session, manager->lsm_worker_cookies[i].tid));
	}

	return ret;
}

/*lsm manager���������к�������Ҫʵ�ֶ�swtich/flush/bloom/drop/merge�Ȳ����Ĵ���*/
static int __lsm_manager_run_server(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	struct timespec now;
	uint64_t fillms, pushms;
	int dhandle_locked;

	conn = S2C(session);
	dhandle_locked = 0;

	while(F_ISSET(conn, WT_CONN_SERVER_RUN)){
		__wt_sleep(0, 10000);

		if (TAILQ_EMPTY(&conn->lsmqh))
			continue;

		__wt_spin_lock(session, &conn->dhandle_lock);
		F_SET(session, WT_SESSION_HANDLE_LIST_LOCKED);

		/*��ÿ��lsm tree����ɨ��*/
		TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q){
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
				continue;

			/*����merge_aggressiveness*/
			WT_ERR(__lsm_manager_aggressive_update(session, lsm_tree));
			WT_ERR(__wt_epoch(session, &now));

			pushms = (lsm_tree->work_push_ts.tv_sec == 0 ? 0 : WT_TIMEDIFF(now, lsm_tree->work_push_ts) / WT_MILLION);
			fillms = 3 * lsm_tree->chunk_fill_ms;
			if(fillms == 0)
				fillms = 10000;

			/*workersȫ���ڽ��в�������,���跢���κ��źŵ���������߳�*/
			if (lsm_tree->queue_ref >= LSM_TREE_MAX_QUEUE)
				WT_STAT_FAST_CONN_INCR(session, lsm_work_queue_max);
			else if((!lsm_tree->modified && lsm_tree->nchunks > 1) 
				|| (lsm_tree->queue_ref == 0 && lsm_tree->nchunks > 1) 
				||(lsm_tree->merge_aggressiveness > 3 && !F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING))
				|| pushms > fillms){
					WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree));
					WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_DROP, 0, lsm_tree));
					WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
					WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_BLOOM, 0, lsm_tree));

					WT_ERR(__wt_verbose(session, WT_VERB_LSM,
						"MGR %s: queue %d mod %d nchunks %d flags 0x%x aggressive %d pushms %" PRIu64 "fillms %" PRIu64,
						lsm_tree->name, lsm_tree->queue_ref,
						lsm_tree->modified, lsm_tree->nchunks,
						lsm_tree->flags,
						lsm_tree->merge_aggressiveness,
						pushms, fillms));

					WT_ERR(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_MERGE, 0, lsm_tree));
			}
		}
		__wt_spin_unlock(session, &conn->dhandle_lock);
		F_CLR(session, WT_SESSION_HANDLE_LIST_LOCKED);
		dhandle_locked = 0;
	}

err:
	if (dhandle_locked) {
		__wt_spin_unlock(session, &conn->dhandle_lock);
		F_CLR(session, WT_SESSION_HANDLE_LIST_LOCKED);
	}

	return ret;
}

/*lsm manager�߳�ִ���庯��*/
static WT_THREAD_RET __lsm_worker_manager(void* args)
{
	WT_DECL_RET;
	WT_LSM_WORKER_ARGS *cookie;
	WT_SESSION_IMPL *session;

	cookie = (WT_LSM_WORKER_ARGS *)args;
	session = cookie->session;

	WT_ERR(__lsm_general_worker_start(session));
	WT_ERR(__lsm_manager_run_server(session));
	WT_ERR(__lsm_manager_worker_shutdown(session));

	if (ret != 0) {
err:		
		WT_PANIC_MSG(session, ret, "LSM worker manager thread error");
	}

	/*�õȴ�ֹͣ�����߳�ֹͣ�ȴ�*/
	F_CLR(S2C(session), WT_CONN_SERVER_LSM);

	return WT_THREAD_RET_VALUE;
}

/*��ո�����������е�����*/
int __wt_lsm_manager_clear_tree(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *current, *next;
	uint64_t removed;

	manager = &S2C(session)->lsm_manager;
	removed = 0;

	/* Clear out the tree from the switch queue */
	__wt_spin_lock(session, &manager->switch_lock);
	for (current = TAILQ_FIRST(&manager->switchqh); current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;

		++removed;
		TAILQ_REMOVE(&manager->switchqh, current, q);
		__wt_lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->switch_lock);

	/* Clear out the tree from the application queue */
	__wt_spin_lock(session, &manager->app_lock);
	for (current = TAILQ_FIRST(&manager->appqh); current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;

		++removed;
		TAILQ_REMOVE(&manager->appqh, current, q);
		__wt_lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->app_lock);

	/* Clear out the tree from the manager queue */
	__wt_spin_lock(session, &manager->manager_lock);
	for (current = TAILQ_FIRST(&manager->managerqh); current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;
		++removed;
		TAILQ_REMOVE(&manager->managerqh, current, q);
		__wt_lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->manager_lock);

	WT_STAT_FAST_CONN_INCRV(session, lsm_work_units_discarded, removed);

	return 0;
}

#define	LSM_POP_ENTRY(qh, qlock, qlen) do {				\
	if (TAILQ_EMPTY(qh))								\
		return (0);										\
	__wt_spin_lock(session, qlock);						\
	TAILQ_FOREACH(entry, (qh), q) {						\
		if (FLD_ISSET(type, entry->type)) {				\
			TAILQ_REMOVE(qh, entry, q);					\
			WT_STAT_FAST_CONN_DECR(session, qlen);		\
			break;										\
		}												\
	}													\
	__wt_spin_unlock(session, (qlock));					\
} while (0)

/*����type������ֵ���Ӷ�Ӧ���������������ȡ��һ����������*/
int __wt_lsm_manager_pop_entry(WT_SESSION_IMPL *session, uint32_t type, WT_LSM_WORK_UNIT **entryp)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;
	*entryp = NULL;
	entry = NULL;

	if(type == WT_LSM_WORK_SWITCH){
		LSM_POP_ENTRY(&manager->switchqh, &manager->switch_lock, lsm_work_queue_switch);
	}
	else if(type == WT_LSM_WORK_MERGE){
		LSM_POP_ENTRY(&manager->managerqh, &manager->manager_lock, lsm_work_queue_manager);
	}
	else{
		LSM_POP_ENTRY(&manager->appqh, &manager->app_lock, lsm_work_queue_app);
	}

	if(entry != NULL){
		WT_STAT_FAST_CONN_INCR(session, lsm_work_units_done);
	}

	*entryp = entry;

	return 0;
}


#define	LSM_PUSH_ENTRY(qh, qlock, qlen) do {				\
	__wt_spin_lock(session, qlock);							\
	TAILQ_INSERT_TAIL((qh), entry, q);						\
	WT_STAT_FAST_CONN_INCR(session, qlen);					\
	__wt_spin_unlock(session, qlock);						\
} while (0)

/*����type����ֵ�����Ӧ�Ĳ��������в���һ������*/
int __wt_lsm_manager_push_entry(WT_SESSION_IMPL *session, uint32_t type, uint32_t flags, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;

	switch(type){
	case WT_LSM_WORK_BLOOM:
		if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF)) /*BLOOM�������͹ر�*/
			return 0;
		break;

	case WT_LSM_WORK_MERGE:
		if (!F_ISSET(lsm_tree, WT_LSM_TREE_MERGES))	/*LSM TREE�Ĳ������͹ر�*/
			return 0;
		break;
	}

	WT_RET(__wt_epoch(session, &lsm_tree->work_push_ts));

	WT_RET(__wt_calloc_one(session, &entry));
	entry->type = type;
	entry->flags = flags;
	entry->lsm_tree = lsm_tree;
	WT_ATOMIC_ADD4(lsm_tree->queue_ref, 1);

	WT_STAT_FAST_CONN_INCR(session, lsm_work_units_created);

	if (type == WT_LSM_WORK_SWITCH)
		LSM_PUSH_ENTRY(&manager->switchqh, &manager->switch_lock, lsm_work_queue_switch);
	else if (type == WT_LSM_WORK_MERGE)
		LSM_PUSH_ENTRY(&manager->managerqh, &manager->manager_lock, lsm_work_queue_manager);
	else
		LSM_PUSH_ENTRY(&manager->appqh, &manager->app_lock, lsm_work_queue_app);
	/*��worker thread��������������ź�*/
	WT_RET(__wt_cond_signal(session, manager->work_cond));

	return 0;
}


