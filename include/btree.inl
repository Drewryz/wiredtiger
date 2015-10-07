/************************************************************************
*btree��һЩ������������Ҫ������cache��Ϣ״̬�ı䣬��ҳ״̬��BTREE�е�KEY
*VALUE��ʽ�����Ⱥ�����
************************************************************************/

/*�ж�ref��Ӧ��page�Ƿ���root page*/
static inline int __wt_ref_is_root(WT_REF* ref)
{
	return (ref->home == NULL ? 1 : 0);
}

/*�ж�page�Ƿ�һ����ҳ*/
static inline int __wt_page_is_empty(WT_PAGE* page)
{
	return (page->modify != NULL && F_ISSET(page->modify, WT_PM_REC_MASK) == WT_PM_REC_EMPTY : 1 : 0);
}

/*�ж�page�Ƿ�����ڴ��е��޸ģ�Ҳ������ҳ�ж�*/
static inline int __wt_page_is_modified(WT_PAGE* page)
{
	return (page->modify != NULL && page->modify->write_gen != 0 ? 1 : 0);
}

/*��cache���Ӷ�page���ڴ���Ϣͳ��*/
static inline void __wt_cache_inmem_incr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE* cache;

	WT_ASSERT(session, size < WT_EXABYTE);

	cache = S2C(session)->cache;

	/*��cache�и������ݵ�����*/
	(void)WT_ATOMIC_ADD8(cache->bytes_inmem, size);
	(void)WT_ATOMIC_ADD8(page->memory_footprint, size);
	if (__wt_page_is_modified(page)) {
		(void)WT_ATOMIC_ADD8(cache->bytes_dirty, size);
		(void)WT_ATOMIC_ADD8(page->modify->bytes_dirty, size);
	}
	/*�����page��ͳ����Ϣ����*/
	if (WT_PAGE_IS_INTERNAL(page))
		(void)WT_ATOMIC_ADD8(cache->bytes_internal, size);
	else if (page->type == WT_PAGE_OVFL)
		(void)WT_ATOMIC_ADD8(cache->bytes_overflow, size);
}

/*�����if�ж���Ϊ�˷�ֹ��ֵ���*/
#define WT_CACHE_DECR(s, f, sz) do{					\
	if(WT_ATOMIC_SUB8(f, sz) > WT_EXABYTE)			\
		(void)WT_ATOMIC_ADD8(f, sz);				\
}while(0)

/*��ҳ���ݵݼ���һ������ҳˢ�̺���ڴ�����̭ʱ�����*/
static inline void __wt_cache_page_byte_dirty_decr(WT_SESSION_IMPL* session, WT_PAGE* page, size_t size)
{
	WT_CACHE* cache;
	size_t decr, orig;
	int i;

	cache = S2C(session)->cache;
	for(i = 0; i < 5; i++){
		orig = page->modify->bytes_dirty;
		decr = WT_MIN(size, orig);
		/*�����޸�page��modify������ɹ��ˣ��Ž���cache���޸ģ����Ǹ������Ⱥ������*/
		if (WT_ATOMIC_CAS8(page->modify->bytes_dirty, orig, orig - decr)) {
			WT_CACHE_DECR(session, cache->bytes_dirty, decr);
			break;
		}
	}
}

/*page���ڴ�����̭��cache��Ҫ�޸Ķ�Ӧ���ڴ�ͳ����Ϣ*/
static inline void __wt_cache_page_inmem_decr(WT_SESSION_IMPL* session, WT_PAGE* page, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ASSERT(session, size < WT_EXABYTE);

	WT_CACHE_DECR(session, cache->bytes_inmem, size);
	WT_CACHE_DECR(session, page->memory_footprint, size);
	/*page�Ǹ���ҳ����Ҫ������ҳ����ͳ����Ϣ����*/
	if (__wt_page_is_modified(page))
		__wt_cache_page_byte_dirty_decr(session, page, size);

	if (WT_PAGE_IS_INTERNAL(page))
		WT_CACHE_DECR(session, cache->bytes_internal, size);
	else if (page->type == WT_PAGE_OVFL)
		WT_CACHE_DECR(session, cache->bytes_overflow, size);
}

/*page switch���������cache dirty page���ֽ�ͳ������*/
static inline void __wt_cache_dirty_incr(WT_SESSION_IMPL* session, WT_PAGE* page)
{
WT_CACHE *cache;
	size_t size;

	cache = S2C(session)->cache;
	/*������ҳ����*/
	(void)WT_ATOMIC_ADD8(cache->pages_dirty, 1);

	/*��������������*/
	size = page->memory_footprint;
	(void)WT_ATOMIC_ADD8(cache->bytes_dirty, size);
	(void)WT_ATOMIC_ADD8(page->modify->bytes_dirty, size);
}

/*page switch���������cache dirty page���ֽ�ͳ�Ƶݼ�*/
static inline void __wt_cache_dirty_decr(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_CACHE *cache;
	WT_PAGE_MODIFY *modify;

	cache = S2C(session)->cache;

	if (cache->pages_dirty < 1) {
		__wt_errx(session, "cache eviction dirty-page decrement failed: dirty page count went negative");
		cache->pages_dirty = 0;
	} 
	else
		(void)WT_ATOMIC_SUB8(cache->pages_dirty, 1);

	modify = page->modify;
	if (modify != NULL && modify->bytes_dirty != 0)
		__wt_cache_page_byte_dirty_decr(session, page, modify->bytes_dirty);
}

/*page��cache����̭����Ҫ�� cache��״̬ͳ�ƽ����޸�*/
static inline void __wt_cache_page_evict(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_CACHE *cache;
	WT_PAGE_MODIFY *modify;

	cache = S2C(session)->cache;
	modify = page->modify;

	/*�����޸�cache���ڴ���������*/
	WT_CACHE_DECR(session, cache->bytes_inmem, page->memory_footprint);

	/*�޸�������ͳ��*/
	if (modify != NULL && modify->bytes_dirty != 0) {
		if (cache->bytes_dirty < modify->bytes_dirty) {
			__wt_errx(session, "cache eviction dirty-bytes decrement failed: dirty byte count went negative");
			cache->bytes_dirty = 0;
		} 
		else
			WT_CACHE_DECR(session, cache->bytes_dirty, modify->bytes_dirty);
	}

	/* Update pages and bytes evicted. */
	(void)WT_ATOMIC_ADD8(cache->bytes_evict, page->memory_footprint);
	(void)WT_ATOMIC_ADD8(cache->pages_evict, 1);
}

/*����upd���ڴ����޸ĵ���������*/
static inline size_t __wt_update_list_memsize(WT_UPDATE* upd)
{
	size_t upd_size;
	for (upd_size = 0; upd != NULL; upd = upd->next)
		upd_size += WT_UPDATE_MEMSIZE(upd);

	return upd_size;
}

/*��page��Ϊ��̭״̬*/
static inline void __wt_page_evict_soon(WT_PAGE* page)
{
	page->read_gen = WT_READGEN_OLDEST;
}



