/***************************************************************************
*redo��־����ʵ��
***************************************************************************/

#include "wt_internal.h"

static int __log_decompress(WT_SESSION_IMPL *, WT_ITEM *, WT_ITEM **);
static int __log_read_internal(WT_SESSION_IMPL *, WT_ITEM *, WT_LSN *, uint32_t);
static int __log_write_internal(WT_SESSION_IMPL *, WT_ITEM *, WT_LSN *, uint32_t);

/*����ѹ������ʼλ�ã�������logrec��ͷ*/
#define WT_LOG__COMPRESS_SKIP	(offsetof(WT_LOG_RECORD, record))

/*����log����һ��checkpoint����archive thread����һ��checkpoint�ź�*/
int __wt_log_ckpt(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	log->ckpt_lsn = *ckp_lsn;

	if(conn->log_cond != NULL)
		WT_RET(__wt_cond_signal(session, conn->log_cond));

	return 0;
}

/*���redo log�Ƿ���Ҫ�������ݣ��������0��ʾ�������������ݣ�1��ʾ��Ҫ����log����*/
int __wt_log_needs_recovery(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn, int *rec)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	*rec = 1;
	if(log == NULL)
		return 0;

	/*��ʼ��cursor log,��Ҫ�ǰ�װ*/
	WT_RET(__wt_curlog_open(session, "log:", NULL, &c));

	/*��file + offset��ΪKEY�ڽ��в���*/
	c->set_key(c, ckp_lsn->file, ckp_lsn->offset, 0);
	ret = c->search(c);
	if(ret == 0){
		ret = c->next(c);
		if(ret == WT_NOTFOUND){
			*rec = 0;
			ret = 0;
		}
	}
	else if(ret == WT_NOTFOUND){
		ret = 0;
	}
	else{
		WT_ERR(ret);
	}

err: 
	WT_TRET(c->close(c));

	return ret;
}

/*��checkpoint����������logΪ����д״̬,����log_written��ֵ��Ϊ0��
 *��Ϊlog�е����ݶ�ͬ����page�Ĵ�������*/
void __wt_log_written_reset(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL* conn;
	WT_LOG* log;

	conn = S2C(session);
	if(!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return ;

	log = con->log;
	log->log_written = 0;

	return 0;
}

/*ͨ��һ��ǰ׺ƥ������ҳ�������ص���־�ļ���*/
static int __log_get_files(WT_SESSION_IMPL* session, const char* file_prefix, char*** filesp, u_int* countp)
{
	WT_CONNECTION_IMPL *conn;
	const char *log_path;

	*countp = 0;
	*filesp = NULL;

	conn = S2C(session);
	log_path = conn->log_path;
	if (log_path == NULL)
		log_path = "";

	return __wt_dirlist(session, log_path, file_prefix, WT_DIRLIST_INCLUDE, filesp, countp);
}

/*������е���־�ļ������߲��ҽ���active����־�ļ���(����check lsn����־�ļ�)*/
int __wt_log_get_all_files(WT_SESSION_IMPL *session, char ***filesp, u_int *countp, uint32_t *maxid, int active_only)
{
	WT_DECL_RET;
	WT_LOG *log;
	char **files;
	uint32_t id, max;
	u_int count, i;

	id = 0;
	log = S2C(session)->log;

	*maxid = 0;
	/*�˴�������ڴ棬����쳣����Ҫ�ͷ�*/
	WT_RET(__log_get_files(session, WT_LOG_FILENAME, &filesp, &count));

	/*���˵�����С��checkpoint LSN���ļ�*/
	for(max = 0, i = 0; i < count;){
		WT_ERR(__wt_log_extract_lognum(session, files[i], &id));

		if(active_only && id < log->ckpt_lsn.file){
			__wt_free(session, files[i]);
			files[--count] = NULL;
		}
		else{
			if(id > max)
				max = id;
			i++;
		}
	}

	*maxid = max;
	*filesp = files;
	*countp = count;

	return ret;

err:
	__wt_log_files_free(session, files, count);
	return ret;
}

/*�ͷ�log�ļ����б�*/
void __wt_log_files_free(WT_SESSION_IMPL *session, char **files, u_int count)
{
	u_int i;
	for(i = 0; i < count; i++)
		__wt_free(session, files[i]);

	__wt_free(session, files);
}

/*��log�ļ����н���һ��log number(LSN�е�file id)*/
int __wt_log_extract_lognum(WT_SESSION_IMPL *session, const char *name, uint32_t *id)
{
	const char *p;

	WT_UNUSED(session);
	
	if(id == NULL || name == NULL)
		return WT_ERROR;

	if ((p = strrchr(name, '.')) == NULL || sscanf(++p, "%" SCNu32, id) != 1)
		WT_RET_MSG(session, WT_ERROR, "Bad log file name '%s'", name);

	return 0;
}

static int __log_filename(WT_SESSION_IMPL *session, uint32_t id, const char *file_prefix, WT_ITEM *buf)
{
	const char *log_path;

	log_path = S2C(session)->log_path;

	if (log_path != NULL && log_path[0] != '\0')
		WT_RET(__wt_buf_fmt(session, buf, "%s/%s.%010" PRIu32, log_path, file_prefix, id));
	else
		WT_RET(__wt_buf_fmt(session, buf, "%s.%010" PRIu32, file_prefix, id));

	return (0);
}

/*��fh��Ӧ��log�ļ����пռ��趨��size = log_file_max����һ�����½�log�ļ�ʱ����*/
static int __log_prealloc(WT_SESSION_IMPL* session, WT_FH* fh)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	ret = 0;

	if(fh->fallocate_available == WT_FALLOCATE_AVAILABLE ||(ret = __wt_fallocate(session, fh, LOG_FIRST_RECORD, conn->log_file_max)) == ENOTSUP){
		ret = __wt_ftruncate(session, fh, LOG_FIRST_RECORD + conn->log_file_max);
	}

	return ret;
}

