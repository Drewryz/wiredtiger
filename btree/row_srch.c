/************************************************************
 * row store btree��key������λ����ʵ��
 ***********************************************************/

#include "wt_internal.h"

/* ��insert append�б��в��Ҷ�λkey��Ӧ�ļ�¼, ������һ��skip list stack ���󷵻�*/
static inline int __wt_search_insert_append(WT_SESSION_IMPL* session, WT_CURSOR_BTREE* cbt, WT_ITEM* srch_key, int* donep)
{
	WT_BTREE *btree;
	WT_COLLATOR *collator;
	WT_INSERT *ins;
	WT_INSERT_HEAD *inshead;
	WT_ITEM key;
	int cmp, i;

	btree = S2BT(session);
	collator = btree->collator;
	*donep = 0;

	inshead = cbt->ins_head;
	if ((ins = WT_SKIP_LAST(inshead)) == NULL)
		return NULL;

	/*ͨ��ins���key��ֵ*/
	key.data = WT_INSERT_KEY(ins);
	key.size = WT_INSERT_KEY_SIZE(ins);

	/*����ֵ�Ƚ�*/
	WT_RET(__wt_compare(session, collator, srch_key, &key, &cmp));
	if (cmp >= 0){
		/*
		* !!!
		* We may race with another appending thread.
		*
		* To catch that case, rely on the atomic pointer read above
		* and set the next stack to NULL here.  If we have raced with
		* another thread, one of the next pointers will not be NULL by
		* the time they are checked against the next stack inside the
		* serialized insert function.
		*/

		for (i = WT_SKIP_MAXDEPTH - 1; i >= 0; i--) {
			cbt->ins_stack[i] = (i == 0) ? &ins->next[0] :
				(inshead->tail[i] != NULL) ? &inshead->tail[i]->next[i] : &inshead->head[i];

			cbt->next_stack[i] = NULL;
		}
		cbt->compare = -cmp;
		cbt->ins = ins;
		*donep = 1;
	}

	return 0;
}

/* Ϊrow store��ʽ����insert list,������һ��skip list stack */
int __wt_search_insert(WT_SESSION_IMPL* session, WT_CURSOR_BTREE* cbt, WT_ITEM* srch_key)
{
	WT_BTREE *btree;
	WT_COLLATOR *collator;
	WT_INSERT *ins, **insp, *last_ins;
	WT_INSERT_HEAD *inshead;
	WT_ITEM key;
	size_t match, skiphigh, skiplow;
	int cmp, i;

	btree = S2BT(session);
	collator = btree->collator;
	inshead = cbt->ins_head;
	cmp = 0;				/* -Wuninitialized */

	match = skiphigh = skiplow = 0;
	ins = last_ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;){
		if ((ins = *insp) == NULL){
			cbt->next_stack[i] = NULL;
			cbt->ins_stack[i--] = insp--;
			continue;
		}

		if (ins != last_ins) {
			last_ins = ins;
			key.data = WT_INSERT_KEY(ins);
			key.size = WT_INSERT_KEY_SIZE(ins);
			match = WT_MIN(skiplow, skiphigh);
			WT_RET(__wt_compare_skip(session, collator, srch_key, &key, &cmp, &match));
		}

		/*
		* For every insert element we review, we're getting closer to a better
		* choice; update the compare field to its new value.  If we went past
		* the last item in the list, return the last one: that is used to
		* decide whether we are positioned in a skiplist.
		*/
		if (cmp > 0){			/* Keep going at this level */
			insp = &ins->next[i];
			skiplow = match;
		}
		else if (cmp < 0){		/* Drop down a level */
			cbt->next_stack[i] = ins;
			cbt->ins_stack[i--] = insp--;
			skiphigh = match;
		}
		else{ /*�ҵ��ˣ�����*/
			for (; i >= 0; i--) {
				cbt->next_stack[i] = ins->next[i];
				cbt->ins_stack[i] = &ins->next[i];
			}
		}
	}

	/*
	* For every insert element we review, we're getting closer to a better
	* choice; update the compare field to its new value.  If we went past
	* the last item in the list, return the last one: that is used to
	* decide whether we are positioned in a skiplist.
	*/
	cbt->compare = -cmp;
	cbt->ins = (ins != NULL) ? ins : last_ins;

	return 0;
}

