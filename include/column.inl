/***************************************************
 * column�洢��ʽ�ļ���ʵ��
 **************************************************/

/*��inshead skip list�ж�λ��һ���պô���recno�ļ�¼λ��*/
static inline WT_INSERT* __col_insert_search_gt(WT_INSERT_HEAD* inshead, uint64_t recno)
{
	WT_INSERT *ins, **insp;
	int i;

	/* inshead������recno���ڵķ�Χ������Ҫ�������� */
	if ((ins = WT_SKIP_LAST(inshead)) == NULL)
		return NULL;

	/*ֱ���ж�recno����Ƿ��Ѿ���inshead skip list��ĩβ���������ĩβ��ֱ�ӷ���δ��λ��*/
	if (recno >= WT_INSERT_RECNO(ins))
		return NULL;

	/*skip list��λ����*/
	ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i > 0;){
		if (*insp != NULL && recno >= WT_INSERT_RECNO(*insp)) {
			ins = *insp;	/* GTE: keep going at this level */
			insp = &(*insp)->next[i];
		}
		else {
			--i;		/* LT: drop down a level */
			--insp;
		}
	}

	if (ins == NULL)
		ins = WT_SKIP_FIRST(inshead);
	while (recno >= WT_INSERT_RECNO(ins))
		ins = WT_SKIP_NEXT(ins);
	return ins;
}

/*��inshead skip list�ж�λ����recnoС��ǰһ����¼λ��*/
static inline WT_INSERT* __col_insert_search_lt(WT_INSERT_HEAD* inshead, uint64_t recno)
{
	WT_INSERT *ins, **insp;
	int i;

	/*inshead ������recno���ڵķ�Χ*/
	if ((ins = WT_SKIP_FIRST(inshead)) == NULL)
		return (NULL);

	if (recno <= WT_INSERT_RECNO(ins))
		return (NULL);

	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;){
		if (*insp != NULL && recno > WT_INSERT_RECNO(*insp)) {
			ins = *insp;	/* GT: keep going at this level */
			insp = &(*insp)->next[i];
		}
		else  {
			--i;		/* LTE: drop down a level */
			--insp;
		}
	}

	return ins;
}

/*��inshead skip list�о�ȷ��λ��recno���ڵ�λ��*/
static inline WT_INSERT* __col_insert_search_match(WT_INSERT_HEAD* inshead, uint64_t recno)
{
	WT_INSERT **insp, *ret_ins;
	uint64_t ins_recno;
	int cmp, i;

	if ((ret_ins = WT_SKIP_LAST(inshead)) == NULL)
		return NULL;

	if (recno > WT_INSERT_RECNO(ret_ins))
		return NULL;
	else if (recno == WT_INSERT_RECNO(ret_ins))
		return ret_ins;

	/*skip list��λ����*/
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;) {
		if (*insp == NULL) {
			--i;
			--insp;
			continue;
		}

		ins_recno = WT_INSERT_RECNO(*insp);
		cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;
		if (cmp == 0)			/* Exact match: return */
			return (*insp);
		else if (cmp > 0)		/* Keep going at this level */
			insp = &(*insp)->next[i];
		else {					/* Drop down a level */
			--i;
			--insp;
		}
	}

	return NULL;
}

/*Ϊ������¼�¼��λ����skip list��λ��,����skip ��·������*/
static inline WT_INSERT* __col_insert_search(WT_INSERT_HEAD* inshead, WT_INSERT*** ins_stack, WT_INSERT** next_stack, uint64_t recno)
{
	WT_INSERT **insp, *ret_ins;
	uint64_t ins_recno;
	int cmp, i;

	/* If there's no insert chain to search, we're done. */
	if ((ret_ins = WT_SKIP_LAST(inshead)) == NULL)
		return NULL;

	/*���뵽skip list������棬��������ǰ��ڵ�Ĺ�ϵstack*/
	if (recno >= WT_INSERT_RECNO(ret_ins)) {
		for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
			ins_stack[i] = (i == 0) ? &ret_ins->next[0] : (inshead->tail[i] != NULL) ? &inshead->tail[i]->next[i] : &inshead->head[i];
			next_stack[i] = NULL;
		}
		return ret_ins;
	}

	/*��skip list�м���ж�λ��������ǰ���ϵ*/
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;) {
		if ((ret_ins = *insp) == NULL) { /*�����Ѿ�������ˣ�����ǰ���ϵ�� ��Ҫ������һ��Ķ�λ*/
			next_stack[i] = NULL;
			ins_stack[i--] = insp--;
			continue;
		}

		ins_recno = WT_INSERT_RECNO(ret_ins);
		cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;
		if (cmp > 0)			/* Keep going at this level */
			insp = &ret_ins->next[i];
		else if (cmp == 0)		/* Exact match: return */
			for (; i >= 0; i--) {
				next_stack[i] = ret_ins->next[i];
				ins_stack[i] = &ret_ins->next[i];
			}
		else {				/* Drop down a level */
			next_stack[i] = ret_ins;
			ins_stack[i--] = insp--;
		}
	}
	return ret_ins;
}

/*���variable-length column store page������recnoֵ*/
static inline uint64_t __col_var_last_recno(WT_PAGE* page)
{
	WT_COL_RLE *repeat;

	if (page->pg_var_nrepeats == 0)
		return (page->pg_var_entries == 0 ? 0 : page->pg_var_recno + (page->pg_var_entries - 1));

	repeat = &page->pg_var_repeats[page->pg_var_nrepeats - 1];
	return ((repeat->recno + repeat->rle) - 1 + (page->pg_var_entries - (repeat->indx + 1)));
}

/*���fix-length column store page������recnoֵ*/
static inline uint64_t __col_fix_last_recno(WT_PAGE* page)
{
	return (page->pg_fix_entries == 0 ? 0 : page->pg_fix_recno + (page->pg_fix_entries - 1));
}

/*  */
static inline WT_COL* __col_var_search(WT_PAGE* page, uint64_t recno, uint64_t* start_recnop)
{
	WT_COL_RLE *repeat;
	uint64_t start_recno;
	uint32_t base, indx, limit, start_indx;

	/*���ַ����ж�λ*/
	for (base = 0, limit = page->pg_var_nrepeats; limit != 0; limit >>= 1){
		indx = base + (limit >> 1);

		repeat = page->pg_var_repeats + indx;
		/*��λ��recno��Ӧcolλ��ֱ�ӷ���*/
		if (recno >= repeat->recno && recno < repeat->recno + repeat->rle) {
			if (start_recnop != NULL)
				*start_recnop = repeat->recno;
			return (page->pg_var_d + repeat->indx);
		}

		if (recno < repeat->recno)
			continue;

		base = indx + 1;
		--limit;
	}

	/*���ڵ�һ��repeat�У�������ʼλ��*/
	if (base == 0) {
		start_indx = 0;
		start_recno = page->pg_var_recno;
	}
	else {
		/*���м�δ���к�recno�ص���repeat�У���repeat��recno��Ϊstartrecno*/
		repeat = page->pg_var_repeats + (base - 1);
		start_indx = repeat->indx + 1;
		start_recno = repeat->recno + repeat->rle;
	}

	/*��ѡ����repeat��У��*/
	if (recno >= start_recno + (page->pg_var_entries - start_indx))
		return NULL;

	/*ȷ�����ص�col����*/
	return page->pg_var_d + start_indx + (uint32_t)(recno - start_recno);
}









