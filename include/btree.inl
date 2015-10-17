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

/*����ref��Ӧpage��pindex�ж�Ӧ�Ĳ�λID*/
static inline void __wt_page_refp(WT_SESSION_IMPL* session, WT_REF* ref, WT_PAGE_INDEX** pindexp, uint32_t* slotp)
{
	WT_PAGE_INDEX *pindex;
	uint32_t i;

retry:
	WT_INTL_INDEX_GET(session, ref->home, pindex);

	/*����ָ������ʼλ�ý��в�λ��ѯ��λ*/
	for(i = ref->ref_hint; i < pindex->entries; ++i){
		if (pindex->index[i]->page == ref->page) {
			*pindexp = pindex;
			*slotp = ref->ref_hint = i;
			return;
		}
	}

	/*��ָ����λ��ʼλ��û�ҵ�����0λ�ÿ�ʼȫ��λɨ�趨λ*/
	for (i = 0; i < pindex->entries; ++i){
		if (pindex->index[i]->page == ref->page) {
			*pindexp = pindex;
			*slotp = ref->ref_hint = i;
			return;
		}
	}

	/*û���ҵ������߳�ִ��Ȩ���¹黹������ϵͳ���е���*/
	__wt_yield();
	goto retry;
}

/*Ϊpage����һ��modify�ṹ������ʼ����*/
static inline int __wt_page_modify_init(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	return (page->modify == NULL ? __wt_page_modify_alloc(session, page) : 0);
}

/*��page��δ�޸�״̬��Ϊ�޸�״̬����Ǵ�pageΪ��ҳ*/
static inline void __wt_page_only_modify_set(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	uint64_t last_running = 0;

	if (page->modify->write_gen == 0)
		last_running = S2C(session)->txn_global.last_running;

	/*����write gen������������Ǵ�0��Ϊ1����ʾ����δ�޸�״̬ת��Ϊ�޸�״̬��������ҳ״̬*/
	if(WT_ATOMIC_ADD4(page->modify->write_gen, 1) == 1){
		__wt_cache_dirty_incr(session, page);

		/*����snapshot������λ��*/
		if (F_ISSET(&session->txn, TXN_HAS_SNAPSHOT))
			page->modify->disk_snap_min = session->txn.snap_min;

		if (last_running != 0)
			page->modify->first_dirty_txn = last_running;
	}

	/*�������µ�txn id*/
	if(TXNID_LT(page->modify->update_txn, session->txn.id))
		page->modify->update_txn = session->txn.id;
}

/*���pageΪbtree�ϵ���ҳ,�������������ҳ*/
static inline void __wt_page_modify_set(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	if (S2BT(session)->modified == 0) {
		S2BT(session)->modified = 1;
		WT_FULL_BARRIER();
	}

	__wt_page_only_modify_set(session, page);
}

/*���ref��Ӧ�ĸ��ڵ�pageΪ��ҳ*/
static inline int __wt_page_parent_modify_set(WT_SESSION_IMPL* session, WT_REF* ref, int page_only)
{
	WT_PAGE *parent;

	parent = ref->home;
	WT_RET(__wt_page_modify_init(session, parent));
	if (page_only)
		__wt_page_only_modify_set(session, parent);
	else
		__wt_page_modify_set(session, parent);

	return 0;
}

/*�ж�p��λ���Ƿ���page�Ĵ��̲���������*/
static inline int __wt_off_page(WT_PAGE* page, const void* p)
{
	return (page->dsk == NULL || p < (void *)page->dsk ||p >= (void *)((uint8_t *)page->dsk + page->dsk->mem_size));
}

/*��btree��key/valueֵ����*/
#define	WT_IK_FLAG					0x01
#define	WT_IK_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 32)
#define	WT_IK_DECODE_KEY_LEN(v)		((v) >> 32)
#define	WT_IK_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 1)
#define	WT_IK_DECODE_KEY_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 1)

