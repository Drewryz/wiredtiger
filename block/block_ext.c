/***************************************************************************
*WT_EXT��WT_SIZE��������ʵ��
*block���̿ռ�ķ������
*block�ڴ����ʵ��
***************************************************************************/

#include "wt_internal.h"

static int __block_append(WT_SESSION_IMPL *, WT_EXTLIST *, wt_off_t, wt_off_t);
static int __block_ext_overlap(WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, WT_EXT **, WT_EXTLIST *, WT_EXT **);
static int __block_extlist_dump(WT_SESSION_IMPL *, const char *, WT_EXTLIST *, int);
static int __block_merge(WT_SESSION_IMPL *, WT_EXTLIST *, wt_off_t, wt_off_t);

/*�������ÿһ������һ����Ԫ��������stack��.����ֵ�ǵ�0��ײ�����һ����Ԫ��Ҳ������������һ����Ԫ*/
static inline WT_EXT* __block_off_srch_last(WT_EXT** head, WT_EXT ***stack)
{
	WT_EXT** extp;
	WT_EXT* last;
	int i;

	last = NULL;

	for(i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i>=0;){
		if (*extp != NULL) {
			last = *extp;
			extp = &(*extp)->next[i];
		} 
		else
			stack[i--] = extp--;
	}

	return last;
}

/*�������н���offλ�ò��ң�����ʶ�������·������·����Ԫ����stack��*/
static inline void __block_off_srch(WT_EXT **head, wt_off_t off, WT_EXT ***stack, int skip_off)
{
	WT_EXT** extp;
	int i;

	for(i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;){
		if (*extp != NULL && (*extp)->off < off)
			extp = &(*extp)->next[i + (skip_off ? (*extp)->depth : 0)];
		else /*����һ����в��ң���һ�������ϸ�ȸ���,ֱ��0��Ϊֹ*/
			stack[i--] = extp--;
	}
}

/*��ö�Ӧext��С��sizeƥ����������·��*/
static inline int __block_first_srch(WT_EXT** head, wt_off_t size, WT_EXT*** stack)
{
	WT_EXT *ext;
	
	/*�������0����б���������ext->size��sizeƥ��ĵ�Ԫ,O(n)*/
	WT_EXT_FOREACH(ext, head){
		if (ext->size >= size)
			break;
	}

	if (ext == NULL)
		return 0;

	/*����ext������·����ǽ���O(logN)*/
	__block_off_srch(head, ext->off, stack, 0);

	return 1;
}

/*��WT_SIZE��������ͨ��size��λ��Ӧ��WT_SIZE,����ò���·��,���ӶȽ���O(LogN)*/
static inline void __block_size_srch(WT_SIZE** head, wt_off_t size, WT_SIZE*** stack)
{
	WT_SIZE **szp;
	int i;

	for (i = WT_SKIP_MAXDEPTH - 1, szp = &head[i]; i >= 0;)
		if (*szp != NULL && (*szp)->size < size)
			szp = &(*szp)->next[i];
		else
			stack[i--] = szp--;
}

/*����off��ӦWT_EXT�����е�ǰһ��λ�úͺ�һ��λ��*/
static inline void __block_off_srch_pair(WT_EXTLIST* el, wt_off_t off, WT_EXT** beforep, WT_EXT** afterp)
{
	WT_EXT **head, **extp;
	int i;

	*beforep =NULL;
	*afterp = NULL;

	head = el->off;

	for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;) {
		if (*extp == NULL) { /*�����Ѿ���ĩβ,������һ�����*/
			--i;
			--extp;
			continue;
		}

		if ((*extp)->off < off) {
			*beforep = *extp;
			extp = &(*extp)->next[i];
		} 
		else { /*����һ�������*/		
			*afterp = *extp;
			--i;
			--extp;
		}
	}
}

/*��WT_EXT�����в���һ��ext��Ԫ,ͬʱҲ�����һ��WT_SIZE����Ӧ��������*/
static int __block_ext_insert(WT_SESSION_IMPL* session, WT_EXTLIST* el, WT_EXT* ext)
{
	WT_EXT **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	if (el->track_size) { /*��Ҫ����size���ң�����Ҫ�Ƚ�size��Ϣ���뵽WT_SIZE������*/
		/*��ͨ��ext->size��λ����WT_SIZE�����еĲ���·��*/
		__block_size_srch(el->sz, ext->size, sstack);

		szp = *sstack[0];
		if (szp == NULL || szp->size != ext->size) {
			/*����һ��WT_SIZE���󣬲���size��WT_EXT��Ӧ��������õ�WT_SIZE�У��������������*/
			WT_RET(__wt_block_size_alloc(session, &szp));
			szp->size = ext->size;
			szp->depth = ext->depth;
			for (i = 0; i < ext->depth; ++i) {
				szp->next[i] = *sstack[i];
				*sstack[i] = szp;
			}
		}

		/*����WT_EXT��Ӧ���������,��WT_EXT�ĵķ�����2 ��skipdepth�Ļ�������0 ~ skipdepth��һ�����������ϵ��
		skipdepth ~ 2 x skipdepth��һ��������ϵ��������µ��Ǻ��ߣ���if����������ǰ��,������Ϊ�˺�WT_SIZE����һ�£�*/
		__block_off_srch(szp->off, ext->off, astack, 1);
		for (i = 0; i < ext->depth; ++i) {
			ext->next[i + ext->depth] = *astack[i];
			*astack[i] = ext;
		}
	}
	
	__block_off_srch(el->off, ext->off, astack, 0);
	for (i = 0; i < ext->depth; ++i){
		ext->next[i] = *astack[i];
		*astack[i] = ext;
	}

	/*����������*/
	++el->entries;
	el->bytes += (uint64_t)ext->size;

	/*�������һ����Ԫ��ʶ*/
	if (ext->next[0] == NULL)
		el->last = ext;

	return 0;
}

