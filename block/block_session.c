/**************************************************************************
*��session��Ӧ��WT_EXT/WT_SIZE����صĲ�������
**************************************************************************/

#include "wt_internal.h"

/*һ��ÿ��session�Ỻ���һЩ�ظ����õ�WT_EXT,��ֹ�ڴ��ظ�������ͷ�*/
typedef struct  
{
	WT_EXT*		ext_cache;			/*EXT handler���б�,��ʵ����һ�����󻺳��*/
	u_int		ext_cache_cnt;		/*EXT handler�ĸ���*/
	WT_SIZE*	sz_cache;			/*WT SIZE handler���б���*/
	u_int		sz_cache_cnt;		/*WT SIZE handler�ĸ���*/
}WT_BLOCK_MGR_SESSION;

/*����һ��skip list�����ext��Ԫ*/
static int __block_ext_alloc(WT_SESSION_IMPL* session, WT_EXT** extp)
{
	WT_EXT* ext;
	/*skip list���*/
	u_int skipdepth;

	/*���ѡ��һ��skip�����ֵ*/
	skipdepth = __wt_skip_choose_depth(session);
	/*����WT_EXT����Ԫ,��������2���������Ϊnext����ĳ��ȣ�������Ϊ�˵��������������copy on write??����Ҫȷ��*/
	WT_RET(__wt_calloc(session, 1, sizeof(WT_EXT) + skipdepth * 2 * sizeof(WT_EXT *), &ext));
	ext->depth = (uint8_t)skipdepth;
	(*extp) = ext;

	return 0;
}

int __wt_block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp)
{
	WT_EXT *ext;
	WT_BLOCK_MGR_SESSION *bms;
	u_int i;

	bms = (WT_BLOCK_MGR_SESSION *)session->block_manager;

	/*���Դ�cache list��ȡһ�������ظ����õ�WT_EXT*/
	if(bms != NULL && bms->ext_cache != NULL){
		ext = bms->ext_cache;
		bms->ext_cache = ext->next[0];

		/*��յ�ԭ��WT_EXT�ĵ����ϵ*/
		for(i = 0; i < ext->depth; ++i){
			ext->next[i] = ext->next[i + ext->depth] = NULL;
		}

		/*�����ó��еļ�����-1*/
		if (bms->ext_cache_cnt > 0)
			--bms->ext_cache_cnt;

		*extp = ext;

		return 0;
	}

	/*�����session��û�п����õ�WT_EXT,ֱ�Ӵ��ڴ��з���һ��*/
	return (__block_ext_alloc(session, extp));
}

/*Ϊһ��sessionԤ����һЩWT_EXT����cache��*/
static int __block_ext_prealloc(WT_SESSION_IMPL* session, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_EXT *ext;

	bms = (WT_BLOCK_MGR_SESSION *)session->block_manager;
	
	for(; bms->ext_cache_cnt < max; ++bms->ext_cache_cnt){
		WT_RET(__block_ext_alloc(session, &ext));
		/*���·����WT_EXT����cache��,�Ա�ʹ��*/
		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;
	}

	return 0;
}

/*�ͷ�һ��WT_EXT,���sessionû������cache���ƣ�ֱ���ͷţ����������cache���ƣ���ext�������cache��*/
void __wt_block_ext_free(WT_SESSION_IMPL *session, WT_EXT *ext)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = (WT_BLOCK_MGR_SESSION *)session->block_manager;

	if (bms == NULL)
		__wt_free(session, ext);
	else {
		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;
		++bms->ext_cache_cnt;
	}
}

/*�ͷ�session������й����WT_EXT,��������maxΪ׼.��ֹ�ڴ�ռ�ù���*/
static int __block_ext_discard(WT_SESSION_IMPL *session, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_EXT *ext, *next;

	bms = (WT_BLOCK_MGR_SESSION*)session->block_manager;
	if (max != 0 && bms->ext_cache_cnt <= max)
		return (0);

	for (ext = bms->ext_cache; ext != NULL;) {
		next = ext->next[0];
		__wt_free(session, ext);
		ext = next;

		--bms->ext_cache_cnt;
		if (max != 0 && bms->ext_cache_cnt <= max)
			break;
	}
	bms->ext_cache = ext;

	if (max == 0 && bms->ext_cache_cnt != 0)
		WT_RET_MSG(session, WT_ERROR, "incorrect count in session handle's block manager cache");

	return 0;
}