/*��ȡ�д洢ʱref��Ӧpage�е��ڲ�keyֵ*/
static inline void __wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, size_t *sizep)
{
	v = (uintptr_t)ref->key.ikey;
	/*key��ֵ��key.ikey�ڴ��ϵ�ǲ������ģ�keypָ��keyֵ��ʼλ��,ͨ��v��ֵ����ƫ��*/
	if (v & WT_IK_FLAG){
		*(void **)keyp = WT_PAGE_REF_OFFSET(page, WT_IK_DECODE_KEY_OFFSET(v));
		*sizep = WT_IK_DECODE_KEY_LEN(v);
	} 
	else {
		*(void **)keyp = WT_IKEY_DATA(ref->key.ikey);
		*sizep = ((WT_IKEY *)ref->key.ikey)->size;
	}
}

/*��unpack������Ϊkey��������WT_REF��KEY,�������ڴ�����*/
static inline void __wt_ref_key_onpage_set(WT_PAGE* page, WT_REF* ref, WT_CELL_UNPACK* unpack)
{
	uintptr_t v;
	v = WT_IK_ENCODE_KEY_LEN(unpack->size) | WT_IK_ENCODE_KEY_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->data)) | WT_IK_FLAG;
	ref->key.ikey = (void *)v;
}

/*�ж�ref��Ӧ��key��ֵ�ռ��Ƿ���������*/
static inline WT_IKEY* __wt_ref_key_instantiated(WT_REF* ref)
{
	uintptr_t v;
	v = (uintptr_t)ref->key.ikey;
	return (v & WT_IK_FLAG ? NULL : ref->key.ikey);
}

/*���WT_REF��key*/
static inline void __wt_ref_key_clear(WT_REF* ref)
{
	ref->key.recno = 0;
}

/*cell �ı�ʾ��cell��ƫ�Ʊ���*/
#define	WT_CELL_FLAG				0x01
#define	WT_CELL_ENCODE_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_CELL_DECODE_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 2)

/*KEY�ı�ʾ�ͱ��룬��32λ��KEY�ĳ��ȣ���2Ϊ�Ǳ�ʾֵ��2 ~ 31λΪƫ��ֵ*/
#define	WT_K_FLAG					0x02
#define	WT_K_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 32)
#define	WT_K_DECODE_KEY_LEN(v)		((v) >> 32)
#define	WT_K_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_K_DECODE_KEY_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 2)

/*KV�ı�ʾ�ͱ���
 *0 ~ 1			��ʾλ
 *2 ~ 21		value��ƫ��
 *22 ~ 41		key��ƫ��
 *42 ~ 54		value�ĳ���
 *55 ~ 63		key�ĳ���
 */
#define	WT_KV_FLAG					0x03
#define	WT_KV_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 55)
#define	WT_KV_DECODE_KEY_LEN(v)		((v) >> 55)
#define	WT_KV_MAX_KEY_LEN			(0x200 - 1)
#define	WT_KV_ENCODE_VALUE_LEN(v)	((uintptr_t)(v) << 42)
#define	WT_KV_DECODE_VALUE_LEN(v)	(((v) & 0x007FFC0000000000) >> 42)
#define	WT_KV_MAX_VALUE_LEN			(0x2000 - 1)
#define	WT_KV_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 22)
#define	WT_KV_DECODE_KEY_OFFSET(v)	(((v) & 0x000003FFFFC00000) >> 22)
#define	WT_KV_MAX_KEY_OFFSET		(0x100000 - 1)
#define	WT_KV_ENCODE_VALUE_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_KV_DECODE_VALUE_OFFSET(v)	(((v) & 0x00000000003FFFFC) >> 2)
#define	WT_KV_MAX_VALUE_OFFSET		(0x100000 - 1)