/*��off skip list�в���һ��off��Ӧ��ϵ*/
static int __block_off_insert(WT_SESSION_IMPL* session, WT_EXTLIST* el, wt_off_t off, wt_off_t size)
{
	WT_EXT *ext;

	WT_RET(__wt_block_ext_alloc(session, &ext));
	ext->off = off;
	ext->size = size;

	return __block_ext_insert(session, el, ext);
}

/*���������е�offɾ��*/
static int __block_off_remove(WT_SESSION_IMPL* session, WT_EXTLIST* el, wt_off_t off, WT_EXT** extp)
{
	WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	/*��WT_EXT��0 ~ skipdepth�����ϵ�н���ɾ��*/
	__block_off_srch(el->off, off, astack, 0);
	ext = *astack[0];
	if (ext == NULL || ext->off != off)
		goto corrupt;
	/*���й�ϵ���*/
	for (i = 0; i < ext->depth; ++i)
		*astack[i] = ext->next[i];

	/*���������WT_SIZE����������������Ӧ��ɾ��*/
	if (el->track_size) {
		__block_size_srch(el->sz, ext->size, sstack);
		szp = *sstack[0];
		if (szp == NULL || szp->size != ext->size)
			return (EINVAL);

		__block_off_srch(szp->off, off, astack, 1);
		ext = *astack[0];
		if (ext == NULL || ext->off != off)
			goto corrupt;

		for (i = 0; i < ext->depth; ++i)
			*astack[i] = ext->next[i + ext->depth];

		if (szp->off[0] == NULL) {
			for (i = 0; i < szp->depth; ++i)
				*sstack[i] = szp->next[i];

			__wt_block_size_free(session, szp);
		}
	}

	--el->entries;
	el->bytes -= (uint64_t)ext->size;

	if (extp == NULL) /*ֱ�ӽ���extp����*/
		__wt_block_ext_free(session, ext);
	else
		*extp = ext;

	if (el->last == ext)
		el->last = NULL;

	return 0;

corrupt: 
	WT_PANIC_RET(session, EINVAL, "attempt to remove non-existent offset from an extent list");
}

/*��off��Ӧ��WT_EXT���з�Χ���ѣ����ѳ�����WT_EXT,�����²���skip list��,�м��ɾ��off�����Ӧ��size������
*	ext     |---------|--------remove range------|------------------|
*		ext->off	off						off + size
* ɾ��	ext1|---------|				ext2		 |------------------|
*		   a_off								b_off
*/
int __wt_block_off_remove_overlap(WT_SESSION_IMPL *session, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
	WT_EXT *before, *after, *ext;
	wt_off_t a_off, a_size, b_off, b_size;

	WT_ASSERT(session, off != WT_BLOCK_INVALID_OFFSET);

	/*ƥ�䵽off��Ӧ��λ��*/
	__block_off_srch_pair(el, off, &before, &after);
	/*before����off��Ӧ��EXT*/
	if (before != NULL && before->off + before->size > off) {
		WT_RET(__block_off_remove(session, el, before->off, &ext));

		/*������ѵ�λ��*/
		a_off = ext->off;
		a_size = off - ext->off;
		b_off = off + size;
		b_size = ext->size - (a_size + size);
	}
	else if (after != NULL && off + size > after->off) { /*after����off��Ӧ��WT_EXT*/
		WT_RET(__block_off_remove(session, el, after->off, &ext));

		a_off = WT_BLOCK_INVALID_OFFSET;
		a_size = 0;
		b_off = off + size;
		b_size = ext->size - (b_off - ext->off);
	} 
	else{
		return (WT_NOTFOUND);
	}

	if (a_size != 0) {
		ext->off = a_off;
		ext->size = a_size;
		WT_RET(__block_ext_insert(session, el, ext));
		ext = NULL;
	}

	if (b_size != 0) {
		if (ext == NULL)
			WT_RET(__block_off_insert(session, el, b_off, b_size));
		else 
		{
			ext->off = b_off;
			ext->size = b_size;
			WT_RET(__block_ext_insert(session, el, ext));
			ext = NULL;
		}
	}

	if (ext != NULL)
		__wt_block_ext_free(session, ext);

	return 0;
}

