/********************************************************
 * wiredtiger��lru�㷨ʵ��ģ��
 ********************************************************/

#include "wt_internal.h"

static int				__evict_clear_walks(WT_SESSION_IMPL *);
static int				__evict_has_work(WT_SESSION_IMPL *, uint32_t *);
static int WT_CDECL		__evict_lru_cmp(const void *, const void *);
static int				__evict_lru_pages(WT_SESSION_IMPL *, int);
static int				__evict_lru_walk(WT_SESSION_IMPL *, uint32_t);
static int				__evict_pass(WT_SESSION_IMPL *);
static int				__evict_walk(WT_SESSION_IMPL *, uint32_t);
static int				__evict_walk_file(WT_SESSION_IMPL *, u_int *, uint32_t);
static WT_THREAD_RET	__evict_worker(void *);
static int				__evict_server_work(WT_SESSION_IMPL *);

/*ȷ��һ��evict entry��evict�����ȼ�*/
static inline uint64_t __evict_read_gen(const WT_EVICT_ENTRY* entry)
{
	WT_PAGE*	page;
	uint64_t	read_gen;

	/*��ʾevict entryû��ָ��ref����ֱ�ӷ���*/
	if (entry->ref == NULL)
		return UINT64_MAX;

	page = entry->ref->page;
	if (__wt_page_is_empty(page))	/*page�ǿյģ�����һ����ʼ��read gen����*/
		return WT_READGEN_OLDEST;

	/*
	* Skew the read generation for internal pages, we prefer to evict leaf pages.
	*/
	read_gen = page->read_gen + entry->btree->evict_priority;
	if (WT_PAGE_IS_INTERNAL(page))
		read_gen += WT_EVICT_INT_SKEW;

	return read_gen;
}

/*qsort�����ıȽϺ�������,ʵ�����ǱȽ�����WT_EVICT_ENTRY�����read gen��С, ���a > b����1�� a < b����-1*/
static int WT_CDECL __evict_lru_cmp(const void* a, const void* b)
{
	uint64_t a_lru, b_lru;

	a_lru = __evict_read_gen(a);
	b_lru = __evict_read_gen(b);

	return ((a_lru < b_lru) ? -1 : (a_lru == b_lru) ? 0 : 1);
}

/*��LRU evict list�������һ��evict entry*/
static inline void __evict_list_clear(WT_SESSION_IMPL* session, WT_EVICT_ENTRY* e)
{
	if (e->ref != NULL){
		WT_ASSERT(session, F_ISSET_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU);
	}

	e->ref = NULL;
	e->btree = WT_DEBUG_POINT;
}


/*
 *
 *	Make sure a page is not in the LRU eviction list.  This called from the
 *	page eviction code to make sure there is no attempt to evict a child
 *	page multiple times.
 */
/*��ref��Ӧ��evict entry�����evict lru list���������ȥ*/
void __wt_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_CACHE* cache;
	WT_EVICT_ENTRY* evict;
	uint32_t i, elem;

	/*root page�ʹ���LOCKED״̬�µ�page���ܱ�evict*/
	WT_ASSERT(session, __wt_ref_is_root(ref) || ref->state == WT_REF_LOCKED);

	/*page����evict lru list���У���������*/
	if (!F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, cache->evict_lock);

	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++){
		if (evict->ref == ref) {
			__evict_list_clear(session, evict);
			break;
		}
	}

	__WT_ASSERT(session, !F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU));

	__wt_spin_unlock(session, &cache->evict_lock);
}

/*����eviction thread*/
int __wt_evict_server_wake(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	cache = conn->cache;

	/*verbose��Ϣ*/
	if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTSERVER)){
		uint64_t bytes_inuse, bytes_max;

		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER, "waking, bytes inuse %s max (%" 
			PRIu64 "MB %s %" PRIu64 "MB)",
			bytes_inuse <= bytes_max ? "<=" : ">", bytes_inuse / WT_MEGABYTE,
			bytes_inuse <= bytes_max ? "<=" : ">", bytes_max / WT_MEGABYTE));
	}

	/*����evict�߳�*/
	return __wt_cond_signal(session, cache->evict_cond);
}

