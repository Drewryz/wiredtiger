
#include "wt_internal.h"

/* Threshold when a connection is allocated more cache */
#define	WT_CACHE_POOL_BUMP_THRESHOLD	6
/* Threshold when a connection is allocated less cache */
#define	WT_CACHE_POOL_REDUCE_THRESHOLD	2
/* Balancing passes after a bump before a connection is a candidate. */
#define	WT_CACHE_POOL_BUMP_SKIPS	10
/* Balancing passes after a reduction before a connection is a candidate. */
#define	WT_CACHE_POOL_REDUCE_SKIPS	5

static int __cache_pool_adjust(WT_SESSION_IMPL*, uint64_t, uint64_t, int*);
static int __cache_pool_assess(WT_SESSION_IMPL*, uint64_t*);
static int __cache_pool_balance(WT_SESSION_IMPL*);

/*����connection��cache pool,��������һ�ε����⺯�����ᴴ��cache pool*/
int __wt_cache_pool_config(WT_SESSION_IMPL* session, const char* cfg)
{
	WT_CACHE_POOL *cp;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn, *entry;
	WT_DECL_RET;
	char *pool_name;
	int created, updating;
	uint64_t chunk, reserve, size, used_cache;

	conn = S2C(session);
	created = updating = 0;
	pool_name = NULL;
	cp = NULL;
	size = 0;

	if (I_ISSET(conn, WT_CONN_CACHE_POOL))
		updating = 1;
	else{
		/*��ȡshared_cache.name*/
		WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
		if (cval.len == 0){
			/*��ʾ�û����������shared_cache.size��������shared_cache.name*/
			if (__wt_config_gets(session, &cfg[1], "shared_cache.size", &cval) != WT_NOTFOUND)
				WT_RET_MSG(session, EINVAL, "Shared cache configuration requires a pool name");
			return 0;
		}

		/*���cache_size�Ƿ���ڣ�������ڣ���ʾ�����г�ͻ,��Ϊcache_size����shared_cache��ʽì�ܵ�*/
		if (__wt_config_gets(session, &cfg[1], "cache_size", &cval) != WT_NOTFOUND)
			WT_RET_MSG(session, EINVAL, "Only one of cache_size and shared_cache can be in the configuration");
		/*��cache name���Ƹ�pool name*/
		WT_RET(__wt_strndup(session, cval.str, cval.len, &pool_name));
	}

	__wt_spin_lock(session, &__wt_process.spinlock);
	if (__wt_process.cache_pool == NULL){ /*����һ��cache pool*/
		WT_ASSERT(session, !updating);
		
		WT_ERR(__wt_calloc_one(session, &cp));
		created = 1;
		cp->name = pool_name;
		pool_name = NULL;

		TAILQ_INIT(&cp->cache_pool_qh);
		WT_ERR(__wt_spin_init(session, &cp->cache_pool_lock, "cache shared pool"));
		WT_ERR(__wt_cond_alloc(session,"cache pool server", 0, &cp->cache_pool_cond));

		__wt_process.cache_pool = cp;
		WT_ERR(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Created cache pool %s", cp->name));
	}
	else if (!updating && !WT_STRING_MATCH(__wt_process.cache_pool->name, pool_name, strlen(pool_name))){ /*�ظ�����cache pool,��ʾ����*/
		/* Only a single cache pool is supported. */
		WT_ERR_MSG(session, WT_ERROR, "Attempting to join a cache pool that does not exist: %s", pool_name);
	}

	cp = __wt_process.cache_pool;
	/*����cache pool�����ü�����˵��cache pool�ոմ���,��ʾ����Ч��*/
	if (!updating)
		++cp->refs;

	/*��ȡshared_cache.size��cache.chunk*/
	if (!created){
		if (__wt_config_gets(session, &cfg[1], "shared_cache.size", &cval) == 0 && cval.val != 0)
			size = (uint64_t)cval.val;
		else
			size = cp->size;
		if (__wt_config_gets(session, &cfg[1], "shared_cache.chunk", &cval) == 0 && cval.val != 0)
			chunk = (uint64_t)cval.val;
		else
			chunk = cp->chunk;
	}
	else{
		WT_ERR(__wt_config_gets(session, cfg, "shared_cache.size", &cval));
		WT_ASSERT(session, cval.val != 0);
		size = (uint64_t)cval.val;
		WT_ERR(__wt_config_gets(session, cfg, "shared_cache.chunk", &cval));
		WT_ASSERT(session, cval.val != 0);
		chunk = (uint64_t)cval.val;
	}
	/*
	* Retrieve the reserve size here for validation of configuration.
	* Don't save it yet since the connections cache is not created if
	* we are opening. Cache configuration is responsible for saving the
	* setting.
	* The different conditions when reserved size are set are:
	*  - It's part of the users configuration - use that value.
	*  - We are reconfiguring - keep the previous value.
	*  - We are joining a cache pool for the first time (including
	*  creating the pool) - use the chunk size; that's the default.
	*/
	if (__wt_config_gets(session, &cfg[1], "shared_cache.reserve", &cval) == 0 && cval.val != 0)
		reserve = (uint64_t)cval.val;
	else if (updating)
		reserve = conn->cache->cp_reserved;
	else
		reserve = chunk;

	/*����cache pool�Ѿ����˵�cache����Ŀռ�*/
	used_cache = 0;
	if (!created){
		TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq)
			used_cache += entry->cache->cp_reserved;
	}
	/*�����Ѿ�ʹ���˵Ŀռ�*/
	if (updating)
		used_cache -= conn->cache->cp_reserved;
	/*�ж��޸ĺ�Ŀռ����ܿռ�Ĺ�ϵ���޸ĺ�Ŀռ� + �Ѿ�ʹ�õĿռ��ǲ��ܴ������õ�cache pool�Ŀռ��С*/
	if (used_cache + reserve > size)
		WT_ERR_MSG(session, EINVAL,"Shared cache unable to accommodate this configuration. Shared cache size: %" PRIu64 ", requested min: %" PRIu64, 
		size, used_cache + reserve);

	/* The configuration is verified - it's safe to update the pool. */
	cp->size = size;
	cp->chunk = chunk;
	conn->cache->cp_reserved = reserve;

	/* Wake up the cache pool server so any changes are noticed. */
	if (updating)
		WT_ERR(__wt_cond_signal(session, __wt_process.cache_pool->cache_pool_cond)); /*cache pool��������Ϣ�����˸ı䣬��Ҫ��������balance*/

	WT_ERR(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Configured cache pool %s. Size: %" PRIu64 ", chunk size: %" PRIu64, cp->name, cp->size, cp->chunk));

	F_SET(conn, WT_CONN_CACHE_POOL);

err:
	__wt_spin_unlock(session, &__wt_process.spinlock);
	if (!updating)
		__wt_free(session, pool_name);

	if (!ret != 0 && created){
		__wt_free(session, cp->name);
		WT_TRET(__wt_cond_destroy(session, &cp->cache_pool_cond));
		__wt_free(session, cp);
	}

	return ret;
}

/*��һ��connection cache������connection���뵽cache pool�У�����connection cache��Ӧ��cache pool thread*/
int __wt_conn_cache_pool_open(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;
	WT_CACHE_POOL *cp;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	cache = conn->cache;
	cp = __wt_process.cache_pool;

	/*����һ��session��������cache pool thread�Ĳ���session*/
	if ((ret = __wt_open_internal_session(conn, "cache-pool", 0, 0, &cache->cp_session)) != 0)
		WT_RET_MSG(NULL, ret, "Failed to create session for cache pool");

	/*��conn���뵽cache pool��connection list��*/
	__wt_spin_lock(session, &cp->cache_pool_lock);
	TAILQ_INSERT_TAIL(&cp->cache_pool_qh, conn, cpq);
	__wt_spin_unlock(session, &cp->cache_pool_lock);

	WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Added %s to cache pool %s", conn->home, cp->name));

	/*
	* Each connection participating in the cache pool starts a manager
	* thread. Only one manager is active at a time, but having a thread
	* in each connection saves having a complex election process when
	* the active connection shuts down.
	*/
	F_SET_ATOMIC(cp, WT_CACHE_POOL_ACTIVE);
	F_SET(cache, WT_CACHE_POOL_RUN);
	WT_RET(__wt_thread_create(session, &cache->cp_tid, __wt_cache_pool_server, cache->cp_session));

	/* Wake up the cache pool server to get our initial chunk. */
	WT_RET(__wt_cond_signal(session, cp->cache_pool_cond));

	return 0;
}

/*��cache pool�г���connection cache*/
int __wt_conn_cache_pool_destroy(WT_SESSION_IMPL* session)
{
	WT_CACHE *cache;
	WT_CACHE_POOL *cp;
	WT_CONNECTION_IMPL *conn, *entry;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	int cp_locked, found;

	conn = S2C(session);
	cache = conn->cache;
	cp_locked = found = 0;
	cp = __wt_process.cache_pool;

	/*���Ҷ�Ӧ��conn�����Ƿ���cache pool��connect list����*/
	__wt_spin_lock(session, &cp->cache_pool_lock);
	cp_locked = 1;
	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq){
		if (entry == conn) {
			found = 1;
			break;
		}
	}

	if (found){
		/*��cache pool��connection list����ɾ������Ӧ��connection*/
		WT_TRET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Removing %s from cache pool", entry->home));
		TAILQ_REMOVE(&cp->cache_pool_qh, entry, cpq);

		/* Give the connection's resources back to the pool. */
		WT_ASSERT(session, cp->currently_used >= conn->cache_size);
		cp->currently_used -= conn->cache_size;

		__wt_spin_unlock(session, &cp->cache_pool_lock);
		cp_locked = 0;

		/*ֹͣconnection��Ӧ��cache pool thread*/
		F_CLR(cache, WT_CACHE_POOL_RUN);
		WT_TRET(__wt_cond_signal(session, cp->cache_pool_cond));
		WT_TRET(__wt_thread_join(session, cache->cp_tid));

		wt_session = &cache->cp_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		__wt_spin_lock(session, &cp->cache_pool_lock);
		cp_locked = 1;
	}

	/*����������ֱ�ӷ���*/
	if (cp->refs < 1){
		if (cp_locked)
			__wt_spin_unlock(session, &cp->cache_pool_lock);
		return 0;
	}

	/*�ж�cache pool�Ƿ����������ط����ã�����ǣ����WT_CACHE_POOL_ACTIVE״̬*/
	if (--cp->refs == 0) {
		WT_ASSERT(session, TAILQ_EMPTY(&cp->cache_pool_qh));
		F_CLR_ATOMIC(cp, WT_CACHE_POOL_ACTIVE);
	}

	/*�ͷŵ����Ǽ���״̬��cache pool����*/
	if (!F_ISSET_ATOMIC(cp, WT_CACHE_POOL_ACTIVE)) {
		WT_TRET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Destroying cache pool"));
		__wt_spin_lock(session, &__wt_process.spinlock);
		/*
		* We have been holding the pool lock - no connections could
		* have been added.
		*/
		WT_ASSERT(session, cp == __wt_process.cache_pool && TAILQ_EMPTY(&cp->cache_pool_qh));
		__wt_process.cache_pool = NULL;
		__wt_spin_unlock(session, &__wt_process.spinlock);
		__wt_spin_unlock(session, &cp->cache_pool_lock);
		cp_locked = 0;

		/* Now free the pool. */
		__wt_free(session, cp->name);

		__wt_spin_destroy(session, &cp->cache_pool_lock);
		WT_TRET(__wt_cond_destroy(session, &cp->cache_pool_cond));
		__wt_free(session, cp);
	}

	if (cp_locked) {
		__wt_spin_unlock(session, &cp->cache_pool_lock);

		/* Notify other participants if we were managing */
		if (F_ISSET(cache, WT_CACHE_POOL_MANAGER)) {
			F_CLR_ATOMIC(cp, WT_CACHE_POOL_MANAGED);
			WT_TRET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Shutting down shared cache manager connection"));
		}
	}

	return ret;
}