/*����һ��block��Ӧ���ļ���С*/
static inline int __block_extend( WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp, wt_off_t size)
{
	WT_FH* fh;

	fh = block->fh;

	/*���ܴ�һ�����ļ���ʼ*/
	/*
	 * Callers of this function are expected to have already acquired any
	 * locks required to extend the file.
	 *
	 * We should never be allocating from an empty file.
	 */
	if (fh->size < block->allocsize)
		WT_RET_MSG(session, EINVAL, "file has no description information");

	/*�ļ��������ֵ�ˣ���������չ*/
	if(fh->size > (wt_off_t)INT64_MAX - size)
		WT_RET_MSG(session, WT_ERROR, "block allocation failed, file cannot grow further");

	*offp = fh->size;
	fh->size += size;

	WT_STAT_FAST_DATA_INCR(session, block_extension);
	WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "file extend %" PRIdMAX "B @ %" PRIdMAX, (intmax_t)size, (intmax_t)*offp));

	return 0;
}

/*��һ��block��Ӧ���ļ��з���һ�����ݿռ�(chunk)*/
int __wt_block_alloc(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp, wt_off_t size)
{
	WT_EXT *ext, **estack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];

	WT_ASSERT(session, block->live.avail.track_size != 0);

	WT_STAT_FAST_DATA_INCR(session, block_alloc);

	/*sizeû�к�blockҪ��Ķ��뷽ʽ���룬��������һ������*/
	if(size % block->allocsize != 0){
		WT_RET_MSG(session, EINVAL, "cannot allocate a block size %" PRIdMAX " that is not a multiple of the allocation size %" PRIu32, (intmax_t)size, block->allocsize);
	}

	/*block���ݳ��Ȼ�δ�ﵽҪ�������ݿռ�ĳ��ȣ���block��Ӧ���ļ���������,��ʱ���������ݿռ�*/
	if(block->live.avail.bytes < (uint64_t)size)
		goto append;

	if (block->allocfirst){
		/*����off�����е�һ���ܴ���size��С��ext����λ��*/
		if (!__block_first_srch(block->live.avail.off, size, estack))
			goto append;
		ext = *estack[0];
	}
	else{
		/*��WT_SIZE�����������ܴ���size���ȵ�ext����*/
		__block_size_srch(block->live.avail.sz, size, sstack);
		if ((szp = *sstack[0]) == NULL) {
append:
			WT_RET(__block_extend(session, block, offp, size));
			WT_RET(__block_append(session, &block->live.alloc, *offp, (wt_off_t)size));

			return 0;
		}

		ext = szp->off[0];
	}

	/*��ext��avail�б���ɾ������ʾ���ext��ռ����*/
	WT_RET(__block_off_remove(session, &block->live.avail, ext->off, &ext));
	*offp = ext->off;

	/*����û�г���ext���󳤶ȣ�ȥ���Ѿ�ռ�õģ���δռ�õ����·��뵽live.avail�м���ʹ��*/
	if(ext->size > size){
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
			"allocate %" PRIdMAX " from range %" PRIdMAX "-%"
			PRIdMAX ", range shrinks to %" PRIdMAX "-%" PRIdMAX,
			(intmax_t)size,
			(intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
			(intmax_t)(ext->off + size),
			(intmax_t)(ext->off + size + ext->size - size)));

		ext->off += size;
		ext->size -= size;
		WT_RET(__block_ext_insert(session, &block->live.avail, ext));
	}
	else{ /*�պó������ext,�ͷŵ����ext����*/
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "allocate range %" PRIdMAX "-%" PRIdMAX,
			(intmax_t)ext->off, (intmax_t)(ext->off + ext->size)));

		__wt_block_ext_free(session, ext);
	}

	/*���»�õ��ļ����ݿռ�ϲ���live.alloc�б���*/
	WT_RET(__block_merge(session, &block->live.alloc, *offp, (wt_off_t)size));

	return 0;
}

/*�ͷ�һ��block addr��Ӧ�����ݿռ�(chunk)*/
int __wt_block_free(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size)
{
	WT_DECL_RET;
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(addr_size);
	WT_STAT_FAST_DATA_INCR(session, block_free);

	/*��addr���off /size /checksum*/
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
		"free %" PRIdMAX "/%" PRIdMAX, (intmax_t)offset, (intmax_t)size));
	/*ΪsessionԤ����5��WT_EXT,�����ظ�����WT_EXT����*/
	WT_RET(__wt_block_ext_prealloc(session, 5));

	__wt_spin_lock(session, &block->live_lock);
	ret = __wt_block_off_free(session, block, offset, (wt_off_t)size);
	__wt_spin_unlock(session, &block->live_lock);
}