/*����copy��ֵ��ͨ�������õ�page��key��ָ��ƫ��λ�úͶ�Ӧ���ݳ���*/
static inline int __wt_row_leaf_key_info(WT_PAGE* page, void* copy, WT_IKEY** ikeyp, WT_CELL** cellp, void* datap, size_t *sizep)
{
	WT_IKEY* ikey;
	uintptr_t v;

	v = (uintptr_t)copy;

	switch(v & 0x03){
	case WT_CELL_FLAG: /*cell��ʽ����*/
		if (ikeyp != NULL)
			*ikeyp = NULL;
		if (cellp != NULL)
			*cellp = WT_PAGE_REF_OFFSET(page, WT_CELL_DECODE_OFFSET(v));
		return 0;

	case WT_K_FLAG: /*key��ʽ����*/
		if(cellp != NULL)
			*cellp = NULL;
		if(ikeyp != NULL)
			*ikeyp = NULL;
		if(datap != NULL){
			*(void **)datap = WT_PAGE_REF_OFFSET(page, WT_K_DECODE_KEY_OFFSET(v));
			*sizep = WT_K_DECODE_KEY_LEN(v);
			return 1;
		}
		return 0;

	case WT_KV_FLAG: /*key/value��ʽ����*/
		if (cellp != NULL)
			*cellp = NULL;
		if (ikeyp != NULL)
			*ikeyp = NULL;
		if (datap != NULL) {
			*(void **)datap = WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_KEY_OFFSET(v));
			*sizep = WT_KV_DECODE_KEY_LEN(v);
			return 1;
		}
		return 0;
	}

	/*Ĭ��������keyֵ�洢��ʽ*/
	ikey = copy;
	if (ikeyp != NULL)
		*ikeyp = copy;
	if (cellp != NULL)
		*cellp = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
	if (datap != NULL) {
		*(void **)datap = WT_IKEY_DATA(ikey);
		*sizep = ikey->size;
		return 1;
	}
	return 0;
}

/*�����д洢ʱҶ�ӽڵ��reference key��ֵ*/
static inline void __wt_row_leaf_key_set(WT_PAGE* page, WT_ROW* rip, WT_CELL_UNPACK* unpack)
{
	uintptr_t v;

	/*����洢��ֵ*/
	v = WT_K_ENCODE_KEY_LEN(unpack->size) |
		WT_K_ENCODE_KEY_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->data)) | WT_K_FLAG;

	WT_ROW_KEY_SET(rip, v);
}

/*�����д洢ʱҶ�ӽڵ��reference cell��ֵ*/
static inline void __wt_row_leaf_key_set_cell(WT_PAGE* page, WT_ROW* rip, WT_CELL_UNPACK* unpack)
{
	uintptr_t v;
	v = WT_CELL_ENCODE_OFFSET(WT_PAGE_DISK_OFFSET(page, cell)) | WT_CELL_FLAG;
	WT_ROW_KEY_SET(rip, v);
}

/*�����д洢��Ҳֻ�����reference k/v��ֵ*/
static inline void __wt_row_leaf_value_set(WT_PAGE* page, WT_ROW* rip, WT_CELL_UNPACK* unpack)
{
	uintptr_t key_len, key_offset, value_offset, v;

	v = (uintptr_t)WT_ROW_KEY_COPY(rip);
	if(!(v & WT_K_FLAG)) /*û��������KEY��ֵ����������VALUE��ֵ*/
		return ;

	key_len = WT_K_DECODE_KEY_LEN(v);	/* Key length */
	if (key_len > WT_KV_MAX_KEY_LEN)
		return;

	if (unpack->size > WT_KV_MAX_VALUE_LEN)	/* Value length */
		return;

	key_offset = WT_K_DECODE_KEY_OFFSET(v);	/* Page offsets */
	if (key_offset > WT_KV_MAX_KEY_OFFSET)
		return;

	value_offset = WT_PAGE_DISK_OFFSET(page, unpack->data);
	if (value_offset > WT_KV_MAX_VALUE_OFFSET)
		return;

	/*��k/v��ʽ����ħ����ֵ�ĸ���*/
	v = WT_KV_ENCODE_KEY_LEN(key_len) |
		WT_KV_ENCODE_VALUE_LEN(unpack->size) |
		WT_KV_ENCODE_KEY_OFFSET(key_offset) |
		WT_KV_ENCODE_VALUE_OFFSET(value_offset) | WT_KV_FLAG;

	WT_ROW_KEY_SET(rip, v);
}