/*�ж�lsnָ���log�ļ���ʣ��ռ��Ƿ��ܴ洢��һ���µ�logrec����*/
static int __log_size_fit(WT_SESSION_IMPL* session, WT_LSN* lsn, uint64_t recsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	return (lsn->offset + (wt_off_t)recsize < conn->log_file_max);
}

static int __log_acquire(WT_SESSION_IMPL* session, uint64_t recsize, WT_LOGSLOT* slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int created_log;

	conn = S2C(session);
	log = conn->log;
	created_log = 1;

	slot->slot_release_lsn = log->alloc_lsn;

	/*�����ǰalloc_lsn��Ӧ���ļ�ʣ��ռ��޷�����recsize��С�����ݣ�
	 *��Ҫ�½�һ���µ�log file�����棬���п��ܻ��漰���ݹ����
	 *__wt_log_newfile->__wt_log_allocfile->__log_file_header->__log_acquire*/
	if(!__log_size_fit(session, &log->alloc_lsn, recsize)){
		WT_RET(__wt_log_newfile(session, 0, &created_log));
		if(log->log_close_fh != NULL)
			F_SET(slot, SLOT_CLOSEFH); /*����Ӧ��slot��Ϊ���ر�״̬*/
	}

	/*checkpoint��ʱ������log->written�����ģ��������������¶�Ӧ��ͳ��,������checkpoint����*/
	if(WT_CKPT_LOGSIZE(conn)){
		log->log_written += (wt_off_t)recsize;
		WT_RET(__wt_checkpoint_signal(session, log->log_written));
	}

	slot->slot_start_lsn = log->alloc_lsn;
	slot->slot_start_offset = log->alloc_lsn.offset;

	/*���alloc_lsn��Ӧ����LOG�ĵ�һ����¼�����Ҵ������µ��ļ�,��ʾ��ǰalloc_lsn�޷�����recsize�����ݣ���Ҫ�����ļ��ռ�����*/
	if(log->alloc_lsn.offset == LOG_FIRST_RECORD && created_log)
		WT_RET(__log_prealloc(session, log->log_fh));

	/*�޸�log��״̬��slot��״̬*/
	log->alloc_lsn.offset += (wt_off_t)recsize;
	slot->slot_end_lsn = log->alloc_lsn;
	slot->slot_error = 0;
	slot->slot_fh = log->log_fh;

	return 0;
}