/* ��ָ����key��ref��Ӧ��page���в��Ҷ�λ���洢��ʽΪrow store��ʽ */
int __wt_row_search(WT_SESSION_IMPL* session, WT_ITEM* srch_key, WT_REF* leaf, WT_CURSOR_BTREE* cbt, int insert)
{
	WT_BTREE *btree;
	WT_COLLATOR *collator;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *current, *descent;
	WT_ROW *rip;
	size_t match, skiphigh, skiplow;
	uint32_t base, indx, limit;
	int append_check, cmp, depth, descend_right, done;

	btree = S2BT(session);
	collator = btree->collator;
	item = &cbt->search_key;

	/* btree cursor ��λ */
	__cursor_pos_clear(cbt);

	skiphigh = skiplow = 0;

	/*
	* If a cursor repeatedly appends to the tree, compare the search key
	* against the last key on each internal page during insert before
	* doing the full binary search.
	*
	* Track if the descent is to the right-side of the tree, used to set
	* the cursor's append history.
	*/
	append_check = insert && cbt->append_tree;
	descend_right = 1;

	/*���BTREE SPLITS, ֻ�ܼ�������Ҷ�ӽڵ�, �����ܼ���������*/
	if (leaf != NULL){
		current = leaf;
		goto leaf_only;
	}

	cmp = -1;
	current = &btree->root;
	/*��internal page��������λ*/
	for (depth = 2;; ++depth){
restart:
		page = current->page;
		if (page->type != WT_PAGE_ROW_INT) /*�Ѿ���Ҷ�ӽڵ��ˣ��˳���Ҷ�ӽڵ�������*/
			break;

		WT_INTL_INDEX_GET(session, page, pindex);
		/*ֻ��һ�����ӣ�ֱ��������һ�����ӽڵ�������*/
		if (pindex->entries == 1) {
			descent = pindex->index[0];
			goto descend;
		}

		/* Fast-path appends. ׷��ʽ���룬ֻ��Ҫ��λ���һ��entry,�ж����һ��entry�Ƿ�����˲���KEY��ֵ��Χ������ǣ�ֱ�ӽ�����һ�� */
		if (append_check) {
			descent = pindex->index[pindex->entries - 1];
			__wt_ref_key(page, descent, &item->data, &item->size);
			WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
			if (cmp >= 0)
				goto descend;

			/* A failed append check turns off append checks. */
			append_check = 0;
		}

		/*�ö��ַ������ڲ�����ҳ�ڶ�λ,��λ��key��Ӧ��leaf page*/
		base = 1;
		limit = pindex->entries - 1;
		if (collator == NULL){ /*key��Χ�����Ƚ�,��ֹ�ȽϹ����������*/
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				descent = pindex->index[indx];
				__wt_ref_key(page, descent, &item->data, &item->size);

				match = WT_MIN(skiplow, skiphigh);
				cmp = __wt_lex_compare_skip(srch_key, item, &match);
				if (cmp > 0) {
					skiplow = match;
					base = indx + 1;
					--limit;
				}
				else if (cmp < 0)
					skiphigh = match;
				else
					goto descend;
			}
		}
		else{/*ͨ��collator���Ƚ�*/
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				descent = pindex->index[indx];
				__wt_ref_key(page, descent, &item->data, &item->size);

				WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
				if (cmp > 0) {
					base = indx + 1;
					--limit;
				}
				else if (cmp == 0)
					goto descend;
			}
		}
		/*��λ���洢key�ķ�Χpage ref*/
		descent = pindex->index[base - 1];

		if (pindex->entries != base - 1)
			descend_right = 0;

	descend:
		/*������һ��ҳ��ȡ����������ƣ��ȴ��ڴ�����̭���ڲ�����page,�����Ҫ��ȡ��page��splits,��ô���Ǵ��¼�����ǰ(current)��page*/
		ret = __wt_page_swap(session, current, descent, 0);
		switch (ret){
		case 0:
			current = descent;
			break;

		case WT_RESTART: /*��ȡʧ�ܣ���������*/
			skiphigh = skiplow = 0;
			goto restart;
			break;

		default:
			return ret;
		}
	}

	/*����������btree�����㼶����ô�������㼶����*/
	if (depth > btree->maximum_depth)
		btree->maximum_depth = depth;