/*��block��Ӧ���ļ����ͷŵ�(off, size)λ�õ����ݿռ�(chunk)*/
int __wt_block_off_free(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t offset, wt_off_t size)
{
	WT_DECL_RET;

	/*���з�Χ�ͷţ��п�������ĳ��WT_EXT��Χ֮��*/
	ret = __wt_block_off_remove_overlap(session, &block->live.alloc, offset, size)
	if (ret == 0) /*��alloc���д��ڣ���ʾ�Ǵ�avail�з���ģ��Ⱥϲ���avail�У��ظ�ʹ��*/
		ret = __block_merge(session, &block->live.avail, offset, (wt_off_t)size);
	else if(ret == WT_NOTFOUND) /*��alloc��û���ҵ���ֱ�Ӻϲ��������Ŀռ�������*/
		ret = __block_merge(session, &block->live.discard, offset, (wt_off_t)size);

	return ret;
}

/*���alloc��dicard���ص����֣����Ѿ�ʹ�õĲ����Ƶ�checkpoint��avail������*/
int __wt_block_extlist_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
{
	WT_EXT *alloc, *discard;

	alloc = ci->alloc.off[0];
	discard = ci->discard.off[0];

	while(alloc != NULL && discard != NULL){
		/*��alloc��discard��off��Χһ��*/
		if (alloc->off + alloc->size <= discard->off) {
			alloc = alloc->next[0];
			continue;
		}

		if (discard->off + discard->size <= alloc->off) {
			discard = discard->next[0];
			continue;
		}

		WT_RET(__block_ext_overlap(session, block, &ci->alloc, &alloc, &ci->discard, &discard));
	}

	return 0;
}

static int __block_ext_overlap(WT_SESSION_IMPL * session, WT_BLOCK *block, WT_EXTLIST *ael, WT_EXT **ap, WT_EXTLIST *bel, WT_EXT **bp)
{
	WT_EXT *a, *b, **ext;
	WT_EXTLIST *avail, *el;
	wt_off_t off, size;

	avail = &block->live.ckpt_avail;

		/*
	 * The ranges overlap, choose the range we're going to take from each.
	 *
	 * We can think of the overlap possibilities as 11 different cases:
	 *
	 *		AAAAAAAAAAAAAAAAAA
	 * #1		BBBBBBBBBBBBBBBBBB		ranges are the same
	 * #2	BBBBBBBBBBBBB				overlaps the beginning
	 * #3			BBBBBBBBBBBBBBBB	overlaps the end
	 * #4		BBBBB				B is a prefix of A
	 * #5			BBBBBB			B is middle of A
	 * #6			BBBBBBBBBB		B is a suffix of A
	 *
	 * and:
	 *
	 *		BBBBBBBBBBBBBBBBBB
	 * #7	AAAAAAAAAAAAA				same as #3
	 * #8			AAAAAAAAAAAAAAAA	same as #2
	 * #9		AAAAA				A is a prefix of B
	 * #10			AAAAAA			A is middle of B
	 * #11			AAAAAAAAAA		A is a suffix of B
	 *
	 *
	 * By swapping the arguments so "A" is always the lower range, we can
	 * eliminate cases #2, #8, #10 and #11, and only handle 7 cases:
	 *
	 *		AAAAAAAAAAAAAAAAAA
	 * #1		BBBBBBBBBBBBBBBBBB		ranges are the same
	 * #3			BBBBBBBBBBBBBBBB	overlaps the end
	 * #4		BBBBB				B is a prefix of A
	 * #5			BBBBBB			B is middle of A
	 * #6			BBBBBBBBBB		B is a suffix of A
	 *
	 * and:
	 *
	 *		BBBBBBBBBBBBBBBBBB
	 * #7	AAAAAAAAAAAAA				same as #3
	 * #9		AAAAA				A is a prefix of B
	 */

	a = *ap;
	b = *bp;
	if (a->off > b->off) {				/* Swap,Ϊ�˼����ظ����룬��a��b������ݽ���*/
		b = *ap;
		a = *bp;
		ext = ap; ap = bp; bp = ext;
		el = ael; ael = bel; bel = el;
	}

	if(a->off == b->off){ /*case 1, 4, 9*/
		if(a->size == b->size){ /*case 1, AB��ͬ*/
			*ap = (*ap)->next[0];
			*bp = (*bp)->next[0];
			/*ֱ�ӽ�A��B���ص������Ƶ�checkpint avail list��*/
			WT_RET(__block_merge(session, avail, b->off, b->size));

			WT_RET(__block_off_remove(session, ael, a->off, NULL));
			WT_RET(__block_off_remove(session, bel, b->off, NULL));
		}
		else if(a->size > b->size){ /*case 4, B��A��һ���֣�����A��ǰ׺����*/
			/*ɾ����A��ǰ׺���֣�����ʣ�����Ϊa��(off,size)��Ϊ�µ�ext���²��뵽ael������*/
			WT_RET(__block_off_remove(session, ael, a->off, &a));
			a->off += b->size;
			a->size -= b->size;
			WT_RET(__block_ext_insert(session, ael, a));

			/*��B������bel��ɾ���������뵽checkpoint avail list��*/
			*bp = (*bp)->next[0];
			WT_RET(__block_merge(session, avail, b->off, b->size));
			WT_RET(__block_off_remove(session, bel, b->off, NULL));
		}
		else{ /*case 9*/
			/*A��B��һ����,����A��B��ǰ׺����,���ص����ִ�bel��ɾ��*/
			WT_RET(__block_off_remove(session, bel, b->off, &b));
			b->off += a->size;
			b->size -= a->size;
			WT_RET(__block_ext_insert(session, bel, b));

			/*��A��ael��ɾ�������ϲ���checkpoint avail list��*/
			*ap = (*ap)->next[0];
			WT_RET(__block_merge(session, avail, a->off, a->size));
			WT_RET(__block_off_remove(session, ael, a->off, NULL));
		}
	}
	else if (a->off + a->size == b->off + b->size) { /*case 6��B��A�ĺ�׺����*/
		/*��������a�ĳ��ȼ��ɣ�Ȼ�����²��뵽ael��*/
		WT_RET(__block_off_remove(session, ael, a->off, &a));
		a->size -= b->size;
		WT_RET(__block_ext_insert(session, ael, a));

		/*��B������bel��ɾ���������뵽checkpoint avail list��*/
		*bp = (*bp)->next[0];
		WT_RET(__block_merge(session, avail, b->off, b->size));
		WT_RET(__block_off_remove(session, bel, b->off, NULL));
	}
	else if(a->off + a->size < b->off + b->size){ /*case 3, 7, B��ǰ�벿�ֺ�A�ĺ�벿������*/
		/*���ص�������Ϊ�µ�EXT���뵽checkpoint avail list��*/
		off = b->off;
		size = (a->off + a->size) - b->off;
		WT_RET(__block_merge(session, avail, off, size));

		/*��A��ɾ�����ص�����*/
		WT_RET(__block_off_remove(session, ael, a->off, &a));
		a->size -= size;
		WT_RET(__block_ext_insert(session, ael, a));

		/*��B��ɾ�����ص�����*/
		WT_RET(__block_off_remove(session, bel, b->off, &b));
		b->off += size;
		b->size -= size;
		WT_RET(__block_ext_insert(session, bel, b));
	}
	else{ /*case 5,B��A���м䲿��*/
		/*������沿�ֵ�WT_EXT����λ��*/
		off = b->off + b->size;
		size = (a->off + a->size) - off;

		/*��A�Ĳ���B�ظ�ǰ�벿����Ϊһ��ext������뵽ael��*/
		WT_RET(__block_off_remove(session, ael, a->off, &a));
		a->size = b->off - a->off;
		WT_RET(__block_ext_insert(session, ael, a));
		/*��A��B���ص���벿�ֺϲ���ael�У����ﲻʹ�õ�����EXT,�п���ael�к�*/
		WT_RET(__block_merge(session, ael, off, size));
	}

	return 0;
}