/*��logrec�����ݽ�ѹ��,�������������ڴ�*/
static int __log_decompress(WT_SESSION_IMPL session*, WT_ITEM* in, WT_ITEM** out)
{
	WT_COMPRESSOR *compressor;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG_RECORD *logrec;
	size_t result_len, skip;
	uint32_t uncompressed_size;

	conn = S2C(session);
	logrec = (WT_LOG_RECORD *)in->mem;

	skip = WT_LOG__COMPRESS_SKIP;
	compressor = conn->log_compressor;
	if(compressor == NULL || compressor->decompress == NULL){
		WT_ERR_MSG(session, WT_ERROR, "log_read: Compressed record with no configured compressor");
	}
	
	/*����out�ڴ����*/
	uncompressed_size = logrec->mem_len;
	WT_ERR(__wt_scr_alloc(session, 0, out));
	WT_ERR(__wt_buf_initsize(session, *out, uncompressed_size));
	memcpy((*out)->mem, in->mem, skip);

	/*���н�ѹ��*/
	WT_ERR(compressor->decompress(compressor, &session->iface,
		(uint8_t *)in->mem + skip, in->size - skip,
		(uint8_t *)(*out)->mem + skip,
		uncompressed_size - skip, &result_len));

	if(ret != 0 || result_len != uncompressed_size - WT_LOG__COMPRESS_SKIP)
		WT_ERR(WT_ERROR);

err:
	return ret;
}

/*log(record����)����д��*/
static int __log_fill(WT_SESSION_IMPL* session, WT_MYSLOT* myslot, int direct, WT_ITEM* record, WT_LSN* lsnp)
{
	WT_DECL_RET;
	WT_LOG_RECORD *logrec;

	logrec = (WT_LOG_RECORD *)record->mem;
	if(direct){
		/*����־��ֱ�����̲�������Ҫ�ȴ�IO���*/
		WT_ERR(__wt_write(session, myslot->slot->slot_fh,
			myslot->offset + myslot->slot->slot_start_offset,
			(size_t)logrec->len, (void *)logrec));
	}
	else /*д��slot��������,Ч�ʸ��ã���������*/
		memcpy((char *)myslot->slot->slot_buf.mem + myslot->offset, logrec, logrec->len);

	WT_STAT_FAST_CONN_INCRV(session, log_bytes_written, logrec->len);
	if(lsnp != NULL){
		*lsnp = myslot->slot->slot_start_lsn;
		lsnp->offset += (wt_off_t)myslot->offset;
	}

err:
	if(ret != 0 && myslot->slot->slot_error == 0) /*slot���󱣴�*/
		myslot->slot->slot_error = ret;

	return ret;
}

/*������־ͷ��logrec header����д����־�ļ�(WT_FH)��*/
static int __log_file_header(WT_SESSION_IMPL* session, WT_FH* fh, WT_LSN* end_lsn, int prealloc)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_DESC *desc;
	WT_LOG_RECORD *logrec;
	WT_LOGSLOT tmp;
	WT_MYSLOT myslot;

	conn = S2C(session);
	log = conn->log;

	/*����һ��buf�����BUF���ڹ���logrec*/
	WT_ASSERT(session, sizeof(WT_LOG_DESC) < log->allocsize);
	WT_RET(__wt_scr_alloc(session, log->allocsize, &buf));
	memset(buf->mem, 0, log->allocsize);

	/*����ͷ��Ϣ����Ҫ��ħ��У���֡�WT�汾��Ϣ��log->size,*/
	logrec = (WT_LOG_RECORD *)buf->mem;
	desc = (WT_LOG_DESC *)logrec->record;
	desc->log_magic = WT_LOG_MAGIC;
	desc->majorv = WT_LOG_MAJOR_VERSION;
	desc->minorv = WT_LOG_MINOR_VERSION;
	desc->log_size = (uint64_t)conn->log_file_max;

	/*����logrec��checksum*/
	logrec->len = log->allocsize;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, log->allocsize);

	WT_CLEAR(tmp);
	myslot.slot = &tmp;
	myslot.offset = 0;

	/*slot��fh�Ѿ����˿ռ�����*/
	if(prealloc){
		WT_ASSERT(session, fh != NULL);
		tmp.slot_fh = fh;
	}
	else{ /*�������realloc���̵ģ���ô��ʹ��log��ǰ��Ӧ��slot*/
		WT_ASSERT(session, fh == NULL);
		log->prep_missed++;
		WT_ERR(__log_acquire(session, logrec->len, &tmp));
	}

	/*ֱ��д��log�ļ�����*/
	WT_ERR(__log_fill(session, &myslot, 1, buf, NULL));
	/*��־����*/
	WT_ERR(__wt_fsync(session, tmp.slot_fh));
	if(end_lsn != NULL)
		*end_lsn = tmp.slot_end_lsn;
err:
	__wt_scr_free(session, &buf);
	return ret;
}