/*evict thread���庯��*/
static WT_THREAD_RET __evict_server(void* arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *worker;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)){
		/*Evict pages from cache as needed*/
		WT_ERR(__evict_pass(session));

		/*__evict_pass����ʱ��Ƚϳ��������˳������������Ҫ���ж�һ���߳��Ƿ��˳�ѭ��*/
		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;

		   /*
			* If we have caught up and there are more than the minimum
			* number of eviction workers running, shut one down.
			* ��ֹevict workers thread̫��
			*/
		if (conn->evict_workers > conn->evict_workers_min){
			WT_TRET(__wt_verbose(session, WT_VERB_EVICTSERVER, "Stopping evict worker: %"PRIu32"\n", conn->evict_workers));

			/*ֹͣconn->evict_workers��Ӧ��worker*/
			worker = &conn->evict_workctx[--conn->evict_workers];
			F_CLR(worker, WT_EVICT_WORKER_RUN);
			/*����ָ���˳���workers,���ȴ����˳�*/
			WT_TRET(__wt_cond_signal(session, cache->evict_waiter_cond));
			WT_TRET(__wt_thread_join(session, worker->tid));

			WT_ASSERT(session, ret == 0);
			if (ret != 0){
				(void)__wt_msg(session, "Error stopping eviction worker: %d", ret);
			}
		}

		/*
		* Clear the walks so we don't pin pages while asleep,
		* otherwise we can block applications evicting large pages.
		*/
		if (!F_ISSET(cache, WT_CACHE_STUCK)){
			WT_ERR(__evict_clear_walks(session));
			cache->flags ~= WT_CACHE_WALK_REVERSE;
		}

		/*����˯��״̬*/
		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "sleeping"));
		WT_ERR(__wt_cond_wait(session, cache->evict_cond, 100000));
		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "waking"));
	}

	WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "cache eviction server exiting"));

	if (cache->pages_inmem != cache->pages_evict)
		__wt_errx(session,"cache server: exiting with %" PRIu64 " pages in memory and %" PRIu64 " pages evicted", 
		cache->pages_inmem, cache->pages_evict);
	if (cache->bytes_inmem != 0)
		__wt_errx(session,"cache server: exiting with %" PRIu64 " bytes in memory", cache->bytes_inmem);
	if (cache->bytes_dirty != 0 || cache->pages_dirty != 0)
		__wt_errx(session, "cache server: exiting with %" PRIu64 " bytes dirty and %" PRIu64 " pages dirty", cache->bytes_dirty, cache->pages_dirty);

	if (0){
err:	WT_PANIC_MSG(session, ret, "cache eviction server error");
	}

	return WT_THREAD_RET_VALUE;
}

static int __evict_workers_resize(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *workers;
	size_t alloc;
	uint32_t i;

	conn = S2C(session);

	alloc = conn->evict_workers_alloc * sizeof(workers);
	WT_RET(__wt_realloc(session, &alloc, conn->evict_workers_max * sizeof(*workers), &conn->evict_workctx));
	workers = conn->evict_workctx;

	/*�½������workers*/
	for (i = conn->evict_workers_alloc; i < conn->evict_workers_max; i++){
		WT_ERR(__wt_open_internal_session(conn, "eviction-worker", 0, 0, &workers[i].session));

		workers[i].id = i;
		F_SET(workers[i].session, WT_SESSION_CAN_WAIT);

		/*��С��evict_workers_min slot����worker thread����,���ٱ�֤evict_workers_min���̴߳��ڼ���״̬*/
		if (i < conn->evict_workers_min){
			++conn->evict_workers;
			F_SET(&workers[i], WT_EVICT_WORKER_RUN);
			WT_ERR(__wt_thread_create(workers[i].session, &workers[i].tid, __evict_worker, &workers[i]));
		}
	}

err:
	conn->evict_workers_alloc = conn->evict_workers_max;
	return ret;
}

int __wt_evict_create(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	conn = S2C(session);

	/*����evict server thread����run״̬*/
	F_SET(conn, WT_CONN_EVICTION_RUN);

	/*��Ϊevict thread��Ҫ��page���ж�д����������Ҫ����һ�����ڶ�д������session����*/
	WT_RET(__wt_open_internal_session(conn, "eviction-server", 0, 0, &conn->evict_session));
	session = conn->evict_session;

	/*
	* If eviction workers were configured, allocate sessions for them now.
	* This is done to reduce the chance that we will open new eviction
	* sessions after WT_CONNECTION::close is called.
	*
	* If there's only a single eviction thread, it may be called upon to
	* perform slow operations for the block manager.  (The flag is not
	* reset if reconfigured later, but I doubt that's a problem.)
	* �������õ�workers����������
	*/
	if (conn->evict_workers_max > 0)
		WT_RET(__evict_workers_resize(session));
	else
		F_SET(session, WT_SESSION_CAN_WAIT);

	/*����evict server thread,����߳������������workers thread��*/
	WT_RET(__wt_thread_create(session, &conn->evict_tid, __evict_server, session));
	conn->evict_tid_set = 1;

	return 0;
}