/*��cache pool�����е�connection��cache size����������������ȡ�ʺ��ڴ�cache��һ���ȶ���״̬*/
static int __cache_pool_balance(WT_SESSION_IMPL* session)
{
	WT_CACHE_POOL *cp;
	WT_DECL_RET;
	int adjusted;
	uint64_t bump_threshold, highest;

	cp = __wt_process.cache_pool;
	adjusted = 0;
	highest = 0;

	__wt_spin_lock(NULL, &cp->cache_pool_lock);

	/* If the queue is empty there is nothing to do. */
	if (TAILQ_FIRST(&cp->cache_pool_qh) == NULL)
		goto err;

	WT_ERR(__cache_pool_assess(session, &highest));
	bump_threshold = WT_CACHE_POOL_BUMP_THRESHOLD;

	/*
	* Actively attempt to:
	* - Reduce the amount allocated, if we are over the budget
	* - Increase the amount used if there is capacity and any pressure.
	*/
	for (bump_threshold = WT_CACHE_POOL_BUMP_THRESHOLD;
		F_ISSET_ATOMIC(cp, WT_CACHE_POOL_ACTIVE) && F_ISSET(S2C(session)->cache, WT_CACHE_POOL_RUN);){
		WT_ERR(__cache_pool_adjust(session, highest, bump_threshold, &adjusted));

		if (cp->currently_used <= cp->size && !adjusted)
			break;

		if (bump_threshold > 0)
			--bump_threshold;
	}

err:
	__wt_spin_lock(NULL, &cp->cache_pool_lock);
	return ret;
}