/*�ϲ�a��b������������浽b��*/
int __wt_block_extlist_merge(WT_SESSION_IMPL *session, WT_EXTLIST *a, WT_EXTLIST *b)
{
	WT_EXT *ext;
	WT_EXTLIST tmp;
	u_int i;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "merging %s into %s", a->name, b->name));
	/*���a����Ԫ�ر�b�࣬����ab��λ��*/
	if (a->track_size == b->track_size && a->entries > b->entries){
		/*����a b����*/
		tmp = *a;
		a->bytes = b->bytes;
		b->bytes = tmp.bytes;
		a->entries = b->entries;
		b->entries = tmp.entries;
		/*����a b����ָ�뽻��*/
		for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
			a->off[i] = b->off[i];
			b->off[i] = tmp.off[i];
			a->sz[i] = b->sz[i];
			b->sz[i] = tmp.sz[i];
		}
	}

	/*��a�ϲ���b��*/
	WT_EXT_FOREACH(ext, a->off){
		WT_RET(__block_merge(session, b, ext->off, ext->size));
	}

	return 0;
}

/*��block��alloclist����appendһ�����ݿռ䳤��(off,size)*/
static int __block_append(WT_SESSION_IMPL *session, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
	WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
	u_int i;

	WT_ASSERT(session, el->track_size == 0);

	ext = el->last;
	/*���һ��ext������off��λ��*/
	if (ext != NULL && ext->off + ext->size == off)
		ext->size += size; /*ֱ������ext��size����*/
	else{
		/*��λ��el->off���һ��ext*/
		ext = __block_off_srch_last(el->off, astack);
		/*�������һ��ext��ĩβ��offƫ��,Ҳ����ֱ������*/
		if (ext != NULL && ext->off + ext->size == off)
			ext->size += size;
		else{ /*offһ������ext->off + size*/
			/*��������һ���µ�ext�������뵽ef��off������*/
			WT_RET(__wt_block_ext_alloc(session, &ext));
			ext->off = off;
			ext->size = size;
			/*������˳����룬����ֻ��Ҫ�������һ����Ԫ�ĺ��漴��*/
			for (i = 0; i < ext->depth; ++i)
				*astack[i] = ext;

			++el->entries;
		}

		el->last = ext;
	}

	el->bytes += (uint64_t)size;

	return 0;
}