/*����eviction server thread*/
int __wt_evict_destroy(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *workers;
	WT_SESSION *wt_session;
	uint32_t i;

	conn = S2C(session);
	cache = conn->cache;
	workers = conn->evict_workctx;

	/*���eviction server thread�����б�ʾ*/
	F_CLR(conn, WT_CONN_EVICTION_RUN);

	WT_TRET(__wt_verbose(session, WT_VERB_EVICTSERVER, "waiting for main thread"));

	/*���Ѳ��ȴ�evict server thread���˳�*/
	if (conn->evict_tid_set) {
		WT_TRET(__wt_evict_server_wake(session));
		WT_TRET(__wt_thread_join(session, conn->evict_tid));
		conn->evict_tid_set = 0;
	}
	WT_TRET(__wt_verbose(session, WT_VERB_EVICTSERVER, "waiting for helper threads"));

	/*�ȴ�����workers thread���˳�*/
	for (i = 0; i < conn->evict_workers; i++) {
		WT_TRET(__wt_cond_signal(session, cache->evict_waiter_cond));
		WT_TRET(__wt_thread_join(session, workers[i].tid));
	}

	/*�ر�workers��Ӧ��session*/
	if (conn->evict_workctx != NULL) {
		for (i = 0; i < conn->evict_workers_alloc; i++) {
			wt_session = &conn->evict_workctx[i].session->iface;
			if (wt_session != NULL)
				WT_TRET(wt_session->close(wt_session, NULL));
		}
		__wt_free(session, conn->evict_workctx);
	}
	/*�ر�eviction server session*/
	if (conn->evict_session != NULL){
		wt_session = &conn->evict_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		conn->evict_session = NULL;
	}

	return ret;
}

/* workers thread�߳�ִ���� */
static WT_THREAD_RET __evict_worker(void* arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *worker;
	WT_SESSION_IMPL *session;

	worker = arg;
	session = worker->session;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN) && F_ISSET(worker, WT_EVICT_WORKER_RUN)){
		/*ִ��evict page����*/
		ret = __evict_lru_pages(session, 0);
		if (ret == WT_NOTFOUND) /*�ȴ�����ִ������*/
			WT_ERR(__wt_cond_wait(session, cache->evict_waiter_cond, 10000));
		else
			WT_ERR(ret);
	}
	
	WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "cache eviction worker exiting"));

	if (0){
err:	WT_PANIC_MSG(session, ret, "cache eviction worker error");
	}

	return WT_THREAD_RET_VALUE;
}

/*���evict������ִ�в�����ʾ*/
static int __evict_has_work(WT_SESSION_IMPL* session, uint32_t* flagsp)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint32_t flags;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;
	flags = 0;
	*flagsp = 0;

	/*evction server thread�Ѿ��������˳��ˣ�����Ҫ����*/
	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return 0;

	bytes_inuse = __wt_cache_bytes_inuse(cache);
	dirty_inuse = __wt_cache_dirty_inuse(cache);
	bytes_max = conn->cache_size;

	if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
		LF_SET(WT_EVICT_PASS_ALL);
	else if (dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100)
		/* Ignore clean pages unless the cache is too large */
		LF_SET(WT_EVICT_PASS_DIRTY);
	else if (F_ISSET(cache, WT_CACHE_WOULD_BLOCK)){
		/*
		* Evict pages with oldest generation (which would otherwise
		* block application threads) set regardless of whether we have
		* reached the eviction trigger.
		*/
		LF_SET(WT_EVICT_PASS_WOULD_BLOCK);
		F_CLR(cache, WT_CACHE_WOULD_BLOCK);
	}

	if (F_ISSET(cache, WT_CACHE_STUCK))
		LF_SET(WT_EVICT_PASS_AGGRESSIVE);

	*flagsp = flags;
	return 0;
}

/*evict page����*/
static int __evict_pass(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_EVICT_WORKER *worker;
	uint64_t pages_evicted;
	uint32_t flags;
	int loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Track whether pages are being evicted and progress is made. */
	pages_evicted = cache->pages_evict;

	/*����evict page����*/
	for (loop = 0;; loop++){
		/*����cache������clear walks��ʾ����ô�������ȫ����cache WALKS֮ǰ��Ҫ��һ�μ��*/
		if (F_ISSET(cache, WT_CACHE_CLEAR_WALKS)){
			F_CLR(cache, WT_CACHE_CLEAR_WALKS);
			WT_RET(__evict_clear_walks(session)); /*��̭connection����session�е�evict_ref��page*/
			WT_RET(__wt_cond_signal(session, cache->evict_waiter_cond));
		}

		/*���evict flags,����Ƿ���Ҫevict page���ڴ�,ȷ��evict����������*/
		WT_RET(__evict_has_work(session, &flags));
		if (flags == 0)
			break;

		if (loop > 10)
			LF_SET(WT_EVICT_PASS_AGGRESSIVE);

		/*����woker thread���д�����������̻���evict_workers����������������������*/
		if (LF_ISSET(WT_EVICT_PASS_ALL | WT_EVICT_PASS_DIRTY | WT_EVICT_PASS_WOULD_BLOCK) && conn->evict_workers < conn->evict_workers_max) {
			WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER, "Starting evict worker: %" PRIu32 "\n", conn->evict_workers));
			if (conn->evict_workers >= conn->evict_workers_alloc)
				WT_RET(__evict_workers_resize(session));

			worker = &conn->evict_workctx[conn->evict_workers++];
			F_SET(worker, WT_EVICT_WORKER_RUN);
			WT_RET(__wt_thread_create(session, &worker->tid, __evict_worker, worker));
		}

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER, "Eviction pass with: Max: %" PRIu64 " In use: %" PRIu64 " Dirty: %" PRIu64,
			conn->cache_size, cache->bytes_inmem, cache->bytes_dirty));

		WT_RET(__evict_lru_walk(session, flags));
		WT_RET(__evict_server_work(session));

		/*���������ɵĻ��ͼ��������pages_evicted == cache->pages_evict��ʾevict���ɹ�����ô������Ҫ��˯�ߵȴ����ȴ�page��evict����*/
		if (pages_evicted == cache->pages_evict){
			__wt_sleep(0, 1000 * (uint64_t)loop);
			if (loop == 100){
				if (!LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK)) {
					F_SET(cache, WT_CACHE_STUCK);
					WT_STAT_FAST_CONN_INCR(session, cache_eviction_slow);
					WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER, "unable to reach eviction goal"));
				}
				break;
			}
		}
		else{
			loop = 0;
			pages_evicted = cache->pages_evict;
		}
	}

	return 0;
}