/*��ȡrow store��Ҷ�ӽڵ�ο�KEYֵ*/
static inline int __wt_row_leaf_key(WT_SESSION_IMPL* session, WT_PAGE* page, WT_ROW* rip, WT_ITEM* key, int instantiate)
{
	void* copy;

	copy = WT_ROW_KEY_COPY(rip);
	/*����key��ħ����*/
	if(__wt_row_leaf_key_info(page, copy, NULL, NULL, &key->data, &key->size))
		return 0;

	return __wt_row_leaf_key_work(session, page, rip, key, instantiate);
}

/*��ȡbtree cursor����Ҷ�ӽڵ��keyֵ*/
static inline int __wt_cursor_row_leaf_key(WT_CURSOR_BTREE* cbt, WT_ITEM* key)
{
	WT_PAGE *page;
	WT_ROW *rip;
	WT_SESSION_IMPL *session;

	if (cbt->ins == NULL) {
		session = (WT_SESSION_IMPL *)cbt->iface.session;
		page = cbt->ref->page;
		rip = &page->u.row.d[cbt->slot];
		WT_RET(__wt_row_leaf_key(session, page, rip, key, 0));
	} 
	else { /*ֱ�ӻ�����һ��insert������key����*/
		key->data = WT_INSERT_KEY(cbt->ins);
		key->size = WT_INSERT_KEY_SIZE(cbt->ins);
	}

	return 0;
}

/*����д洢ʱ��Ҷ�ӽڵ��cellֵ*/
static inline WT_CELL* __wt_row_leaf_value_cell(WT_PAGE* page, WT_ROW* rip, WT_CELL_UNPACK* kpack)
{
	WT_CELL *kcell, *vcell;
	WT_CELL_UNPACK unpack;
	void *copy, *key;
	size_t size;

	/*��kpack����ֱ�ӻ��cell����ָ��*/
	if (kpack != NULL)
		vcell = (WT_CELL *)((uint8_t *)kpack->cell + __wt_cell_total_len(kpack));
	else{
		copy = WT_ROW_KEY_COPY(rip);

		/*���cell�Ĵ洢��Ϣ����Ҫ��ƫ�����ͳ���,��ͨ��key��size�õ�cellָ��*/
		if (__wt_row_leaf_key_info(page, copy, NULL, &kcell, &key, &size) && kcell == NULL)
			vcell = (WT_CELL *)((uint8_t *)key + size);
		else { /*ͨ��unpackֱ�ӽ������cell����ָ��*/
			__wt_cell_unpack(kcell, &unpack);
			vcell = (WT_CELL *)((uint8_t *)unpack.cell + __wt_cell_total_len(&unpack));
		}
	}

	/*��cell�Ľ����������ض���ָ��*/
	return __wt_cell_leaf_value_parse(page, vcell);
}

/*����д洢ʱk/v��ħ�����д洢����Ϣ��ƫ���� + ���ȣ�*/
static inline int __wt_row_leaf_value(WT_PAGE* page, WT_ROW* rip, WT_ITEM* value)
{
	uintptr_t v;

	v = (uintptr_t)WT_ROW_KEY_COPY(rip);

	/*�������WT_KV_FLAGģʽ���޷����K/V����Ϣ*/
	if((v & 0x03) == WT_KV_FLAG){
		value->data = WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_VALUE_OFFSET(v));
		value->size = WT_KV_DECODE_VALUE_LEN(v);

		return 1;
	}

	return 0;
}