/*��(off,size)���ݿռ��Ӧ��ϵ�ϲ���el��*/
int __wt_block_insert_ext(WT_SESSION_IMPL *session, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
	return __block_merge(session, el, off, size);
}

/*��(off,size)���ݿռ��Ӧ��ϵ�ϲ���el��*/
static int __block_merge(WT_SESSION_IMPL* session, WT_EXTLIST* el, wt_off_t off, wt_off_t size)
{
	WT_EXT *ext, *after, *before;

	/*���off��el�����еĶ�Ӧ��ϵ*/
	__block_off_srch_pair(el, off, &before, &after);

	if(before != NULL){
		/*(off,size)����before�ռ�֮�ڵ�,���ǲ����ܵģ������ǳ��������BUG,�����׳�һ���쳣*/
		if (before->off + before->size > off){
			WT_PANIC_RET(session, EINVAL, "%s: existing range %" PRIdMAX "-%" PRIdMAX  "overlaps with merge range %" PRIdMAX "-%" PRIdMAX,
				el->name, (intmax_t)before->off, (intmax_t)(before->off + before->size),
				(intmax_t)off, (intmax_t)(off + size));
		}

		/*off���ǽ�����before��ĩβ��Ҳ����ζ�Ų���Ҫ�ϲ���before��*/
		if(before->off + before->size != off)
			before = NULL;
	}

	if(after != NULL){
		/*(off,size)����after֮�ڣ������ǳ���Ĵ����׳�һ���쳣*/
		if (off + size > after->off)
			WT_PANIC_RET(session, EINVAL,
			"%s: merge range %" PRIdMAX "-%" PRIdMAX
			" overlaps with existing range %" PRIdMAX
			"-%" PRIdMAX,
			el->name,
			(intmax_t)off, (intmax_t)(off + size),
			(intmax_t)after->off,
			(intmax_t)(after->off + after->size));

		/*(off,size)�����ڽ�����after��ͷ��Ҳ������ϲ���after��*/
		if (off + size != after->off)
			after = NULL;
	}
	/*�Ȳ���before����Ҳ����after������˵���Ƕ�������һ��ext,������Ϣ��ʾ�����Ӧ����Ϊ�˴��̵�˳���д�������*/
	if (before == NULL && after == NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "%s: insert range %" PRIdMAX "-%" PRIdMAX,
			el->name, (intmax_t)off, (intmax_t)(off + size)));

		return __block_off_insert(session, el, off, size);
	}

	/*��after���кϲ�*/
	if (before == NULL){
		WT_RET(__block_off_remove(session, el, after->off, &ext));
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
			"%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %" PRIdMAX "-%" PRIdMAX,
			el->name, (intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
			(intmax_t)off, (intmax_t)(off + ext->size + size)));

		ext->off = off;
		ext->size += size;
	}
	else{ /*��before��after���кϲ�*/
		if (after != NULL) { /*before��current��after�������ģ�������EXT���кϲ�,���if�Ǻϲ�after*/
			size += after->size;
			WT_RET(__block_off_remove(session, el, after->off, NULL));
		}

		/*�ϲ�before*/
		WT_RET(__block_off_remove(session, el, before->off, &ext));

		WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %"
			PRIdMAX "-%" PRIdMAX, el->name,
			(intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
			(intmax_t)ext->off,
			(intmax_t)(ext->off + ext->size + size)));

		ext->size += size;
	}
	/*���ϲ����ext���뵽ext skiplist��*/
	return __block_ext_insert(session, el, ext);
}

/*��ȡblock��avail ext����el��,����el.off/el.size���ж�ȡ�������ݵĳ��Ȳ��ܳ���ckpt_size*/
int __wt_block_extlist_read_avail(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t ckpt_size)
{
	WT_DECL_RET;

	if (el->offset == WT_BLOCK_INVALID_OFFSET)
		return 0;

	/*��e*/
	WT_ERR(__wt_block_extlist_read(session, block, el, ckpt_size));

	WT_ERR_NOTFOUND_OK(__wt_block_off_remove_overlap(session, el, el->offset, el->size));

err:
	return ret;
}

#define	WT_EXTLIST_READ(p, v) do {					\
	uint64_t _v;									\
	WT_ERR(__wt_vunpack_uint(&(p), 0, &_v));		\
	(v) = (wt_off_t)_v;								\
} while (0)