/*���session��Ӧconnection������sessions��evict walk points,������session��btree��evict_ref��Ӧ��page����evict����*/
static int __evict_clear_walks(WT_SESSION_IMPL* session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_REF *ref;
	WT_SESSION_IMPL *s;
	u_int i, session_cnt;

	conn = S2C(session);
	cache = conn->cache;

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i){
		if (!s->active || !F_ISSET(s, WT_SESSION_CLEAR_EVICT_WALK))
			continue;

		if (s->dhandle == cache->evict_file_next)
			cache->evict_file_next = NULL;

		session->dhandle = s->dhandle;
		btree = s->dhandle->handle;
		ref = btree->evict_ref;
		if (ret != NULL){
			btree->evict_ref = NULL;
			WT_TRET(__wt_page_release(session, ref, 0));
		}
		session->dhandle = NULL;
	}

	return ret;
}

/*�����ǰsession��Ӧbtree��evict point*/
static int __evict_tree_walk_clear(WT_SESSION_IMPL* session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	F_SET(session, WT_SESSION_CLEAR_EVICT_WALK);
	/*�ȴ�btree->evict_ref��page��evicted��Ȼ���˳�*/
	while (btree->evict_ref != NULL && ret == 0){
		F_SET(cache, WT_CACHE_CLEAR_WALKS);
		ret = __wt_cond_wait(session, cache->evict_waiter_cond, 100000);
	}

	F_CLR(session, WT_SESSION_CLEAR_EVICT_WALK);

	return ret;
}

/*��ref��Ӧ��page��evicted��������page evict���ڴ�*/
int __wt_evict_page(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_ISOLATION saved_iso;

	/*
	* We have to take care when evicting pages not to write a change that:
	*  (a) is not yet committed; or
	*  (b) is committed more recently than an in-progress checkpoint.
	*
	* We handle both of these cases by setting up the transaction context
	* before evicting, using a special "eviction" isolation level, where
	* only globally visible updates can be evicted.
	*/

	__wt_txn_update_oldest(session);
	txn = &session->txn;
	saved_iso = txn->isolation;
	txn->isolation = TXN_ISO_EVICTION;

	WT_ASSERT(session, !F_ISSET(txn, TXN_HAS_ID) || !__wt_txn_visible(session, txn->id));
	/*�ڽ���evict page֮ǰ����Ҫ��session������������TXN_ISO_EVICTION*/
	ret = __wt_evict(session, ref, 0);
	txn->isolation = saved_iso;

	return ret;
}

/*���session��Ӧbtree�ϵ�evict ref��page,����btree��evict lru list�е�entryɾ��,���������Ƕ�ռʽ������*/
int __wt_evict_file_exclusive_on(WT_SESSION_IMPL* session, int* evict_resetp)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	u_int i, elem;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*����btree������evict page,����meta btree����btree�Ѿ����뱾���������沿��, ֱ�ӷ���*/
	if (F_ISSET(btree, WT_BTREE_NO_EVICTION)){
		*evict_resetp = 0;
		return 0;
	}

	*evict_resetp = 1;

	__wt_spin_lock(session, &cache->evict_walk_lock);
	F_SET(btree, WT_BTREE_NO_EVICTION); /*���������ʾ��Ϊ�˶�ռbtree evict�Ĳ���Ȩ*/
	__wt_spin_unlock(session, &cache->evict_walk_lock);

	/*���session��btree�ϵ�evict ref,���������*/
	WT_RET(__evict_tree_walk_clear(session));

	__wt_spin_lock(session, &cache->evict_lock);
	/*��btree��������evict lru list�е�evict entry���*/
	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++){
		if (evict->btree == btree)
			__evict_list_clear(session, evict);
	}

	__wt_spin_unlock(session, &cache->evict_lock);
	/*
	* We have disabled further eviction: wait for concurrent LRU eviction
	* activity to drain.
	* �ȴ������߳����evict�������� spin_wait
	*/
	while (btree->evict_busy > 0)
		__wt_yield();

	return 0;
}

/*�ͷ�btree�Ķ�ռevict����Ȩ*/
void __wt_evict_file_exclusive_off(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	WT_ASSERT(session, btree->evict_ref == NULL);
	/*����ط�Ϊʲô����cache->evict_lock ??*/
	F_CLR(btree, WT_BTREE_NO_EVICTION);
}