/*ͨ��ref��Ϣ������һ����Ӧ�ĵ�block��(addr/size/type)�ԣ����block��ַ�Կ��ԴӴ����϶�ȡ��Ӧ����Ϣ*/
static inline int __wt_ref_info(WT_SESSION_IMPL* session, WT_REF* ref, const uint8_t** addrp, size_t* sizep, u_int* typep)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK *unpack, _unpack;

	addr = ref->addr;
	unpack = &_unpack;

	if(addr == NULL){
		*addrp = NULL;
		*sizep = NULL;
		if(typep != NULL)
			*typep = 0;
	}
	else if (__wt_off_page(ref->home, addr)){ /*addr�洢�����ڴ����ϣ���Ҫ���д��̶�ȡ*/
		*addrp = addr->addr;
		*sizep = addr->size;
		if (typep != NULL)
			switch (addr->type) { /*ȷ��typeֵ*/
			case WT_ADDR_INT:
				*typep = WT_CELL_ADDR_INT;
				break;
			case WT_ADDR_LEAF:
				*typep = WT_CELL_ADDR_LEAF;
				break;
			case WT_ADDR_LEAF_NO:
				*typep = WT_CELL_ADDR_LEAF_NO;
				break;

			WT_ILLEGAL_VALUE(session);
		}
	}
	else{ /*��unpack�н���*/
		__wt_cell_unpack((WT_CELL *)addr, unpack);
		*addrp = unpack->data;
		*sizep = unpack->size;
		if (typep != NULL)
			*typep = unpack->type;
	}

	return 0;
}

/*�ж�page�Ƿ���Խ���LRU��̭*/
static inline int __wt_page_can_evict(WT_SESSION_IMPL* session, WT_PAGE* page, int check_splits)
{
	WT_BTREE *btree;
	WT_PAGE_MODIFY *mod;

	btree = S2BT(session);
	mod = page->modify;

	/*pageû�н��й��޸ģ�����ֱ�ӽ�����̭������Ҫ�������̲���*/
	if(mod == NULL)
		return 1;

	/*page�Ѿ����ڽ�����̭,����Ҫ�ظ�*/
	if(F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
		return 0;

	/*page���ڷ���,���ܽ�����̭*/
	if(check_splits && WT_PAGE_IS_INTERNAL(page) && !__wt_txn_visible_all(session, mod->mod_split_txn))
		return 0;

	/*btree���ڽ���checkpoint���������page�Ǳ��޸Ĺ��ģ����ܽ�����̭*/
	if (btree->checkpointing && (__wt_page_is_modified(page) || F_ISSET(mod, WT_PM_REC_MULTIBLOCK))) {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_checkpoint);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_checkpoint);
		return 0;
	}

	/*page����cache�������ж�������ҳ�����һ�������ִ�е��������������page,���ܽ�����̭*/
	if (page->read_gen != WT_READGEN_OLDEST && !__wt_txn_visible_all(session, __wt_page_is_modified(page) ? mod->update_txn : mod->rec_max_txn))
		return 0;

	/*���page�ղŷ�����split��������������������̭������ͨ��viction thread������̭slipt*/
	if (check_splits && !__wt_txn_visible_all(session, mod->inmem_split_txn))
		return 0;

	return 1;
}

/*������̭���ͷ�һ���ڴ��е�page*/
static inline int  __wt_page_release_evict(WT_SESSION_IMPL* session, WT_REF* ref)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	int locked, too_big;

	btree = S2BT(session);
	page = ref->page;
	too_big = (page->memory_footprint > btree->maxmempage) ? 1 : 0;

	/*�Ƚ�state��ref mem����ΪLOCKED,��ֹ��������������ҳ*/
	locked = WT_ATOMIC_CAS4(ref->state, WT_REF_MEM, WT_REF_LOCKED);
	WT_TRET(__wt_hazard_clear(session, page));
	if(!locked){
		WT_TRET(EBUSY);
		return ret;
	}

	/*��btree���Ϊ������̭page״̬��������̭������,������ҳ��̭*/
	(void)WT_ATOMIC_CAS4(btree->evict_busy, 1);
	if((ret = __wt_evict_page(session, ref)) == 0){
		if (too_big)
			WT_STAT_FAST_CONN_INCR(session, cache_eviction_force);
		else
			WT_STAT_FAST_CONN_INCR(session, cache_eviction_force_delete);
	}
	else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_force_fail);

	/*��̭��ϣ�btree��̭�������ݼ�*/
	(void)WT_ATOMIC_SUB4(btree_evict_busy, 1);

	return ret;
}