/*�Ӵ����϶�ȡһ��ext list����ext������Ϣ�������뵽ext list����,����൱��entry��������Ϣ��*/
int __wt_block_extlist_read(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t ckpt_size)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	wt_off_t off, size;
	int (*func)(WT_SESSION_IMPL *, WT_EXTLIST *, wt_off_t, wt_off_t);
	const uint8_t *p;

	if(el->offset == WT_BLOCK_INVALID_OFFSET)
		return 0;

	WT_RET(__wt_scr_alloc(session, el->size, &tmp));
	/*��block��Ӧ���ļ��ж�ȡһ�����ݿռ�鵽tmp�Ļ�������*/
	WT_ERR(__wt_block_read_off(session, block, tmp, el->offset, el->size, el->cksum));

	/*���block headerָ��λ��*/
	p = WT_BLOCK_HEADER_BYTE(tmp->mem);
	/*ǰ2��uint64_t��ֵ��extlistħ��У��ֵ��һ��0ֵ��size*/
	/*��ȡoffֵ*/
	WT_EXTLIST_READ(p, off);
	/*��ȡsizeֵ*/
	WT_EXTLIST_READ(p, size);

	/*���кϷ���У��*/
	if (off != WT_BLOCK_EXTLIST_MAGIC || size != 0)
		goto corrupted;

	func = el->track_size == 0 ? __block_append : __block_merge;
	for(;;){
		WT_EXTLIST_READ(p, off);
		WT_EXTLIST_READ(p, size);
		if(off == WT_BLOCK_INVALID_OFFSET)
			break;

		/*(off,size)�ĺϷ���У��,off��sizeһ����block->allocsize��������off + size��ƫ�Ʊ����ڼ���λ�÷�Χ��*/
		if (off < block->allocsize || off % block->allocsize != 0 ||
			size % block->allocsize != 0 || off + size > ckpt_size){
corrupted:		
			WT_PANIC_RET(session, WT_ERROR, "file contains a corrupted %s extent list, range %" PRIdMAX "-%" PRIdMAX " past end-of-file",
				el->name, (intmax_t)off, (intmax_t)(off + size));
		}
		/*����(off,size)��ext�������*/
		WT_ERR(func(session, el, off, size));
	}

	if (WT_VERBOSE_ISSET(session, WT_VERB_BLOCK))
		WT_ERR(__block_extlist_dump(session, "read extlist", el, 0));

err:
	__wt_scr_free(session, &tmp);
	return ret;
}

#define	WT_EXTLIST_WRITE(p, v)		WT_ERR(__wt_vpack_uint(&(p), 0, (uint64_t)(v)))

/*��һ��ext list��������Ϣд�뵽block��Ӧ�ļ���*/
int __wt_block_extlist_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, WT_EXTLIST *additional)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_EXT *ext;
	WT_PAGE_HEADER *dsk;
	size_t size;
	uint32_t entries;
	uint8_t *p;

	if(WT_VERBOSE_ISSET(session, WT_VERB_BLOCK))
		WT_RET(__block_extlist_dump(session, "write extlist", el, 0));

	/*����entry����*/
	entries = el->entries + (additional == NULL ? 0 : additional->entries);
	if (entries == 0) { /*ext listû���κ���Ч�ĵ�Ԫ��ֱ�ӷ���*/
		el->offset = WT_BLOCK_INVALID_OFFSET;
		el->cksum = el->size = 0;
		return 0;
	}

	/*���㻺�����Ĵ�С������һ�����Ӧ�Ļ�����*/
	size = (entries + 2) * 2 * WT_INTPACK64_MAXSIZE; /*ǰ����һ����ħ��У��ֵ + size��ֵ��������entry�� + 2��*/
	WT_RET(__wt_block_write_size(session, block, &size));
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/*����ҳͷλ��*/
	dsk = (WT_PAGE_HEADER)(tmp->mem);
	memset(dsk, 0, WT_BLOCK_HEADER_BYTE_SIZE);
	dsk->type = WT_PAGE_BLOCK_MANAGER;

	/*����block header��ָ��block��������ַ*/
	p = WT_BLOCK_HEADER_BYTE(dsk);
	/*��дһ��extlist��ħ��У����*/
	WT_EXTLIST_WRITE(p, WT_BLOCK_EXTLIST_MAGIC);	/* Initial value */
	/*���һ��У��ֵ0*/
	WT_EXTLIST_WRITE(p, 0);
	/*��ext��λ����Ϣд�뵽��������*/
	WT_EXT_FOREACH(ext, el->off) {		
		WT_EXTLIST_WRITE(p, ext->off);
		WT_EXTLIST_WRITE(p, ext->size);
	}

	/*�����ӵ�ext listҲд�뵽��������*/
	if (additional != NULL){
		WT_EXT_FOREACH(ext, additional->off) {	
			WT_EXTLIST_WRITE(p, ext->off);
			WT_EXTLIST_WRITE(p, ext->size);
		}
	}

	/*д��һ��������ext����,Ϊ�˶�������*/
	WT_EXTLIST_WRITE(p, WT_BLOCK_INVALID_OFFSET);
	WT_EXTLIST_WRITE(p, 0);

	/*�������ݳ���*/
	dsk->u.datalen = WT_PTRDIFF32(p, WT_BLOCK_HEADER_BYTE(dsk));
	/*��������page�ĳ���*/
	tmp->size = WT_PTRDIFF32(p, dsk);
	dsk->mem_size = WT_PTRDIFF32(p, dsk);

	/*������д�������,������off/size/checksum*/
	WT_ERR(__wt_block_write_off(session, block, tmp, &el->offset, &el->size, &el->cksum, 1, 1));
	/*��el��live.alloc���Ƴ�*/
	WT_TRET(__wt_block_off_remove_overlap(session, &block->live.alloc, el->offset, el->size));

	WT_ERR(__wt_verbose(session, WT_VERB_BLOCK, "%s written %" PRIdMAX "/%" PRIu32, el->name, (intmax_t)el->offset, el->size));

