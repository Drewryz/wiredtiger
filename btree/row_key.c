/*********************************************************
*row store btree��key��ز���
*********************************************************/
#include "wt_internal.h"

static void __inmem_row_leaf_slots(uint8_t* , uint32_t, uint32_t, uint32_t);

/*���ڴ���ʵ����page��row�е�key����*/
int __wt_row_leaf_keys(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(key);
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_ROW *rip;
	uint32_t gap, i;

	btree = S2BT(session);

	/*ҳû�������ݣ������Ѿ�����ڴ���ʵ����row keyʵ������ֱ�ӷ���*/
	if (page->pg_row_entries == 0){
		F_SET_ATOMIC(page, WT_PAGE_BUILD_KEYS);
		return 0;
	}

	WT_RET(__wt_scr_alloc(session, 0, &key));
	/*ÿһ��ռһ��bitλ��*/
	WT_RET(__wt_scr_alloc(session, (uint32_t)__bitstr_size(page->pg_row_entries), &tmp));

	/*ȷ���м����*/
	if ((gap = btree->key_gap) == 0)
		gap = 1;

	/*��row slots��λ���ڴ��д���*/
	__inmem_row_leaf_slots(tmp->mem, 0, page->pg_row_entries, gap);

	for (rip = page->pg_row_d, i = 0; i < page->pg_row_entries; ++rip, ++i){
		if (__bit_test(tmp->mem, i)) /*ȷ��������Ƿ���Ҫʵ����key,�ǲ���ʵ��������__inmem_row_leaf_slotsȷ����*/
			WT_ERR(__wt_row_leaf_key_work(session, page, rip, key, 1));
	}

	/*�������ʵ����KEY��ʾ*/
	F_SET_ATOMIC(page, WT_PAGE_BUILD_KEYS);

err:
	__wt_scr_free(session, &key);
	__wt_scr_free(session, &tmp);
	return ret;
}

/*����gap��������������Ҫʵ����key��row slots*/
static void __inmem_row_leaf_slots(uint8_t* list, uint32_t base, uint32_t entries, uint32_t gap)
{
	uint32_t indx, limit;

	if (entries < gap)
		return;

	/* �ö��ַ�����keyʵ������row�����, ��ǵļ����gap��С */
	limit = entries;
	indx = base + (limit >> 1);
	__bit_set(list, indx);

	__inmem_row_leaf_slots(list, base, limit >> 1, gap);

	base = indx + 1;
	--limit;
	__inmem_row_leaf_slots(list, base, limit >> 1, gap);
}

/*���rip��Ӧrow key�Ŀ���*/
int __wt_row_leaf_key_copy(WT_SESSION_IMPL* session, WT_PAGE* page, WT_ROW* rip, WT_ITEM* key)
{
	WT_RET(__wt_row_leaf_key(session, page, rip, key, 0));

	if (!WT_DATA_IN_ITEM(key))
		WT_RET(__wt_buf_set(session, key, key->data, key->size));

	return 0;
}