/*�ͷ�һ��ref��Ӧ��page*/
static inline int __wt_page_release(WT_SESSION_IMPL* session, WT_REF* ref, uint32_t flags)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = S2BT(session);

	/*refΪNULL����REF��Ӧ��page��root page,���ܽ����ͷ�*/
	if(ref == NULL || __wt_ref_is_root(ref)){
		return 0;
	}

	page = ref->page;

	/*�ж��Ƿ�����̭page, �ͷŵ�һ�������緢����������page*/
	if(page->read_gen != WT_READGEN_OLDEST || LF_ISSET(WT_READ_NO_EVICT) || F_ISSET(btree, WT_BTREE_NO_EVICTION)
		!__wt_page_can_evict(session, page, 1)){
			return __wt_hazard_clear(session, page);
	}

	WT_RET_BUSY_OK(__wt_page_release_evict(session, ref));

	return 0;
}

/*����held��want��pageָ�룬�൱����̭held,�Ӵ����ж�ȡwant������held��λ��*/
static inline int __wt_page_swap_func(WT_SESSION_IMPL* session, WT_REF* held, WT_REF* want, uint32_t flags)
{
	WT_DECL_RET;
	int acquired;

	if(held == want)
		return 0;
	/*��ȡref��Ӧpage��ָ�룬����������ڴ��У���Ӵ����ж�ȡ���뵽�ڴ���*/
	ret = __wt_page_in_func(session, want, flags);
	/*want��ȡʧ�ܻ��߲���ʧ��*/
	if (LF_ISSET(WT_READ_CACHE) && ret == WT_NOTFOUND)
		return (WT_NOTFOUND);

	if (ret == WT_RESTART)
		return (WT_RESTART);

	acquired = ret == 0;
	/*�ͷŵ�held��Ӧ��page*/
	WT_TRET(__wt_page_release(session, held, flags));
	
	/*�����ͷ�heldʧ�ܣ��޷�����ָ�룬Ӧ�ý�want���ڴ����ͷ�*/
	if (acquired && ret != 0)
		WT_TRET(__wt_page_release(session, want, flags));

	return ret;
}

/*���page�Ƿ���һ��hazard pointer,����Ƿ������Ӧhazard����,�����漰�������������������ڴ���������֤����*/
static inline WT_HAZARD* __wt_page_hazard_check(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	WT_SESSION_IMPL *s;
	uint32_t i, hazard_size, session_cnt;

	conn = S2C(session);

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for(s == conn->sessions, i = 0; i < session_cnt; ++s, ++i){
		if(!s->active)
			continue;

		WT_ORDERED_READ(hazard_size, s->hazard_size);

		for (hp = s->hazard; hp < s->hazard + hazard_size; ++hp){
			if (hp->page == page)
				return hp;
		}
	}

	return NULL;
}

/*���һ��������skiplist��insert����*/
static inline u_int __wt_skip_choose_depth(WT_SESSION_IMPL* session)
{
	u_int d;
	for(d = 1; d < WT_SKIP_MAXDEPTH && __wt_random(session->rnd) < WT_SKIP_PROBABILITY; d++)
		;

	return d;
}

/*
 * __wt_btree_lsm_size --
 *	Return if the size of an in-memory tree with a single leaf page is over
 * a specified maximum.  If called on anything other than a simple tree with a
 * single leaf page, returns true so our LSM caller will switch to a new tree.
 */
static inline __wt_btree_lsm_size(WT_SESSION_IMPL* session, uint64_t maxsize)
{
	WT_BTREE *btree;
	WT_PAGE *child, *root;
	WT_PAGE_INDEX *pindex;
	WT_REF *first;

	btree = S2BT(session);
	root = btree->root.page;

	if(root == NULL)
		return 0;

	if(!F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return 1;

	/* ���btree�Ƿ�ֻ��һ��Ҷ�ӽڵ� */
	WT_INTL_INDEX_GET(session, root, pindex);
	if (pindex->entries != 1)		
		return 1;

	first = pindex->index[0];
	if (first->state != WT_REF_MEM)		
		return (0);

	child = first->page;
	if(child->type != WT_PAGE_ROW_LEAF) /*��һ������Ҳ���ǵ���Ҷ�ӽڵ�*/
		return 1;

	return child->memory_footprint > maxsize;
}