/*����cache pool��ʹ��Ƶ�ʣ���ͨ��ͳ�Ƶ���cache��λʱ������߶�ȡ�ֽ���������connection list��connection�����õ�*/
static int __cache_pool_assess(WT_SESSION_IMPL* session, uint64_t* phighest)
{
	WT_CACHE_POOL* cp;
	WT_CACHE* cache;
	WT_CONNECTION_IMPL* entry;
	uint64_t entries, highest, new;

	cp = __wt_process.cache_pool;
	entries = highest = 0;

	/* Generate read pressure information. */
	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq) {
		if (entry->cache_size == 0 || entry->cache == NULL)
			continue;
		cache = entry->cache;
		++entries;
		new = cache->bytes_read;

		/* Handle wrapping of eviction requests. */
		if (new >= cache->cp_saved_read)
			cache->cp_current_read = new - cache->cp_saved_read; /*��λʱ���ڶ�ȡ����,���ÿ�������洢��cp_saved_read,��ô����Ӧ���� new - cp_saved_read*/
		else
			cache->cp_current_read = new;

		cache->cp_saved_read = new;
		if (cache->cp_current_read > highest)
			highest = cache->cp_current_read;
	}

	WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Highest eviction count: %" PRIu64 ", entries: %" PRIu64, highest, entries));
	highest = highest / (entries + 1);
	++highest; /* Avoid divide by zero. */

	return 0;
}