/*���һ��row key,���row����Ҷ�����ݽڵ��ϣ����keyû�б�ʵ�������ڴ��У������ڴ�ʵ����*/
int __wt_row_leaf_key_work(WT_SESSION_IMPL* session, WT_PAGE* page, WT_ROW* rip_arg, WT_ITEM* keyb, int instantiate)
{
	enum { FORWARD, BACKWARD } direction;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_ROW *rip, *jump_rip;
	size_t size;
	u_int last_prefix;
	int jump_slot_offset, slot_offset;
	void *copy;
	const void *p;

	/*
	* !!!
	* It is unusual to call this function: most code should be calling the
	* front-end, __wt_row_leaf_key, be careful if you're calling this code
	* directly.
	*/
	btree = S2BT(session);
	unpack = &_unpack;
	rip = rip_arg;

	jump_rip = NULL;
	jump_slot_offset = 0;
	last_prefix = 0;

	p = NULL;
	size = 0;
	direction = BACKWARD;
	for (slot_offset = 0;;){
		if (0){
switch_and_jump:	
			/* Switching to a forward roll. */
			WT_ASSERT(session, direction == BACKWARD);
			direction = FORWARD;

			/* Skip list of keys with compatible prefixes. */
			rip = jump_rip;
			slot_offset = jump_slot_offset;
		}

		copy = WT_ROW_KEY_COPY(rip);
		/*��copy��ֵ���ݽ��н������õ�key/value�Ե�ֵ�����ֵ��cell�У���cell����unpack*/
		(void)__wt_row_leaf_key_info(page, copy, &ikey, &cell, &p, &size);
		/*ֱ�ӿ��Ե���key��value��ֵ*/
		if (cell == NULL){
			keyb->data = p;
			keyb->size = size;
			
			/*
			* If this is the key we originally wanted, we don't
			* care if we're rolling forward or backward, or if
			* it's an overflow key or not, it's what we wanted.
			* This shouldn't normally happen, the fast-path code
			* that front-ends this function will have figured it
			* out before we were called.
			*
			* The key doesn't need to be instantiated, skip past
			* that test.
			*/
			if (slot_offset == 0)
				goto done;

			goto switch_and_jump;
		}

		/*�ж�key�Ƿ��Ѿ�ʵ����*/
		if (ikey != NULL){
			if (slot_offset == 0) {
				keyb->data = p;
				keyb->size = size;
				goto done;
			}

			/*keyֵ����overflow���ͣ�ֵ�Ǵ洢�������ط���cell������*/
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
				goto next;

			keyb->data = p;
			keyb->size = size;
			direction = FORWARD;
			goto next;
		}

		/*��cell����unpack*/
		__wt_cell_unpack(cell, unpack);

		/* 3: ����overflow key */
		if (unpack->type == WT_CELL_KEY_OVFL){
			if (slot_offset == 0) {
				WT_ERR(__wt_readlock(session, btree->ovfl_lock));

				/*��ȡkeyֵ�洢��cellλ��*/
				copy = WT_ROW_KEY_COPY(rip);
				if (!__wt_row_leaf_key_info(page, copy, NULL, &cell, &keyb->data, &keyb->size)) {
					__wt_cell_unpack(cell, unpack);
					ret = __wt_dsk_cell_data_ref(session, WT_PAGE_ROW_LEAF, unpack, keyb);
				}

				WT_TRET(__wt_readunlock(session, btree->ovfl_lock));
				WT_ERR(ret);
				break;
			}

			goto next;
		}

		/*key��ǰ׺û��ѹ��*/
		if (unpack->prefix == 0){
			WT_ASSERT(session, btree->huffman_key != NULL);
			/*ֱ�ӻ�ȡkeyֵ*/
			WT_ERR(__wt_dsk_cell_data_ref(session, WT_PAGE_ROW_LEAF, unpack, keyb));
			if (slot_offset == 0)
				goto done;
			goto switch_and_jump;
		}

		/*
		* 5: an on-page reference to a key that's prefix compressed.
		*	If rolling backward, keep looking for something we can
		* use.
		*	If rolling forward, build the full key and keep rolling
		* forward.
		* unpackǰ׺��ѹ����,�ȱ������rip��λ�ã���ֹ�����row key��Ҳ
		* ��һ��overflow���ͣ������Ļ����Ǿ�Ҫ���˵����ڵ�λ�ý��л�ȡ
		*/

		if (direction == BACKWARD) {
			/*
			* If there's a set of keys with identical prefixes, we
			* don't want to instantiate each one, the prefixes are
			* all the same.
			*
			* As we roll backward through the page, track the last
			* time the prefix decreased in size, so we can start
			* with that key during our roll-forward.  For a page
			* populated with a single key prefix, we'll be able to
			* instantiate the key we want as soon as we find a key
			* without a prefix.
			*/
			if (slot_offset == 0)
				last_prefix = unpack->prefix;
			if (slot_offset == 0 || last_prefix > unpack->prefix) {
				jump_rip = rip;
				jump_slot_offset = slot_offset;
				last_prefix = unpack->prefix;
			}
		}
		if (direction == FORWARD) {
			/*
			* Get a reference to the current key's bytes.  Usually
			* we want bytes from the page, fast-path that case.
			*/
			if (btree->huffman_key == NULL) {
				p = unpack->data;
				size = unpack->size;
			}
			else {
				if (tmp == NULL)
					WT_ERR(__wt_scr_alloc(session, 0, &tmp));
				WT_ERR(__wt_dsk_cell_data_ref(session, WT_PAGE_ROW_LEAF, unpack, tmp));
				p = tmp->data;
				size = tmp->size;
			}

			/*
			* Grow the buffer as necessary as well as ensure data
			* has been copied into local buffer space, then append
			* the suffix to the prefix already in the buffer.
			*
			* Don't grow the buffer unnecessarily or copy data we
			* don't need, truncate the item's data length to the
			* prefix bytes.
			*/
			keyb->size = unpack->prefix;
			WT_ERR(__wt_buf_grow(session, keyb, keyb->size + size));
			memcpy((uint8_t *)keyb->data + keyb->size, p, size);
			keyb->size += size;

			if (slot_offset == 0)
				break;
		}
next:
		switch (direction) {
		case  BACKWARD:
			--rip;
			++slot_offset;
			break;
		case FORWARD:
			++rip;
			--slot_offset;
			break;
		}
	}

	if (instantiate) {
		copy = WT_ROW_KEY_COPY(rip_arg);
		(void)__wt_row_leaf_key_info(page, copy, &ikey, &cell, NULL, NULL);
		if (ikey == NULL) {
			/*���ڴ���ʵ����ikey����*/
			WT_ERR(__wt_row_ikey_alloc(session, WT_PAGE_DISK_OFFSET(page, cell), keyb->data, keyb->size, &ikey));
			/*
			* Serialize the swap of the key into place: on success,
			* update the page's memory footprint, on failure, free
			* the allocated memory.
			*/
			if (WT_ATOMIC_CAS8(WT_ROW_KEY_COPY(rip), copy, ikey))
				__wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + ikey->size);
			else
				__wt_free(session, ikey);
		}
	}

done:
err:
	__wt_scr_free(session, &tmp);
   return (ret);
}

/*���ڴ��д���һ��WT_KEY�ṹ����������ֵ*/
int __wt_row_ikey_alloc(WT_SESSION_IMPL* session, uint32_t cell_offset, const void* key, size_t size, WT_IKEY **ikeyp)
{
	WT_IKEY *ikey;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_IKEY) + size, &ikey));
	ikey->size = WT_STORE_SIZE(size);
	ikey->cell_offset = cell_offset;
	memcpy(WT_IKEY_DATA(ikey), key, size);
	*ikeyp = ikey;

	return 0;
}

int __wt_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t cell_offset, const void *key, size_t size, WT_REF *ref)
{
	WT_RET(__wt_row_ikey(session, cell_offset, key, size, ref));

	__wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + size);

	return 0;
}

/*ʵ����һ��ָ����key�ṹ����*/
int __wt_row_ikey(WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key, size_t size, WT_REF *ref)
{
	WT_IKEY *ikey;

	WT_RET(__wt_row_ikey_alloc(session, cell_offset, key, size, &ikey));
	ref->key.ikey = ikey;

	return 0;
}