/*��ϵͳ�ڴ��Ϸ���WT_SIZE����*/
static int __block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
{
	return __wt_calloc_one(session, szp);
}

/*����һ��WT_SIZE,���session�Ļ�������п����õ�WT_SIZE�ӻ������ȡ�����û�У���ϵͳ�ڴ��з���*/
int __wt_block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = (WT_BLOCK_MGR_SESSION*)session->block_manager;

	/* Return a WT_SIZE structure for use from a cached list. */
	if (bms != NULL && bms->sz_cache != NULL) {
		(*szp) = bms->sz_cache;
		bms->sz_cache = bms->sz_cache->next[0];

		/*
		 * The count is advisory to minimize our exposure to bugs, but
		 * don't let it go negative.
		 */
		if (bms->sz_cache_cnt > 0)
			--bms->sz_cache_cnt;
		return 0;
	}

	return __block_size_alloc(session, szp);
}

/*��session��WT_SIZEԤ���䣬����Ԥ�����WT_SIZE������뻺�����*/
static int __block_size_prealloc(WT_SESSION_IMPL *session, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_SIZE *sz;

	bms = (WT_BLOCK_MGR_SESSION*)session->block_manager;

	for (; bms->sz_cache_cnt < max; ++bms->sz_cache_cnt) {
		WT_RET(__block_size_alloc(session, &sz));

		sz->next[0] = bms->sz_cache;
		bms->sz_cache = sz;
	}

	return 0;
}

/*��һ��ʹ����ϵ�WT_SIZE�ͷţ����session�Ļ�����ܻ����Ӧ��ֱ�ӷ��뻺����У�����ֱ���ͷ�*/
void __wt_block_size_free(WT_SESSION_IMPL *session, WT_SIZE *sz)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = (WT_BLOCK_MGR_SESSION *)session->block_manager;
	if (bms == NULL)
		__wt_free(session, sz);
	else {
		sz->next[0] = bms->sz_cache;
		bms->sz_cache = sz;

		++bms->sz_cache_cnt;
	}
}

/*�ͷŻ�����й����WT_SIZE���󣬷�ֹ�ڴ�ռ�ù���*/
static int __block_size_discard(WT_SESSION_IMPL *session, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_SIZE *sz, *nsz;

	bms = (WT_BLOCK_MGR_SESSION*)session->block_manager;
	if (max != 0 && bms->sz_cache_cnt <= max)
		return (0);

	for (sz = bms->sz_cache; sz != NULL;) {
		nsz = sz->next[0];
		__wt_free(session, sz);
		sz = nsz;

		--bms->sz_cache_cnt;
		if (max != 0 && bms->sz_cache_cnt <= max)
			break;
	}
	bms->sz_cache = sz;

	if (max == 0 && bms->sz_cache_cnt != 0)
		WT_RET_MSG(session, WT_ERROR, "incorrect count in session handle's block manager cache");

	return 0;
}

/*��session�Ķ��󻺳��������ͷ�*/
static int __block_manager_session_cleanup(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;

	if (session->block_manager == NULL)
		return (0);

	WT_TRET(__block_ext_discard(session, 0));
	WT_TRET(__block_size_discard(session, 0));

	__wt_free(session, session->block_manager);

	return ret;
}

/*����һ��session�Ķ��󻺳�أ���Ϊsession��WT_SIZE/WT_EXT���󻺳��Ԥ����max������*/
int __wt_block_ext_prealloc(WT_SESSION_IMPL *session, u_int max)
{
	if (session->block_manager == NULL) {
		WT_RET(__wt_calloc(session, 1, sizeof(WT_BLOCK_MGR_SESSION), &session->block_manager));
		session->block_manager_cleanup =
			__block_manager_session_cleanup;
	}
	WT_RET(__block_ext_prealloc(session, max));
	WT_RET(__block_size_prealloc(session, max));

	return 0;
}

/*���������еĶ�����࣬�ͷŵ�һЩ����*/
int __wt_block_ext_discard(WT_SESSION_IMPL *session, u_int max)
{
	WT_RET(__block_ext_discard(session, max));
	WT_RET(__block_size_discard(session, max));

	return (0);
}