/* ��lru queue��evict���е�evict pages */
static int __evict_lru_pages(WT_SESSION_IMPL *session, int is_server)
{
	WT_DECL_RET;
	while ((ret = __wt_evict_lru_page(session, is_server)) == 0 || ret == EBUSY)
		;

	return ret;
}

/*����evict�����page��ӵ�LRU QUEUE��*/
static int __evict_lru_walk(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	uint64_t cutoff;
	uint32_t candidates, entries, i;

	cache = S2C(session)->cache;

	/*Ϊevict������ø�����Ҫ��evict��page*/
	if ((ret = __evict_walk(session, flags)) != 0)
		return (ret == EBUSY ? 0 : ret);

	__wt_spin_lock(session, &cache->evict_lock);
	entries = cache->evict_entries;
	/*��evict list����entry->read_gen�Ӵ�С����*/
	qsort(cache->evict, entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);
	/*��λ������û�б�evict��read gen��С��evict entry*/
	while (entries > 0 && cache->evict[entries - 1].ref == NULL)
		--entries;

	cache->evict_entries = entries;
	/*evict lru list��û��evict entry,ֱ�ӷ���*/
	if (entries == 0){
		cache->evict_candidates = 0;
		cache->evict_current = NULL;
		__wt_spin_unlock(session, &cache->evict_lock);

		return 0;
	}

	/*һ���и�evict entry����Ч�ģ����ҽ���evict*/
	WT_ASSERT(session, cache->evict[0].ref != NULL);
	if (LF_ISSET(WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK)){
		/*
		* Take all candidates if we only gathered pages with an oldest
		* read generation set.
		*/
		cache->evict_candidates = entries;
	}
	else{
		/* Find the bottom 25% of read generations. */
		cutoff = (3 * __evict_read_gen(&cache->evict[0]) + __evict_read_gen(&cache->evict[entries - 1])) / 4;
		/*ȷ��evict_candidates�߽�*/
		for (candidates = 1 + entries / 10; candidates < entries / 2; candidates++){
			if (__evict_read_gen(&cache->evict[candidates]) > cutoff)
				break;
		}

		cache->evict_candidates = candidates;
	}

	/* If we have more than the minimum number of entries, clear them. */
	if (cache->evict_entries > WT_EVICT_WALK_BASE) {
		for (i = WT_EVICT_WALK_BASE, evict = cache->evict + i; i < cache->evict_entries; i++, evict++)
			__evict_list_clear(session, evict);
		cache->evict_entries = WT_EVICT_WALK_BASE;
	}

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->evict_lock);
	/*����һ��evict worker���д���*/
	WT_RET(__wt_cond_signal(session, cache->evict_waiter_cond));

	return 0;
}

/*����Ƿ�Ӧevict worker�ڹ����������, ��lru queue��evict page*/
static int __evict_server_work(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	if (S2C(session)->evict_workers > 1) {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_server_not_evicting);
		/*
		* If there are candidates queued, give other threads a chance
		* to access them before gathering more.
		* �������sched_yield��Ϊ����CPU�ܼ�����ʱ��evict server �ó�CPU��Դ��
		* evict work threadִ��evict��������ֹevict queue�жѻ�����ȴ�evict��ʵ��
		*/
		if (cache->evict_candidates > 10 && cache->evict_current != NULL)
			__wt_yield();
	}
	else /*û��evict worker�̣߳�ֱ����server�߳̽���evict����*/
		WT_RET_NOTFOUND_OK(__evict_lru_pages(session, 1));

	return 0;
}

/*ɨ������connection�д򿪵�btree�����ļ�������evict��btree page����evict lru���е���*/
static int __evict_walk(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, prev_slot, retries, slot, start_slot, spins;
	int incr, dhandle_locked;
	WT_DECL_SPINLOCK_ID(id); 

	conn = S2C(session);
	cache = S2C(session)->cache;
	dhandle = NULL;
	incr = dhandle_locked = 0;
	retries = 0;

	/*��cache->read_gen�Լ�*/
	__wt_cache_read_gen_incr(session);

	/*
	* Update the oldest ID: we use it to decide whether pages are
	* candidates for eviction.  Without this, if all threads are blocked
	* after a long-running transaction (such as a checkpoint) completes,
	* we may never start evicting again.
	*/
	__wt_txn_update_oldest(session);

	if (cache->evict_current == NULL)
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_empty);
	else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_not_empty);

	/*
	 * Set the starting slot in the queue and the maximum pages added
	 * per walk.
	 */
	start_slot = slot = cache->evict_entries;
	max_entries = slot + WT_EVICT_WALK_INCR;