/*��һ����־�ļ�*/
static int __log_openfile(WT_SESSION_IMPL* session, int ok_create, WT_FH** fh, const char* file_prefix, uint32_t id)
{
	WT_DECL_ITEM(path);
	WT_DECL_RET;

	/*����һ��path������*/
	WT_RET(__wt_scr_alloc(session, 0, &path));
	/*����һ��log�ļ�����������path��*/
	WT_ERR(__log_filename(session, id, file_prefix, path));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG,"opening log %s", (const char *)path->data));
	/*�������򿪶�Ӧ��log�ļ�*/
	WT_ERR(__wt_open(session, path->data, ok_create, 0, WT_FILE_TYPE_LOG, fh));

err:
	__wt_scr_free(session, &path);
	return ret;
}

/*��ȡһ��Ԥ����log�ļ�������to_num����һ����ʽ��log�ļ����������Ԥ�����ļ�*/
static int __log_alloc_prealloc(WT_SESSION_IMPL* session, uint32_t to_num)
{
	WT_DECL_ITEM(from_path);
	WT_DECL_ITEM(to_path);
	WT_DECL_RET;
	uint32_t	from_num;
	u_int		logcount;
	char**		logfiles;


	logfiles = NULL;

	/*��ȡһ��������log�ļ��б�*/
	WT_ERR(__log_get_files(session, WT_LOG_PREPNAME, &logfiles, &logcount));
	if(logcount == 0)
		return WT_NOTFOUND;

	/*��õ�һ��Ԥ�����ļ���lsn->file���*/
	WT_ERR(__wt_log_extract_lognum(session, logfiles[0], &from_num));

	WT_ERR(__wt_scr_alloc(session, 0, &from_path));
	WT_ERR(__wt_scr_alloc(session, 0, &to_path));

	/*����һ��Ԥ�����ļ�·��*/
	WT_ERR(__log_filename(session, from_num, WT_LOG_PREPNAME, from_path));
	/*����һ������Ԥ����LOG�ļ�����ʽlog�ļ�·��*/
	WT_ERR(__log_filename(session, to_num, WT_LOG_FILENAME, to_path));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_alloc_prealloc: rename log %s to %s", (char *)from_path->data, (char *)to_path->data));

	WT_STAT_FAST_CONN_INCR(session, log_prealloc_used);
	/*�ļ���ʽ������Ч*/
	WT_ERR(__wt_rename(session, (const char*)(from_path->data), to_path->data));

err:
	__wt_scr_free(session, &from_path);
	__wt_scr_free(session, &to_path);
	if (logfiles != NULL) /*�ͷ�__log_get_files���ļ����б�*/
		__wt_log_files_free(session, logfiles, logcount);

	return ret;
}

/*��log�ļ�����truncate������ʹ����ʵ����Ч��log����ƥ��*/
static int __log_truncate(WT_SESSION_IMPL* session, WT_LSN* lsn, const char* file_prefix, uint32_t this_log)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *log_fh, *tmp_fh;
	WT_LOG *log;
	uint32_t lognum;
	u_int i, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	log_fh = NULL;
	logcount = 0;
	logfiles = NULL;

	/*���ȵ���LSN��Ӧ��־�ļ��ĳ��ȵ�������Ч���ݳ���λ����*/
	WT_ERR(__log_openfile(session, 0, &log_fh, file_prefix, lsn->file));
	WT_ERR(__wt_ftruncate(session, log_fh, lsn->offset));
	tmp_fh = log_fh;
	log_fh = NULL;
	WT_ERR(__wt_fsync(session, tmp_fh));
	WT_ERR(__wt_close(session, &tmp_fh));

	if(this_log)
		goto err;

	/*�����ʽlog�ļ����б�*/
	WT_ERR(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));

	/*������С��log trunc lsn���ļ�ȫ�����*/
	for(i = 0; i < logcount; i++){
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		if(lognum > lsn->file && lognum < log->trunc_lsn.file){
			WT_ERR(__log_openfile(session, 0, &log_fh, file_prefix, lognum));
			/*ֻ����log file��ͷ��Ϣ*/
			WT_ERR(__wt_ftruncate(session, log_fh, LOG_FIRST_RECORD));

			tmp_fh = log_fh;
			log_fh = NULL;
			WT_ERR(__wt_fsync(session, tmp_fh));
			WT_ERR(__wt_close(session, &tmp_fh));
		}
	}

err:
	WT_TRET(__wt_close(session, &log_fh));
	if(logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);

	return ret;
}

/*����һ��ָ�����͵���־�ļ����ڷ���Ĺ����У��Ȼ�ͨ����ʱ��־�ļ�����־ͷд�뵽�ļ��У������и���,
 *������ʱ�ļ�Ӧ���Ƿ�ֹ���̲߳����ĳ�ͻ*/
