/***************************************************************************
*block�Ĵ��������ٲ���,block header��Ϣ�Ķ�д����
***************************************************************************/
#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL* session, WT_BLOCK* block);

/*����һ��block manager��Ӧ���ļ�*/
int __wt_block_manager_truncate(WT_SESSION_IMPL *session, const char *filename, uint32_t allocsize)
{
	WT_DECL_RET;
	WT_FH *fh;

	/*���ļ�filename������ļ�����*/
	WT_RET(__wt_open(session, filename, 0, 0, WT_FILE_TYPE_DATA, &fh));
	WT_ERR(__wt_ftruncate(session, fh, (wt_off_t)0));

	/*д���ļ���һЩԪ��������,������������*/
	WT_ERR(__wt_desc_init(session, fh, allocsize));
	WT_ERR(__wt_fsync(session, fh));

err:
	WT_TRET(__wt_close(session, &fh));

	return ret;
}

/*Ϊblock manager����һ���ļ�����д�������Ԫ������Ϣ*/
int __wt_block_manager_create(WT_SESSION_IMPL *session, const char *filename, uint32_t allocsize)
{
	WT_DECL_RET;
	WT_FH *fh;
	char *path;

	/* Create the underlying file and open a handle. */
	WT_RET(__wt_open(session, filename, 1, 1, WT_FILE_TYPE_DATA, &fh));

	/*д��block file��Ҫ��Ԫ������Ϣ������д�����������*/
	ret = __wt_desc_init(session, fh, allocsize);
	WT_TRET(__wt_fsync(session, fh));
	WT_TRET(__wt_close(session, &fh));

	/*�������Ҫcheckpoint��������Ҫ���ļ�Ŀ¼�����ļ�Ҳˢ��*/
	if (ret == 0 && F_ISSET(S2C(session), WT_CONN_CKPT_SYNC) &&
	    (ret = __wt_filename(session, filename, &path)) == 0) {
		ret = __wt_directory_sync(session, path);
		__wt_free(session, path);
	}

	/*���ʧ�ܣ�ɾ��ǰ�洴�����ļ�*/
	if (ret != 0)
		WT_TRET(__wt_remove(session, filename));

	return ret;
}

/*��conn���ͷ�һ��block�ڴ����*/
static int __block_destroy(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	uint64_t bucket;

	conn = S2C(session);
	bucket = block->name_hash % WT_HASH_ARRAY_SIZE;
	/*��block��connection��queue��ɾ��*/
	WT_CONN_BLOCK_REMOVE(conn, block, bucket);

	if (block->name != NULL)
		__wt_free(session, block->name);

	if (block->fh != NULL)
		WT_TRET(__wt_close(session, &block->fh));

	__wt_spin_destroy(session, &block->live_lock);

	__wt_overwrite_and_free(session, block);

	return ret;
}

void __wt_block_configure_first_fit(WT_BLOCK* block, int on)
{
	/*
	 * Switch to first-fit allocation so we rewrite blocks at the start of
	 * the file; use atomic instructions because checkpoints also configure
	 * first-fit allocation, and this way we stay on first-fit allocation
	 * as long as any operation wants it.
	 */
	if (on)
		(void)WT_ATOMIC_ADD4(block->allocfirst, 1);
	else
		(void)WT_ATOMIC_SUB4(block->allocfirst, 1);
}

/*Ϊsession��������һ��block����*/
int __wt_block_open(WT_SESSION_IMPL *session, const char *filename, const char *cfg[], int forced_salvage, 
					int readonly, uint32_t allocsize, WT_BLOCK **blockp)
{
	WT_BLOCK *block;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	uint64_t bucket, hash;

	WT_TRET(__wt_verbose(session, WT_VERB_BLOCK, "open: %s", filename));

	/*ȷ��filename��Ӧ��block��conn->queue�е�bucket���*/
	conn = S2C(session);
	*blockp = NULL;
	hash = __wt_hash_city64(filename, strlen(filename));
	bucket = hash % WT_HASH_ARRAY_SIZE;

	/*�ж�filename�ļ���block�Ƿ��Ѿ���conn->queue��,����ڣ�����Ҫ������ֱ�ӷ��ش��ڵ�block����
	 *block_lock��Ϊ�˱���conn�����block���õ�*/
	__wt_spin_lock(session, &conn->block_lock);
	SLIST_FOREACH(block, &conn->blockhash[bucket], hashl) {
		if (strcmp(filename, block->name) == 0) {
			++block->ref;
			*blockp = block;

			__wt_spin_unlock(session, &conn->block_lock);
			return 0;
		}
	}

	/*����һ��block����*/
	WT_ERR(__wt_calloc_one(session, &block));
	block->ref = 1;
	WT_CONN_BLOCK_INSERT(conn, block, bucket);

	/*����block��name�Ͷ��볤��*/
	WT_ERR(__wt_strdup(session, filename, &block->name));
	block->name_hash = hash;
	block->allocsize = allocsize;

	/*��ȡ���ã�������block_allocation��ȷ��allocfirst�ĳ�ʼֵ*/
	WT_ERR(__wt_config_gets(session, cfg, "block_allocation", &cval));
	block->allocfirst = WT_STRING_MATCH("first", cval.str, cval.len) ? 1 : 0;

	/*���ô����ļ�page cache��ʽ��direct io��ʽ�ǲ��ܼ��ݵ�*/
	if (conn->direct_io && block->os_cache_max)
		WT_ERR_MSG(session, EINVAL, "os_cache_max not supported in combination with direct_io");
	
	/*��ȡ�����е���������ݳ���*/
	WT_ERR(__wt_config_gets(session, cfg, "os_cache_dirty_max", &cval));
	block->os_cache_dirty_max = (size_t)cval.val;

	/*direct io�ǲ�����os �����ļ���ҳ���ڵ�*/
	if (conn->direct_io && block->os_cache_dirty_max)
		WT_ERR_MSG(session, EINVAL, "os_cache_dirty_max not supported in combination with direct_io");

	/*��filename�ļ�*/
	WT_ERR(__wt_open(session, filename, 0, 0, readonly ? WT_FILE_TYPE_CHECKPOINT : WT_FILE_TYPE_DATA, &block->fh));

	/*��ʼ��live_lock*/
	WT_ERR(__wt_spin_init(session, &block->live_lock, "block manager"));

	/*��Salvage�����⣬����Ҫ��ȡ�ļ���ʼ��������Ϣ��block��*/
	if (!forced_salvage)
		WT_ERR(__desc_read(session, block));

	*blockp = block;
	__wt_spin_unlock(session, &conn->block_lock);

	return 0;

err:
	WT_TRET(__block_destroy(session, block));
	__wt_spin_unlock(session, &conn->block_lock);

	return ret;
}