retry:
	while (slot < max_entries && ret == 0){
		/*�����߳��Ѿ�����˶�session��Ӧ��btree��walk point���,ֱ���˳�����*/
		if (F_ISSET(cache, WT_CACHE_CLEAR_WALKS))
			break;

		/*���conn->dhandle_lock����Ȩ��*/
		if (!dhandle_locked) {
			for (spins = 0; (ret = __wt_spin_trylock(session, &conn->dhandle_lock, &id)) == EBUSY && !F_ISSET(cache, WT_CACHE_CLEAR_WALKS); spins++) {
				if (spins < 1000)
					__wt_yield();
				else
					__wt_sleep(0, 1000);
			}
			if (ret != 0)
				break;
			dhandle_locked = 1; /*��ʾ�Ѿ����dhandle_lock���´�ѭ������ȥ��dhandle_lock*/
		}

		/*���һ��dhandle*/
		if (dhandle == NULL)
			dhandle = SLIST_FIRST(&conn->dhlh);
		else{
			if (incr){
				WT_ASSERT(session, dhandle->session_inuse > 0);
				(void)WT_ATOMIC_SUB4(dhandle->session_inuse, 1);
				incr = 0;
			}
			dhandle = SLIST_NEXT(dhandle, l);
		}

		/* If we reach the end of the list, we're done. */
		if (dhandle == NULL)
			break;

		/* Ignore non-file handles, or handles that aren't open. */
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") || !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		if (cache->evict_file_next != NULL && cache->evict_file_next != dhandle)
			continue;
		cache->evict_file_next = NULL;

		/* Skip files that don't allow eviction. */
		btree = dhandle->handle;
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
			continue;
		/*
		* Also skip files that are checkpointing or configured to
		* stick in cache until we get aggressive.
		* ���ڽ���checkpoint��btree������ͨ��evict����
		*/
		if ((btree->checkpointing || btree->evict_priority != 0) && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			continue;

		/* Skip files if we have used all available hazard pointers. */
		if (btree->evict_ref == NULL && session->nhazard >= conn->hazard_max - WT_MIN(conn->hazard_max / 2, 10))
			continue;
		/*
		* If we are filling the queue, skip files that haven't been
		* useful in the past.
		*/
		if (btree->evict_walk_period != 0 && cache->evict_entries >= WT_EVICT_WALK_INCR && btree->evict_walk_skips++ < btree->evict_walk_period)
			continue;
		btree->evict_walk_skips = 0;
		prev_slot = slot;

		(void)WT_ATOMIC_ADD4(dhandle->session_inuse, 1);
		incr = 1;
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = 0;

		/*���һ��dhandle��Ӧ��BTREE����*/
		__wt_spin_lock(session, &cache->evict_walk_lock);
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
			WT_WITH_DHANDLE(session, dhandle, ret = __evict_walk_file(session, &slot, flags));
			WT_ASSERT(session, session->split_gen == 0);
		}
		__wt_spin_unlock(session, &cache->evict_walk_lock);

		/*����walk��û���������BTREE��ҳ��Ϊ���������ô�´μ���evict walk�ĸ��ʣ���Ϊwalk btree�Ǻķ���Դ��*/
		if (slot == prev_slot)
			btree->evict_walk_period = WT_MIN(WT_MAX(1, 2 * btree->evict_walk_period), 100);
		else
			btree->evict_walk_period = 0;
	}

	if (incr){
		WT_ASSERT(session, dhandle->session_inuse > 0);
		(void)WT_ATOMIC_SUB4(dhandle->session_inuse, 1);
		incr = 0;
	}

	/*�м�break�����dhandle_lockû���ͷţ�����������ͷ�*/
	if (dhandle_locked){
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = 0;
	}

	/*
	* Walk the list of files a few times if we don't find enough pages.
	* Try two passes through all the files, give up when we have some
	* candidates and we aren't finding more.  Take care not to skip files
	* on subsequent passes.
	*/
	if (!F_ISSET(cache, WT_CACHE_CLEAR_WALKS) && ret == 0 && slot < max_entries 
		&& (retries < 2  || (!LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK) && retries < 10 && (slot == cache->evict_entries || slot > start_slot)))) {
		cache->evict_file_next = NULL;
		start_slot = slot;
		++retries;
		goto retry;
	}

	cache->evict_file_next = dhandle;
	cache->evict_entries = slot;

	return ret;
}

/*��һ��page���õ�entry lru list����*/
static void __evict_init_candidate(WT_SESSION_IMPL* session, WT_EVICT_ENTRY* evict, WT_REF* ref)
{
	WT_CACHE *cache;
	u_int slot;

	cache = S2C(session)->cache;

	slot = (u_int)(evict - cache->evict);
	if (slot >= cache->evict_max)
		cache->evict_max = slot + 1;

	if (evict->ref != NULL)
		__evict_list_clear(session, evict);

	evict->ref = ref;
	evict->btree = S2BT(session);
	/* Mark the page on the list */
	F_SET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU);
}