leaf_only:
	page = current->page;
	cbt->ref = current;

	/*
	* In the case of a right-side tree descent during an insert, do a fast
	* check for an append to the page, try to catch cursors appending data
	* into the tree.
	*
	* It's tempting to make this test more rigorous: if a cursor inserts
	* randomly into a two-level tree (a root referencing a single child
	* that's empty except for an insert list), the right-side descent flag
	* will be set and this comparison wasted.  The problem resolves itself
	* as the tree grows larger: either we're no longer doing right-side
	* descent, or we'll avoid additional comparisons in internal pages,
	* making up for the wasted comparison here.  Similarly, the cursor's
	* history is set any time it's an insert and a right-side descent,
	* both to avoid a complicated/expensive test, and, in the case of
	* multiple threads appending to the tree, we want to mark them all as
	* appending, even if this test doesn't work.
	*/

	/*���ӵ���һ��page��Ӧ�ķ�֧��*/
	if (insert && descend_right){
		cbt->append_tree = 1;
		/*Ҷ��page��û�м�¼����ôֱ���ڵ�һ��entry slot�Ͻ������ݲ���*/
		if (page->pg_row_entries == 0){
			cbt->slot = WT_ROW_SLOT(page, page->pg_row_d);
			F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
			cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
		}
		else{
			/*��λ�����һ��slotλ�ã�*/
			cbt->slot = WT_ROW_SLOT(page, page->pg_row_d + (page->pg_row_entries - 1));
			cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
		}

		WT_ERR(__wt_search_insert_append(session, cbt, srch_key, &done));
		if (done) /*�Ѿ���λ���ˣ�ֱ�ӷ���*/
			return 0;

		cbt->ins_head = NULL;
	}

	/*Ҷ�ӽڵ��ϵĶ��ֲ���*/
	base = 0;
	limit = page->pg_row_entries;
	if (collator == NULL){ /* û��ָ���Ƚ�������Ĭ�ϵ��ڴ�Ƚϴ�С�����Ƚ������� */
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;
			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));
			/*match�ǵ�ǰƥ��key���ڴ�λ��*/
			match = WT_MIN(skiplow, skiphigh);
			cmp = __wt_lex_compare_skip(srch_key, item, &match);
			if (cmp > 0) {
				skiplow = match;
				base = indx + 1;
				--limit;
			}
			else if (cmp < 0)
				skiphigh = match;
			else
				goto leaf_match;
		}
	}
	else{
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;
			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));

			WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
			if (cmp > 0) {
				base = indx + 1;
				--limit;
			}
			else if (cmp == 0)
				goto leaf_match;
		}
	}

	if (0) {
	leaf_match:	
		/* ��ȫ��λ���˶�Ӧ��key��rowλ�ã�ֱ�����õ�btree cursor */
		cbt->compare = 0;
		cbt->slot = WT_ROW_SLOT(page, rip);
		return (0);
	}
	
	/*�Ѿ�����page�ĵ�һ��entry����ʾָ��������key��С�ڱ�page��С����row key*/
	if (base == 0) {
		cbt->compare = 1;
		cbt->slot = WT_ROW_SLOT(page, page->pg_row_d);

		F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
	}
	else {
		/* û�ҵ�ƥ���key,��KEY�������page�Ĵ洢��Χ֮�ڣ�ȷ����洢�ĺ�һ��slot base, ��ô�洢���KEY��ֵ��λ��Ӧ����base -1��λ�� */
		cbt->compare = -1;
		cbt->slot = WT_ROW_SLOT(page, page->pg_row_d + (base - 1));

		cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
	}

	if (WT_SKIP_FIRST(cbt->ins_head) == NULL)
		return 0;

	/*����insert list�еĶ�λ*/
	if (insert) {
		WT_ERR(__wt_search_insert_append(session, cbt, srch_key, &done));
		if (done)
			return 0;
	}

	WT_ERR(__wt_search_insert(session, cbt, srch_key));

	return 0;

	/*�����ڲ�����page*/