/*�ر�session�е�һ��block,�п��ܻ����block����*/
int __wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	if (block == NULL)				/* Safety check */
		return (0);

	conn = S2C(session);

	WT_TRET(__wt_verbose(session, WT_VERB_BLOCK, "close: %s", block->name == NULL ? "" : block->name ));

	__wt_spin_lock(session, &conn->block_lock);

	/*���ü�����Ϊ���ӳ��ͷţ������ڵ��������ʱ�������ط������������block*/
	if (block->ref == 0 || --block->ref == 0)
		WT_TRET(__block_destroy(session, block));

	__wt_spin_unlock(session, &conn->block_lock);

	return ret;
}

/*д��һ��block header��Ϣ*/
int __wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh, uint32_t allocsize)
{
	WT_BLOCK_DESC *desc;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	/*����buf��С���룬Ϊ������д�����*/
	WT_RET(__wt_scr_alloc(session, allocsize, &buf));
	memset(buf->mem, 0, allocsize);

	desc = (WT_BLOCK_DESC*)(buf->mem);
	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;

	/* Update the checksum. */
	desc->cksum = __wt_cksum(desc, allocsize);

	ret = __wt_write(session, fh, (wt_off_t)0, (size_t)allocsize, desc);

	__wt_scr_free(session, &buf);

	return ret;
}

/*��block��Ӧ���ļ��ж�ȡblock��header��Ϣ*/
static int __desc_read(WT_SESSION_IMPL* session, WT_BLOCK* block)
{
	WT_BLOCK_DESC *desc;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	uint32_t cksum;

	/*����һ��block�����С�Ļ�����*/
	WT_RET(__wt_scr_alloc(session, block->allocsize, &buf));
	/*���ļ���ʼ����ȡһ�������С������*/
	WT_RET(__wt_read(session, block->fh, (wt_off_t)0, (size_t)block->allocsize, buf->mem));

	desc = (WT_BLOCK_DESC *)(buf->mem);

	WT_ERR(__wt_verbose(session, WT_VERB_BLOCK,
		"%s: magic %" PRIu32
		", major/minor: %" PRIu32 "/%" PRIu32
		", checksum %#" PRIx32,
		block->name, desc->magic,
		desc->majorv, desc->minorv,
		desc->cksum));

	cksum = desc->cksum;
	desc->cksum = 0;
	/*У��ħ���ֺ�checksum*/
	if (desc->magic != WT_BLOCK_MAGIC || cksum != __wt_cksum(desc, block->allocsize))
		WT_ERR_MSG(session, WT_ERROR, "%s does not appear to be a WiredTiger file", block->name);

	/*У��block�汾��Ϣ,�Ͱ汾wiredtiger���治�ܴ���߰汾�����ϵ�block*/
	if (desc->majorv > WT_BLOCK_MAJOR_VERSION || (desc->majorv == WT_BLOCK_MAJOR_VERSION && desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_ERR_MSG(session, WT_ERROR,
		"unsupported WiredTiger file version: this build only "
		"supports major/minor versions up to %d/%d, and the file "
		"is version %d/%d",
		WT_BLOCK_MAJOR_VERSION, WT_BLOCK_MINOR_VERSION,
		desc->majorv, desc->minorv);

err:
	__wt_scr_free(session, &buf);
	return ret;
}

/*��block��ͷ��Ϣ���õ�stats����,�Ա�ͳ����ʾ*/
void __wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_DSRC_STATS *stats)
{
	/*
	 * We're looking inside the live system's structure, which normally
	 * requires locking: the chances of a corrupted read are probably
	 * non-existent, and it's statistics information regardless, but it
	 * isn't like this is a common function for an application to call.
	 */
	__wt_spin_lock(session, &block->live_lock);

	WT_STAT_SET(stats, allocation_size, block->allocsize);
	WT_STAT_SET(stats, block_checkpoint_size, block->live.ckpt_size);
	WT_STAT_SET(stats, block_magic, WT_BLOCK_MAGIC);
	WT_STAT_SET(stats, block_major, WT_BLOCK_MAJOR_VERSION);
	WT_STAT_SET(stats, block_minor, WT_BLOCK_MINOR_VERSION);
	WT_STAT_SET(stats, block_reuse_bytes, block->live.avail.bytes);
	WT_STAT_SET(stats, block_size, block->fh->size);

	__wt_spin_unlock(session, &block->live_lock);
}