/*����session��Ӧbtree��evict_ref����btree���ѷ���evict������page��ӵ�evict lru list����*/
static int __evict_walk_file(WT_SESSION_IMPL* session, u_int* slotp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	uint64_t pages_walked;
	uint32_t walk_flags;
	int enough, internal_pages, modified, restarts;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*���evict 10��page*/
	start = cache->evict + *slotp;
	end = WT_MIN(start + WT_EVICT_WALK_PER_FILE, cache->evict + cache->evict_slots);

	enough = internal_pages = restarts = 0;

	walk_flags = WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT;

	/*����btree�����evict page*/
	for (evict = start, pages_walked = 0; evict < end && !enough && (ret == 0 || ret == WT_NOTFOUND);
		ret = __wt_tree_walk(session, &btree->evict_ref, &pages_walked, walk_flags)){
		enough = (pages_walked > WT_EVICT_MAX_PER_FILE);
		ref = btree->evict_ref;
		if (ret == NULL){
			if (++restarts == 2 || enough)
				break;
			continue;
		}
		/*root page���ܱ�evict*/
		if (__wt_is_root(ref))
			continue;

		page = ref->page;
		modified = __wt_page_is_modified(page);

		/*
		* Use the EVICT_LRU flag to avoid putting pages onto the list
		* multiple times.
		*/
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		if (__wt_page_is_empty(page))
			goto fast;
		
		/*������ҳ������evict������ָ��evict��ҳ���ݣ������������page����*/
		if (!modified && LF_ISSET(WT_EVICT_PASS_DIRTY))
			continue;

		/*
		* If we are only trickling out pages marked for definite
		* eviction, skip anything that isn't marked.
		*/
		if (LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK) && page->read_gen != WT_READGEN_OLDEST)
			continue;

		/* Limit internal pages to 50% unless we get aggressive. */
		if (WT_PAGE_IS_INTERNAL(page) && ++internal_pages > WT_EVICT_WALK_PER_FILE / 2 && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			continue;

		/*
		* If this page has never been considered for eviction,
		* set its read generation to a little bit in the
		* future and move on, give readers a chance to start
		* updating the read generation.
		* page������̭��������read_gen���õĺ�cacheһֱ����ֹ��̭�ж�
		*/
		if (page->read_gen == WT_READGEN_NOTSET){
			page->read_gen = __wt_cache_read_gen_set(session);
			continue;
		}

fast:
		/*����page������evict����������*/
		if (!__wt_page_can_evict(session, page, 1))
			continue;

		/*
		* If the page is clean but has modifications that appear too
		* new to evict, skip it.
		*
		* Note: take care with ordering: if we detected that the page
		* is modified above, we expect mod != NULL.
		*/
		mod = page->modify;
		if (!modified && mod != NULL && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK) && !__wt_txn_visible_all(session, mod->rec_max_txn))
			continue;

		/*
		* If the oldest transaction hasn't changed since the last time
		* this page was written, it's unlikely that we can make
		* progress.  Similarly, if the most recent update on the page
		* is not yet globally visible, eviction will fail.  These
		* heuristics attempt to avoid repeated attempts to evict the
		* same page.
		*
		* That said, if eviction is stuck, or we are helping with
		* forced eviction, try anyway: maybe a transaction that was
		* running last time we wrote the page has since rolled back,
		* or we can help get the checkpoint completed sooner.
		*/
		if (modified && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK) &&
			(mod->disk_snap_min == S2C(session)->txn_global.oldest_id || !__wt_txn_visible_all(session, mod->update_txn)))
			continue;

		/*��evict page���뵽evict lru list����*/
		WT_ASSERT(session, evict->ref == NULL);
		__evict_init_candidate(session, evict, ref);
		++evict;

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER, "select: %p, size %" PRIu64, page, page->memory_footprint));
	}

	/*
	* If we happen to end up on the root page, clear it.  We have to track
	* hazard pointers, and the root page complicates that calculation.
	*
	* Also clear the walk if we land on a page requiring forced eviction.
	* The eviction server may go to sleep, and we want this page evicted
	* as quickly as possible.
	*/
	ref = btree->evict_ref;
	if (ref != NULL && (__wt_ref_is_root(ref) || ref->page->read_gen == WT_READGEN_OLDEST)){
		btree->evict_ref = NULL;
		__wt_page_release(session, ref, 0);
	}

	if (ret == WT_NOTFOUND)
		ret = 0;

	*slotp += (u_int)(evict - start);
	WT_STAT_FAST_CONN_INCRV(session, cache_eviction_walk, pages_walked);

	return ret;
}