int __wt_log_allocfile(WT_SESSION_IMPL *session, uint32_t lognum, const char *dest, int prealloc)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(from_path);
	WT_DECL_ITEM(to_path);
	WT_DECL_RET;
	WT_FH *log_fh, *tmp_fh;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	log_fh = NULL;

	WT_RET(__wt_scr_alloc(session, 0, &from_path));
	WT_ERR(__wt_scr_alloc(session, 0, &to_path));
	WT_ERR(__log_filename(session, lognum, WT_LOG_TMPNAME, from_path));
	WT_ERR(__log_filename(session, lognum, dest, to_path));

	/*�����ʱ�ļ�������һ����ʼ��log headerд�뵽��ʱ�ļ���*/
	WT_ERR(__log_openfile(session, 1, &log_fh, WT_LOG_TMPNAME, lognum));
	WT_ERR(__log_file_header(session, log_fh, NULL, 1));
	WT_ERR(__wt_ftruncate(session, log_fh, LOG_FIRST_RECORD));
	if (prealloc)
		WT_ERR(__log_prealloc(session, log_fh));

	tmp_fh = log_fh;
	log_fh = NULL;

	WT_ERR(__wt_fsync(session, tmp_fh));
	WT_ERR(__wt_close(session, &tmp_fh));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_prealloc: rename %s to %s", (char *)from_path->data, (char *)to_path->data));

	WT_ERR(__wt_rename(session, from_path->data, to_path->data));

err:
	__wt_scr_free(session, &from_path);
	__wt_scr_free(session, &to_path);
	WT_TRET(__wt_close(session, &log_fh));

	return ret;
}

/*����log number�Ƴ�һ����Ӧ���ļ�*/
int __wt_log_remove(WT_SESSION_IMPL *session, const char *file_prefix, uint32_t lognum)
{
	WT_DECL_ITEM(path);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__log_filename(session, lognum, file_prefix, path));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_remove: remove log %s", (char *)path->data));
	WT_ERR(__wt_remove(session, path->data));

err:
	__wt_scr_free(session, &path);
	return ret;
}

/*Ϊsession��һ����־�ļ��� Ŀ�����ҳ��Ѿ�������־�ļ������LSN,
 *��������Ϊ����־�ļ����ļ�������������ھ���־�ļ���
 * ��__wt_log_newfileΪ�䴴��һ���µ���־*/
int __wt_log_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t firstlog, lastlog, lognum;
	u_int i, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	logfiles = NULL;
	logcount = 0;
	lastlog = 0;
	firstlog = UINT32_MAX;

	/*��session connection��Ӧ����־�ļ�Ŀ¼�����ļ�*/
	if (log->log_dir_fh == NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG, "log_open: open fh to directory %s", conn->log_path));
		WT_RET(__wt_open(session, conn->log_path, 0, 0, WT_FILE_TYPE_DIRECTORY, &log->log_dir_fh));
	}

	/*��session��Ӧ����־�ļ�Ŀ¼�½������Ѿ�������ʱ��־�ļ�������logfiles��*/
	WT_ERR(__log_get_files(session, WT_LOG_TMPNAME, &logfiles, &logcount));
	/*ɾ�����е���ʱ��־�ļ�,��ΪҪ��һ������LSN��Ӧ���ļ����ͱ��뽫���ڹر���������ʹ�õ�LSN��־�ļ�����ô����Ӧ��
	 *��ʱ�ļ��ͻᱻ�ر�*/
	for(i = 0; i < logcount; i++){
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		WT_ERR(__wt_log_remove(session, WT_LOG_TMPNAME, lognum));
	}

	__wt_log_files_free(session, logfiles, logcount);

	logfiles = NULL;
	logcount = 0;

	/*�������Ԥ������־�ļ����ļ���,��ͨ��ɾ������Ԥ�����ļ�*/
	WT_ERR(__log_get_files(session, WT_LOG_PREPNAME, &logfiles, &logcount));

	for (i = 0; i < logcount; i++) {
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		WT_ERR(__wt_log_remove(session, WT_LOG_PREPNAME, lognum));
	}
	__wt_log_files_free(session, logfiles, logcount);
	logfiles = NULL;

	/*��ȡ��ʽ����־�ļ�������ͨ���ļ���ȷ��lastlog��firstlog*/
	WT_ERR(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
	for (i = 0; i < logcount; i++) {
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		lastlog = WT_MAX(lastlog, lognum);
		firstlog = WT_MIN(firstlog, lognum);
	}

	/*��lastlog��Ϊfileid*/
	log->fileid = lastlog;
	WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_open: first log %d last log %d", firstlog, lastlog));
	/*û����С��lsn����������Ϊ��1��0)*/
	if (firstlog == UINT32_MAX) {
		WT_ASSERT(session, logcount == 0);
		WT_INIT_LSN(&log->first_lsn);
	} 
	else {
		log->first_lsn.file = firstlog;
		log->first_lsn.offset = 0;
	}
	/*��__wt_log_newfile����һ���µ���־�ļ���������־ͷ��Ϣд���ļ���*/
	WT_ERR(__wt_log_newfile(session, 1, NULL));
	/*��������ǰ����־�ļ�������log��״̬*/
	if (logcount > 0) {
		log->trunc_lsn = log->alloc_lsn;
		FLD_SET(conn->log_flags, WT_CONN_LOG_EXISTED);
	}

err:
	if(logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);

	return ret;
}