static int __cache_pool_adjust(WT_SESSION_IMPL *session, uint64_t highest, uint64_t bump_threshold, int *adjustedp)
{
	WT_CACHE_POOL *cp;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *entry;
	uint64_t adjusted, reserved, read_pressure;
	int force, grew;

	*adjustedp = 0;
	cp = __wt_process.cache_pool;
	force = (cp->currently_used > cp->size); /*�жϵ�ǰcache pool���ڴ������Ƿ񳬹���cache pool���õ�����*/
	grew = 0;
	if (WT_VERBOSE_ISSET(session, WT_VERB_SHARED_CACHE)){
		WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Cache pool distribution: "));
		WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "\t" "cache_size, read_pressure, skips: "));
	}

	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq){
		cache = entry->cache;
		reserved = cache->cp_reserved;
		adjusted = 0;

		/*����cache�Ķ�ȡ�ܶ�*/
		read_pressure = cache->cp_current_read / highest;
		WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "\t%" PRIu64 ", %" PRIu64 ", %" PRIu32, entry->cache_size, read_pressure, cache->cp_skip_count));

		/* Allow to stabilize after changes. */
		if (cache->cp_skip_count > 0 && --cache->cp_skip_count > 0)
			continue;

		if (entry->cache_size < reserved){ /*connection��cache size��δ��cacheָ���Ŀռ��С,�����ʵ��Ե���С*/
			grew = 1;
			adjusted = reserved - entry->cache_size; /*����cache size���޼�С*/
		}
		else if ((force && entry->cache_size > reserved) || (read_pressure < WT_CACHE_POOL_REDUCE_THRESHOLD &&
			highest > 1 && entry->cache_size > reserved && cp->currently_used >= cp->size)){ 
			/*����connection cache�Ķ�ȡ�ʲ��㣬����cache poolȴ�������ֵ����ô��Ҫ�����ӵ���*/
			grew = 0;
			/*
			 * Shrink by a chunk size if that doesn't drop us below the reserved size.
			 * /*����cache size��������
			 */
			if (entry->cache_size > cp->chunk + reserved)
				adjusted = cp->chunk;
			else
				adjusted = entry->cache_size - reserved;
		}
		else if (highest > 1 && entry->cache_size < cp->size &&
			cache->bytes_inmem >= (entry->cache_size * cache->eviction_target) / 100 &&
			cp->currently_used < cp->size && read_pressure > bump_threshold){ /*cache��eviction��С���ڴ滺�������ڶ�ȡ���ܶȴ���ָ������ֵ��˵��cache��Դ�ḻ�����Կ��ǽ���һ������С*/
			grew = 1;
			adjusted = WT_MIN(cp->chunk, cp->size - cp->currently_used);
		}

		if (adjusted > 0){
			*adjustedp = 1;
			if (grew > 0){ /*����cache �ռ�����*/
				cache->cp_skip_count = WT_CACHE_POOL_BUMP_SKIPS;
				entry->cache_size += adjusted;
				cp->currently_used += adjusted;
			}
			else{ /*����cache size��С*/
				cache->cp_skip_count = WT_CACHE_POOL_REDUCE_SKIPS;
				WT_ASSERT(session, entry->cache_size >= adjusted && cp->currently_used >= adjusted);
				entry->cache_size -= adjusted;
				cp->currently_used -= adjusted;
			}
			WT_RET(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Allocated %s%" PRId64 " to %s", grew ? "" : "-", adjusted, entry->home));
		}
	}

	return 0;
}