err:
	if (leaf != NULL)
		WT_TRET(__wt_page_release(session, current, 0));

	return ret;
}

/* ��btree cursor�����λ��btree��һ����¼λ�� */
int __wt_row_random(WT_SESSION_IMPL* session, WT_CURSOR_BTREE* cbt)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_INSERT *p, *t;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *current, *descent;

	btree = S2BT(session);

	/* ��λbtree cursorλ�� */
	__cursor_pos_clear(cbt);

restart:
	current = &btree->root;
	for (;;){
		page = current->page;
		if (page->type != WT_PAGE_ROW_INT) /* ��λ��Ҷ�ӽڵ��ˣ� ��Ҷ�ӽڵ��н��в��� */
			break;

		/* �����λһ���ڲ�����ҳ��entry�� */
		WT_INTL_INDEX_GET(session, page, pindex);
		descent = pindex->index[__wt_random(session->rnd) % pindex->entries];
		/* ��ȡ�亢�ӽڵ㣬������current page */
		if ((ret = __wt_page_swap(session, current, descent, 0)) == 0) {
			current = descent;
			continue;
		}

		/* __wt_page_swap����restart, ���ܶ�ȡ��leaf page����split, ��������һ���page�����¶�λ */
		if (ret == WT_RESTART && (ret = __wt_page_release(session, current, 0)) == 0)
			goto restart;

		return ret;
	}

	/* ����Ҷ�ӽڵ������λ */
	if (page->pg_row_entries != 0) {
		/*
		* The use case for this call is finding a place to split the
		* tree.  Cheat (it's not like this is "random", anyway), and
		* make things easier by returning the first key on the page.
		* If the caller is attempting to split a newly created tree,
		* or a tree with just one big page, that's not going to work,
		* check for that.
		*/

		/* Ҷ�ӽڵ�page����洢���м�¼�����������λ */
		cbt->ref = current;
		cbt->compare = 0;
		WT_INTL_INDEX_GET(session, btree->root.page, pindex);
		cbt->slot = pindex->entries < 2 ? __wt_random(session->rnd) % page->pg_row_entries : 0;

		/*���Ҷ�ӽڵ��ֵ������ֵ��btree cursor��search key*/
		return (__wt_row_leaf_key(session, page, page->pg_row_d + cbt->slot, &cbt->search_key, 0));
	}

	/* leaf page�Ǹո��½��ģ���ôȡ��һ��entry��insert list ���ж�λ */
	F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
	if ((cbt->ins_head = WT_ROW_INSERT_SMALLEST(page)) == NULL)
		WT_ERR(WT_NOTFOUND);

	/*
	 * ��������½���btree����insert list��¼����ô��insert list�ж�λһ���м��key��Ϊ����
	 */
	for (p = t = WT_SKIP_FIRST(cbt->ins_head);;) {
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		t = WT_SKIP_NEXT(t);
	}

	cbt->ref = current;
	cbt->compare = 0;
	cbt->ins = t;

err:
	WT_TRET(__wt_page_release(session, current, 0));
	return ret;
}