/*��evict queue��ȡһ��evict page��ref*/
static int _evict_get_ref(WT_SESSION_IMPL *session, int is_server, WT_BTREE **btreep, WT_REF **refp)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t candidates;
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	cache = S2C(session)->cache;
	*btreep = NULL;
	*refp = NULL;

	/*��cache->evict_current��Чʱ�����evict_lock��������������spin wait*/
	for (;;){
		if (cache->evict_current == NULL)
			return WT_NOTFOUND;
		if (__wt_spin_trylock(session, &cache->evict_lock, &id) == 0)
			break;

		__wt_yield();
	}

	/*�����evict server thread�Ļ�����evict out һ��������page*/
	candidates = cache->evict_candidates;
	if (is_server && candidates > 1)
		candidates /= 2;

	/*��evict queue�л�ȡpage*/
	while ((evict = cache->evict_current) != NULL && evict < cache->evict + candidates && evict->ref != NULL){
		WT_ASSERT(session, evict->btree != NULL);
		++cache->evict_current;

		/*
		* Lock the page while holding the eviction mutex to prevent
		* multiple attempts to evict it.  For pages that are already
		* being evicted, this operation will fail and we will move on.
		* ���Խ�ref��Ӧ��page����lock,������ɹ���Ҳ�����page�Ѿ���evcit,
		* ֱ�ӽ����page��evict lru list�������ȥ
		*/
		if (!WT_ATOMIC_CAS4(evict->ref->state, WT_REF_MEM, WT_REF_LOCKED)){
			__evict_list_clear(session, evict);
			continue;
		}

		/*����evict busy����������ֹbtree handle���ر�*/
		(void*)WT_ATOMIC_ADD4(evict->btree->evict_busy, 1);

		/*�Ѿ����һ��evict page*/
		*btreep = evict->btree;
		refp = evict->ref;
		__evict_list_clear(session, evict);

		break;
	}
	/*evict queueû��evict page,ֱ�ӽ�evict_current���NULL*/
	if (evict >= cache->evict + cache->evict_candidates)
		cache->evict_current = NULL;

	__wt_spin_unlock(session, &cache->evict_lock);

	return ((*refp == NULL) ? WT_NOTFOUND : 0);
}

/*��evict queue�л�ȡһ��page,��evict���page*/
int __wt_evict_lru_page(WT_SESSION_IMPL* session, int is_server)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;

	WT_RET(_evict_get_ref(session, is_server, &btree, &ref));
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);
	/*
	 * An internal session flags either the server itself or an eviction
	 * worker thread.
	 */
	if (F_ISSET(session, WT_SESSION_INTERNAL)){
		if (is_server)
			WT_STAT_FAST_CONN_INCR(session, cache_eviction_server_evicting);
		else
			WT_STAT_FAST_CONN_INCR(session, cache_eviction_worker_evicting);
	}
	else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_app);

	/*
	* In case something goes wrong, don't pick the same set of pages every
	* time.
	*
	* We used to bump the page's read generation only if eviction failed,
	* but that isn't safe: at that point, eviction has already unlocked
	* the page and some other thread may have evicted it by the time we
	* look at it.
	*/
	page = ref->page;
	if (page->read_gen != WT_READGEN_OLDEST)
		page->read_gen = __wt_cache_read_gen_set(session);

	/*��page����evict����*/
	WT_WITH_BTREE(session, btree, ret = __wt_evict_page(session, ref));
	/*���evict������evict_busy�����ݼ�*/
	(void)WT_ATOMIC_SUB4(btree->evict_busy, 1);

	WT_RET(ret);

	cache = S2C(session)->cache;
	if (F_ISSET(cache, WT_CACHE_STUCK))
		F_CLR(cache, WT_CACHE_STUCK);

	return ret;
}

int __wt_cache_wait(WT_SESSION_IMPL* session, int full)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	int busy, count;

	cache = S2C(session)->cache;

	/*
	* If the current transaction is keeping the oldest ID pinned, it is in
	* the middle of an operation.	This may prevent the oldest ID from
	* moving forward, leading to deadlock, so only evict what we can.
	* Otherwise, we are at a transaction boundary and we can work harder
	* to make sure there is free space in the cache.
	* ���û����ȫ��⣬�������й�ϵ����Ҫ��ϸ����
	*/
	txn_global = &S2C(session)->txn_global;
	txn_state = &(txn_global->states[session->id]);
	busy = (txn_state->id != WT_TXN_NONE || session->nhazard > 0 || (txn_state->snap_min != WT_TXN_NONE && txn_global->current != txn_global->oldest_id));
	if (busy && full < 100)
		return 0;

	count = busy ? 1 : 10;

	for (;;){
		/*
		* A pathological case: if we're the oldest transaction in the
		* system and the eviction server is stuck trying to find space,
		* abort the transaction to give up all hazard pointers before
		* trying again.
		* ���evict�Ǹ������������ô�ᵼ��ϵͳ����������������ȴ��������
		* ���stuck״̬����ô������evction server�ٴγ���֮ǰ�������������
		* ��ֹϵͳ�ȴ�̫��
		*/
		if (F_ISSET(cache, WT_CACHE_STUCK) && __wt_txn_am_oldest(session)) {
			F_CLR(cache, WT_CACHE_STUCK);
			WT_STAT_FAST_CONN_INCR(session, txn_fail_cache);
			return WT_ROLLBACK;
		}

		/*��evict queue�л�ȡһ��evict page����evict����*/
		ret = __wt_evict_lru_page(session, 0);
		switch (ret){
		case 0: /*�ɹ��ˣ�������һ��*/
			if (--count == 0)
				return (0);
			break;

		case EBUSY:
			continue;

		case WT_NOTFOUND:
			break;

		default:
			return ret;
		}
	}
}