/*�ر�session��Ӧ����־�ļ�*/
int __wt_log_close(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	/*�ر���־�ļ�*/
	if (log->log_close_fh != NULL && log->log_close_fh != log->log_fh) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG, "closing old log %s", log->log_close_fh->name));
		WT_RET(__wt_fsync(session, log->log_close_fh));
		WT_RET(__wt_close(session, &log->log_close_fh));
	}

	if (log->log_fh != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG, "closing log %s", log->log_fh->name));
		WT_RET(__wt_fsync(session, log->log_fh));
		WT_RET(__wt_close(session, &log->log_fh));
		log->log_fh = NULL;
	}

	/*fsync��־Ŀ¼�����ļ�*/
	if (log->log_dir_fh != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG, "closing log directory %s", log->log_dir_fh->name));
		WT_RET(__wt_directory_sync_fh(session, log->log_dir_fh));
		WT_RET(__wt_close(session, &log->log_dir_fh));
		log->log_dir_fh = NULL;
	}

	return 0;
}

/*ȷ��һ����־�ļ���Ч���ݵĿռ��С*/
static int __log_filesize(WT_SESSION_IMPL* session, WT_FH* fh, wt_off_t* eof)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	wt_off_t log_size, off, off1;
	uint32_t allocsize, bufsz;
	char *buf, *zerobuf;

	conn = S2C(session);
	log = conn->log;
	if(eof == NULL)
		return 0;

	*eof = 0;
	WT_RET(__wt_filesize(session, fh, &log_size));
	if(log == NULL)
		allocsize = LOG_ALIGN;
	else
		allocsize = log->allocsize;

	buf = zerobuf = NULL;
	if (allocsize < WT_MEGABYTE && log_size > WT_MEGABYTE) /*�����1M��ʽ����*/
		bufsz = WT_MEGABYTE;
	else
		bufsz = allocsize;

	/*��������ƥ��Ļ�����*/
	WT_RET(__wt_calloc_def(session, bufsz, &buf));
	WT_ERR(__wt_calloc_def(session, bufsz, &zerobuf));

	/*���ļ�ĩβ��ʼ��ǰ��ÿ�ζ�ȡ1�����볤�ȵ����ݣ���zerobuf�Ƚϣ�ֱ���в�Ϊ0������Ϊֹ��
	 *��Ϊ0�����ݱ�ʾ����log��ĩβ,��־�ļ���__log_prealloc�������СΪlog->log_file_max,����Ϊ0��*/
	for (off = log_size - (wt_off_t)bufsz; off >= 0; off -= (wt_off_t)bufsz) {
		WT_ERR(__wt_read(session, fh, off, bufsz, buf));
		if (memcmp(buf, zerobuf, bufsz) != 0)
			break;
	}
	/*�Ѿ������ļ���ʼλ��*/
	if(off < 0)
		off = 0;

	/*���п���λ�õ�ȷ�ϣ����磬��128 ~ 256���buf���������ݲ�ƥ�䣬�ҵ����һ����Ϊ0��ƫ��*/
	for (off1 = bufsz - allocsize; off1 > 0; off1 -= (wt_off_t)allocsize){
		if (memcmp(buf + off1, zerobuf, sizeof(uint32_t)) != 0)
			break;
	}

	off = off + off1;
	/*��eof���õ����һ����Ϊ0������ƫ����*/
	*eof = off + (wt_off_t)allocsize;

err:
	if (buf != NULL)
		__wt_free(session, buf);

	if(zerobuf != NULL)
		__wt_free(session, zerobuf);

	return ret;
}