err:
	__wt_scr_free(session, &tmp);
	return ret;
}

/*��block��Ӧ���ļ���С����С�Ĵ�СΪel�����һ��ext�Ĵ�С���൱�ڽ����һ��extָ������ݱ��Ƴ�*/
int __wt_block_extlist_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
	WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
	WT_FH *fh;
	wt_off_t orig, size;

	fh = block->fh;
	/*�ҵ�el�����һ��ext����*/
	ext = __block_off_srch_last(el->off, astack);
	if (ext == NULL)
		return 0;

	WT_ASSERT(session, ext->off + ext->size <= fh->size);
	if (ext->off + ext->size < fh->size) /*���ext���е����ݻ�û�г����ļ�����ʾ�ļ�������truncate����*/
		return 0;

	orig = fh->size;
	size = ext->off;
	/*��ext��el���Ƴ�*/
	WT_RET(__block_off_remove(session, el, size, NULL));
	/*���ļ���Сȥ����ext�Ĵ�С����Ϊ�ļ���truncate��ext������*/
	fh->size = size;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "truncate file from %" PRIdMAX " to %" PRIdMAX, (intmax_t)orig, (intmax_t)size));

	/*�ļ���С*/
	WT_RET_BUSY_OK(__wt_ftruncate(session, block->fh, size));

	return 0;
}

/*��ʼ��һ��WT_EXT�������*/
int __wt_block_extlist_init(WT_SESSION_IMPL *session, WT_EXTLIST *el, const char *name, const char *extname, int track_size)
{
	size_t size;

	WT_CLEAR(*el);

	size = (name == NULL ? 0 : strlen(name)) + strlen(".") + (extname == NULL ? 0 : strlen(extname) + 1);
	WT_RET(__wt_calloc_def(session, size, &el->name));

	snprintf(el->name, size, "%s.%s", name == NULL ? "" : name, extname == NULL ? "" : extname);

	el->offset = WT_BLOCK_INVALID_OFFSET;
	el->track_size = track_size;

	return 0;
}

/*�ͷ�һ��ext list����*/
void __wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el)
{
	WT_EXT *ext, *next;
	WT_SIZE *szp, *nszp;

	__wt_free(session, el->name);
	/*�ͷ�ext skip list*/
	for (ext = el->off[0]; ext != NULL; ext = next) {
		next = ext->next[0];
		__wt_free(session, ext);
	}
	/*�ͷ�size skip list*/
	for (szp = el->sz[0]; szp != NULL; szp = nszp) {
		nszp = szp->next[0];
		__wt_free(session, szp);
	}
	/*���el���ڴ�����*/
	WT_CLEAR(*el);
}

/*��ext list dump����*/
static int __block_extlist_dump(WT_SESSION_IMPL *session, const char *tag, WT_EXTLIST *el, int show_size)
{
	WT_EXT *ext;
	WT_SIZE *szp;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "%s: %s: %" PRIu64 " bytes, by offset:%s",
		tag, el->name, el->bytes, el->entries == 0 ? " [Empty]" : ""));

	if (el->entries == 0)
		return (0);

	WT_EXT_FOREACH(ext, el->off){
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "\t{%" PRIuMAX "/%" PRIuMAX "}",
		(uintmax_t)ext->off, (uintmax_t)ext->size));
	}

	if (!show_size)
		return 0;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
		"%s: %s: by size:%s",
		tag, el->name, el->entries == 0 ? " [Empty]" : ""));
	if (el->entries == 0)
		return (0);

	/*dump size skip list*/
	WT_EXT_FOREACH(szp, el->sz) {
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "\t{%" PRIuMAX "}", (uintmax_t)szp->size));
		WT_EXT_FOREACH_OFF(ext, szp->off)
			WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "\t\t{%" PRIuMAX "/%" PRIuMAX "}", (uintmax_t)ext->off, (uintmax_t)ext->size));
	}

	return 0;
}