/*connection��cache pool������thread*/
WT_THREAD_RET __wt_cache_pool_server(void *arg)
{
	WT_CACHE *cache;
	WT_CACHE_POOL *cp;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)arg;

	cp = __wt_process.cache_pool;
	cache = S2C(session)->cache;

	while (F_ISSET_ATOMIC(cp, WT_CACHE_POOL_ACTIVE) && F_ISSET(cache, WT_CACHE_POOL_RUN)){
		if (cp->currently_used <= cp->size)
			WT_ERR(__wt_cond_wait(session, cp->cache_pool_cond, 1000000));

		if (!F_ISSET_ATOMIC(cp, WT_CACHE_POOL_ACTIVE) && F_ISSET(cache, WT_CACHE_POOL_RUN))
			break;

		/* Try to become the managing thread,��ռʽ�ĳ�Ϊleader thread�������߳����û�����ͻ��ΪFollower thread,��֤ͬʱֻ��һ���߳���ִ��balance*/
		F_CAS_ATOMIC(cp, WT_CACHE_POOL_MANAGED, ret);
		if (ret == 0) {
			F_SET(cache, WT_CACHE_POOL_MANAGER);
			WT_ERR(__wt_verbose(session, WT_VERB_SHARED_CACHE, "Cache pool switched manager thread"));
		}

		if (F_ISSET(cache, WT_CACHE_POOL_MANAGER))
			(void)__cache_pool_balance(session);
	}

	if (0){
err:
		WT_PANIC_MSG(session, ret, "cache pool manager server error");
	}
}




