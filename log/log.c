/***************************************************************************
*redo��־����ʵ��
***************************************************************************/

#include "wt_internal.h"

static int __log_decompress(WT_SESSION_IMPL *, WT_ITEM *, WT_ITEM **);
static int __log_read_internal(WT_SESSION_IMPL *, WT_ITEM *, WT_LSN *, uint32_t);
static int __log_write_internal(WT_SESSION_IMPL *, WT_ITEM *, WT_LSN *, uint32_t);

/*����ѹ������ʼλ�ã�������logrec��ͷ*/
#define WT_LOG_COMPRESS_SKIP	(offsetof(WT_LOG_RECORD, record))

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

	skip = WT_LOG_COMPRESS_SKIP;
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

/*releaseһ��log��Ӧ��slot�� ����������ȻὫslot buffer�е�����д�뵽��Ӧ�ļ���page cache��
 *Ȼ����ļ�����sync������������־����*/
static int __log_release(WT_SESSION_IMPL* session, WT_LOGSLOT* slot, int* freep)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LSN sync_lsn;
	size_t write_size;
	int locked, yield_count;
	WT_DECL_SPINLOCK_ID(id);	

	conn = S2C(session);
	log = conn->log;
	locked = 0;
	yield_count = 0;
	*freep = 1;

	/*��slot�Ļ������е�log record����д�뵽��Ӧ�ļ���*/
	if(F_ISSET(slot, SLOT_BUFFERED)){
		write_size = (size_t)(slot->slot_end_lsn.offset - slot->slot_start_offset);
		WT_ERR(__wt_write(session, slot->slot_fh, slot->slot_start_offset, write_size, slot->slot_buf.mem));
	}

	/*slot��һ��dummy slot������buffer�������ݵ�slot,��������������slot��slot pool��*/
	if(F_ISSET(slot, SLOT_BUFFERED) && !F_ISSET(slot, SLOT_SYNC | SLOT_SYNC_DIR)){
		*freep = 0;
		slot->slot_state = WT_LOG_SLOT_WRITTEN;

		WT_ERR(__wt_cond_signal(session, conn->log_wrlsn_cond));

		goto done;
	}

	/*�޸�ͳ����Ϣ*/
	WT_STAT_FAST_CONN_INCR(session, log_release_write_lsn);
	/*�ж�write lsn�Ƿ�ﵽrelease lsn��λ�ã�����ﵽ������write_lsn�ĸ���*/
	while (LOG_CMP(&log->write_lsn, &slot->slot_release_lsn) != 0) {
		if (++yield_count < 1000)
			__wt_yield();
		else
			WT_ERR(__wt_cond_wait(session, log->log_write_cond, 200));
	}

	log->write_lsn = slot->slot_end_lsn;
	/*log write lsn���˸��£��õ�log_write_cond���߳����½���write lsn�ж�*/
	WT_ERR(__wt_cond_signal(session, log->log_write_cond));

	/*���slot���ڹر��ļ��ı�ʾ��֪ͨ��Ӧ�ȴ��߳̽����ļ��ر�*/
	if (F_ISSET(slot, SLOT_CLOSEFH))
		WT_ERR(__wt_cond_signal(session, conn->log_close_cond));

	while (F_ISSET(slot, SLOT_SYNC | SLOT_SYNC_DIR)){
		/*�������sync��fileС��slot->slot_end_lsn.file����ʾslot��Ӧ����־�ļ���û�����sync����(����ˢend_lsn��Ӧ���ļ�)��������еȴ�*/
		if (log->sync_lsn.file < slot->slot_end_lsn.file || __wt_spin_trylock(session, &log->log_sync_lock, &id) != 0) {
				WT_ERR(__wt_cond_wait(session, log->log_sync_cond, 10000));
				continue;
		}
		/*�����λ�ã����̻߳����log_sync_lock,���Խ���sync����*/
		locked = 1;

		sync_lsn = slot->slot_end_lsn;

		/*��ˢ��log dir path�����ļ�*/
		if (F_ISSET(slot, SLOT_SYNC_DIR) &&(log->sync_dir_lsn.file < sync_lsn.file)) {
			WT_ASSERT(session, log->log_dir_fh != NULL);
			WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_release: sync directory %s", log->log_dir_fh->name));
			WT_ERR(__wt_directory_sync_fh(session, log->log_dir_fh));
			log->sync_dir_lsn = sync_lsn;
			WT_STAT_FAST_CONN_INCR(session, log_sync_dir);
		}

		/*��ˢ����־�ļ�*/
		if (F_ISSET(slot, SLOT_SYNC) && LOG_CMP(&log->sync_lsn, &slot->slot_end_lsn) < 0) {
			WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_release: sync log %s", log->log_fh->name));
			WT_STAT_FAST_CONN_INCR(session, log_sync);
			WT_ERR(__wt_fsync(session, log->log_fh));
			/*����sync_lsn��֪ͨ�����߳�log->sync_lsn�����˸ı䣬���½��бȶ��ж�*/
			log->sync_lsn = sync_lsn;
			WT_ERR(__wt_cond_signal(session, log->log_sync_cond));
		}

		/*�����slot��SYNC��ʶ*/
		F_CLR(slot, SLOT_SYNC | SLOT_SYNC_DIR);
		/*�ͷ�sync spin lock*/
		locked = 0;
		__wt_spin_unlock(session, &log->log_sync_lock);
		/*��ֹ�����߳����°�SLOT_SYNC���û���*/
		break;
	}
err:
	if(locked)
		__wt_spin_unlock(session, &log->log_sync_lock);

	/*���err,����slot��errorֵ*/
	if (ret != 0 && slot->slot_error == 0)
		slot->slot_error = ret;

done:
	return ret;
}

/*Ϊ��־session����һ���µ���־�ļ���������־�ļ�ͷ��Ϣд�뵽��־�ļ���*/
int __wt_log_newfile(WT_SESSION_IMPL *session, int conn_create, int *created)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LSN end_lsn;
	int create_log;

	conn = S2C(session);
	log = conn->log;
	create_log = 1;
	
	/*�ȴ�__log_close_server�߳����庯����log_close_fh�Ĺر���ɣ���Ϊ���½��µ���־�ļ�������ʹ�õ�
	 *��־�ļ��������ڱ�д������һ��Ҫ�ȴ���д���*/
	while(log->log_close_fh != NULL){
		WT_STAT_FAST_CONN_INCR(session, log_close_yields);
		__wt_yield();
	}
	log->log_close_fh = log->log_fh;
	log->fileid++;

	ret = 0;
	/*Ԥ�ȷ���һ����־�ļ�����������Ŀ�Ŀ����Ǽӿ��ļ��Ľ���*/
	if(conn->log_prealloc){
		ret = __log_alloc_prealloc(session, log->fileid);
		/*������ص���ret = 0, ��ʾlog->fileid��Ӧ���ļ��Ѿ����������̴߳�����*/
		if (ret == 0)
			create_log = 0;

		/*��������*/
		if (ret != 0 && ret != WT_NOTFOUND)
			return ret;
	}

	/*û��Ԥ�����ļ����������´�����������־�ļ�ͷ��Ϣд�뵽�½�������־�ļ���*/
	if (create_log && (ret = __wt_log_allocfile(session, log->fileid, WT_LOG_FILENAME, 0)) != 0)
		return ret;

	/*���´�������־�ļ�*/
	WT_RET(__log_openfile(session, 0, &log->log_fh, WT_LOG_FILENAME, log->fileid));

	/*��ǰ��־alloc_lsn��λ��������*/
	log->alloc_lsn.file = log->fileid;
	log->alloc_lsn.offset = LOG_FIRST_RECORD;
	end_lsn = log->alloc_lsn;

	if (conn_create) {
		/*���½���־��������*/
		WT_RET(__wt_fsync(session, log->log_fh));
		log->sync_lsn = end_lsn;
		log->write_lsn = end_lsn;
	}

	if (created != NULL)
		*created = create_log;

	return 0;
}

/*������־��ȡ��һ����redo log����ʱʹ��*/
int __wt_log_read(WT_SESSION_IMPL *session, WT_ITEM* record, WT_LSN* lsnp, uint32_t flags)
{
	WT_DECL_ITEM(uncitem);
	WT_DECL_RET;
	WT_LOG_RECORD *logrec;
	WT_ITEM swap;

	WT_ERR(__log_read_internal(session, record, lsnp, flags));
	logrec = (WT_LOG_RECORD *)record->mem;

	/*�����־��¼�Ǿ�����ѹ���ģ����н�ѹ��,ѹ����־���˸о���Ӱ����־д��Ч�ʣ���wiredtigerռ�õ�CPU����ʱ�����Կ��ǽ���־ѹ��ȡ��*/
	if (F_ISSET(logrec, WT_LOG_RECORD_COMPRESSED)) {
		WT_ERR(__log_decompress(session, record, &uncitem));

		swap = *record;
		*record = *uncitem;
		*uncitem = swap;
	}
err:
	__wt_scr_free(session, &uncitem);
	return ret;
}

/*����־�ļ��ж�ȡlsnpָ��ƫ�Ƴ���һ����־log record*/
static int __log_read_internal(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	uint32_t cksum, rdup_len, reclen;

	WT_UNUSED(flags);

	if (lsnp == NULL || record == NULL)
		return 0;

	conn = S2C(session);
	log = conn->log;

	/*lsnp->offsetһ����log->allocsize��ʽ�����*/
	if (lsnp->offset % log->allocsize != 0 || lsnp->file > log->fileid)
		return WT_NOTFOUND;

	/*����־�ļ�*/
	WT_RET(__log_openfile(session, 0, &log_fh, WT_LOG_FILENAME, lsnp->file));

	/*��ȡlog rec,logrec��С��λ��1��log->allocsize*/
	WT_ERR(__wt_buf_init(session, record, log->allocsize));
	WT_ERR(__wt_read(session, log_fh, lsnp->offset, (size_t)log->allocsize, record->mem));

	/*���log rec�ĳ���*/
	reclen = *(uint32_t *)record->mem;
	if (reclen == 0) {
		ret = WT_NOTFOUND;
		goto err;
	}

	/*��ȡlogrecʣ�ಿ��*/
	if (reclen > log->allocsize) {
		rdup_len = __wt_rduppo2(reclen, log->allocsize);
		WT_ERR(__wt_buf_grow(session, record, rdup_len));
		WT_ERR(__wt_read(session, log_fh, lsnp->offset, (size_t)rdup_len, record->mem));
	}

	logrec = (WT_LOG_RECORD *)record->mem;
	cksum = logrec->checksum;
	logrec->checksum = 0;
	/*����check sum���*/
	logrec->checksum = __wt_cksum(logrec, logrec->len);
	if (logrec->checksum != cksum)
		WT_ERR_MSG(session, WT_ERROR, "log_read: Bad checksum");

	record->size = logrec->len;
	WT_STAT_FAST_CONN_INCR(session, log_reads);

err:
	WT_TRET(__wt_close(session, &log_fh));
	return ret;
}

/*����session��Ӧ��־��ȡ����ȡ��ɺ�ͨ��func����������־����*/
int __wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *lsnp, uint32_t flags, int (*func)(WT_SESSION_IMPL *session, 
				WT_ITEM *record, WT_LSN *lsnp, WT_LSN *next_lsnp, void *cookie, int firstrecord), void *cookie)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(uncitem);
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_ITEM buf;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN end_lsn, next_lsn, rd_lsn, start_lsn;
	wt_off_t log_size;
	uint32_t allocsize, cksum, firstlog, lastlog, lognum, rdup_len, reclen;
	u_int i, logcount;
	int eol;
	int firstrecord;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	log_fh = NULL;
	logcount = 0;
	logfiles = NULL;
	firstrecord = 1;
	eol = 0;
	WT_CLEAR(buf);

	/*������־����ָ�벻��ΪNULL*/
	if (func == NULL)
		return 0;

	if (LF_ISSET(WT_LOGSCAN_RECOVER)){
		WT_RET(__wt_verbose(session, WT_VERB_LOG, "__wt_log_scan truncating to %u/%" PRIuMAX, log->trunc_lsn.file, (uintmax_t)log->trunc_lsn.offset));
	}

	if(log != NULL){
		/*�����־��¼���볤��*/
		allocsize = log->allocsize;
		if(lsnp == NULL){
			if (LF_ISSET(WT_LOGSCAN_FIRST)) /*�ӵ���־��ʼλ�ÿ�ʼ����*/
				start_lsn = log->first_lsn;
			else if (LF_ISSET(WT_LOGSCAN_FROM_CKP)) /*��checkpoint����������*/
				start_lsn = log->ckpt_lsn;
			else
				return (WT_ERROR);	/* Illegal usage */
		}
		else{
			/*���ָ��һ�����ݵ�λ��(lsnp != NULL),�򲻿����Ǵ���ʼλ�û���checkpoint������,�Ȿ�����ǳ�ͻ��*/
			if (LF_ISSET(WT_LOGSCAN_FIRST|WT_LOGSCAN_FROM_CKP))
				WT_RET_MSG(session, WT_ERROR, "choose either a start LSN or a start flag");

			/*������־λ�ò��Ϸ�*/
			if(lsnp->offset % allocsize != 0 || lsnp->file > log->fileid)
				return WT_NOTFOUND;

			start_lsn = *lsnp;
			if(WT_IS_INIT_LSN(&start_lsn))
				start_lsn = log->first_lsn;
		}
		/*ȷ����־����λ��*/
		end_lsn = log->alloc_lsn;
	}
	else{
		allocsize = LOG_ALIGN;
		lastlog = 0;
		firstlog = UINT32_MAX;

		/*�����־�ļ����б�*/
		WT_RET(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
		if(logcount == 0) /*û���κ���־�ļ�����������*/
			return ENOTSUP;

		/*����־�ļ��л�ȡ���ݵ���ʼλ�ã�start lsn���ͽ���λ��(end lsn)*/
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
			lastlog = WT_MAX(lastlog, lognum);
			firstlog = WT_MIN(firstlog, lognum);
		}

		start_lsn.file = firstlog;
		end_lsn.file = lastlog;
		start_lsn.offset = end_lsn.offset = 0;

		__wt_log_files_free(session, logfiles, logcount);
		logfiles = NULL;
	}

	/*��ʼ������־���ӿ�ʼ���ļ������������ļ�*/
	WT_ERR(__log_openfile(session, 0, &log_fh, WT_LOG_FILENAME, start_lsn.file));
	WT_ERR(__log_filesize(session, log_fh, &log_size));
	for(;;){
		/*�Ѿ��������һ����¼�ˣ��л�����һ���ļ�*/
		if(rd_lsn.offset + allocsize > log_size){ 
advance:
			WT_ERR(__wt_close(session, &log_fh));
			log_fh = NULL;
			eol = 1;

			/*�������־����ģʽ��ֱ��ɾ������Ч�����ݣ�ʹ֮��allocsize����*/
			if (LF_ISSET(WT_LOGSCAN_RECOVER))
				WT_ERR(__log_truncate(session, &rd_lsn, WT_LOG_FILENAME, 1));

			rd_lsn.file++;
			rd_lsn.offset = 0;

			/*�Ѿ������һ���ļ��ˣ��������*/
			if(rd_lsn.file > end_lsn.file)
				break;
			/*����һ���ļ�*/
			WT_ERR(__log_openfile(session, 0, &log_fh, WT_LOG_FILENAME, rd_lsn.file));
			WT_ERR(__log_filesize(session, log_fh, &log_size));
			eol = 0;

			continue;
		}

		/*�ȶ�ȡһ�����볤�ȣ���Ϊһ��logrec����Ϊһ��allocsize���ȶ���*/
		WT_ASSERT(session, buf.memsize >= allocsize);
		WT_ERR(__wt_read(session, log_fh, rd_lsn.offset, (size_t)allocsize, buf.mem));
		/*ȷ��logrec�ĳ���*/
		reclen = *(uint32_t *)buf.mem;
		if(reclen == 0){
			eol = 1;
			break;
		}

		/*��reclen����allocsize�ĳ���*/
		rdup_len = __wt_rduppo2(reclen, allocsize);
		/*����logrec�ĳ��ȣ���ȡʣδ�����ĳ���*/
		if(rdup_len > allocsize){ 
			if (rd_lsn.offset + rdup_len > log_size) /*�����ļ����ȣ����ݴ洢����һ���ļ���*/
				goto advance;

			WT_ERR(__wt_buf_grow(session, &buf, rdup_len));
			WT_ERR(__wt_read(session, log_fh, rd_lsn.offset, (size_t)rdup_len, buf.mem));
			WT_STAT_FAST_CONN_INCR(session, log_scan_rereads);
		}

		/*����checksum���*/
		buf.size = reclen;
		logrec = (WT_LOG_RECORD *)buf.mem;
		cksum = logrec->checksum;
		logrec->checksum = 0;
		logrec->checksum = __wt_cksum(logrec, logrec->len);
		if(cksum != logrec->checksum){
			/*���checksum�쳣��˵������������־���ǲ����õģ����Ǳ�����ֹ��������ݣ�����log�ļ��Ӵ�lsnλ�ýص����������*/
			if (log != NULL)
				log->trunc_lsn = rd_lsn;

			if (LF_ISSET(WT_LOGSCAN_ONE))
				ret = WT_NOTFOUND;

			break;
		}

		WT_STAT_FAST_CONN_INCR(session, log_scan_records);
		/*ȷ����һ��logrec��λ��*/
		next_lsn = rd_lsn;
		next_lsn.offset += (wt_off_t)rdup_len;
		if (rd_lsn.offset != 0){
			/*�����־��ѹ��������logrec body��ѹ��*/
			if (F_ISSET(logrec, WT_LOG_RECORD_COMPRESSED)) {
				WT_ERR(__log_decompress(session, &buf, &uncitem));
				/*������־���ݴ���*/
				WT_ERR((*func)(session, uncitem, &rd_lsn, &next_lsn, cookie, firstrecord)); __wt_scr_free(session, &uncitem);
			}
			else{
				/*������־���ݴ���*/
				WT_ERR((*func)(session, &buf, &rd_lsn, &next_lsn, cookie, firstrecord));
			}

			firstrecord = 0;
			if(LF_ISSET(WT_LOGSCAN_ONE))
				break;
		}
		rd_lsn = next_lsn;
	}

	/* �չ�������wt���������е���־���ݣ���Ҫ��ȡ����Ч����־�ļ�*/
	if (LF_ISSET(WT_LOGSCAN_RECOVER) && LOG_CMP(&rd_lsn, &log->trunc_lsn) < 0)
		WT_ERR(__log_truncate(session, &rd_lsn, WT_LOG_FILENAME, 0));

err:
	WT_STAT_FAST_CONN_INCR(session, log_scans);

	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);

	__wt_buf_free(session, &buf);
	__wt_scr_free(session, &uncitem);

	if (LF_ISSET(WT_LOGSCAN_ONE) && eol && ret == 0)
		ret = WT_NOTFOUND;

	/*û����־�ļ����ݣ�˵������Ҫ���ݣ�Ҳ����ɹ���*/
	if (ret == ENOENT)
		ret = 0;

	WT_TRET(__wt_close(session, &log_fh));

	return ret;
}

/*��ͨ���ϲ�buffer��д��־���������кܶ��СIO����*/
static int __log_direct_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
{
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT tmp;
	WT_MYSLOT myslot;
	int dummy, locked;
	WT_DECL_SPINLOCK_ID(id);

	log = S2C(session)->log;
	myslot.slot = &tmp;
	myslot.offset = 0;
	dummy = 0;
	WT_CLEAR(tmp);

	/*���slot lock*/
	if (__wt_spin_trylock(session, &log->log_slot_lock, &id) != 0)
		return EAGAIN;

	locked = 1;

	/*��һ����ʱ��slot�����й���������Ϊͬ��sync����*/
	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
		F_SET(&tmp, SLOT_SYNC_DIR);

	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(&tmp, SLOT_SYNC);

	/*���ļ���slot start lsn�ĸ���*/
	WT_ERR(__log_acquire(session, record->size, &tmp));
	
	__wt_spin_unlock(session, &log->log_slot_lock);
	locked = 0;

	/*��������д����*/
	WT_ERR(__log_fill(session, &myslot, 1, record, lsnp));
	WT_ERR(__log_release(session, &tmp, &dummy));

err:
	if(locked)
		__wt_spin_unlock(session, &log->log_slot_lock);

	return ret;
}

/*��һ��logrecд����־�ļ���,��������������������־ѹ�������logrec body��ѹ��*/
int __wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
{
	WT_COMPRESSOR *compressor;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(citem);
	WT_DECL_RET;
	WT_ITEM *ip;
	WT_LOG *log;
	WT_LOG_RECORD *complrp;
	int compression_failed;
	size_t len, src_len, dst_len, result_len, size;
	uint8_t *src, *dst;

	conn = S2C(session);
	log = conn->log;

	/*log����д���ļ����ΪNULL������д*/
	if(log->log_fh == NULL)
		return 0;

	ip = record;
	if ((compressor = conn->log_compressor) != NULL && record->size < log->allocsize)
		WT_STAT_FAST_CONN_INCR(session, log_compress_small);
	else if(compressor != NULL){
		src = (uint8_t *)record->mem + WT_LOG_COMPRESS_SKIP;
		src_len = record->size - WT_LOG_COMPRESS_SKIP;

		/*������־����ѹ�������ݳ��ȵ�ȷ��*/
		if (compressor->pre_size == NULL)
			len = src_len;
		else
			WT_ERR(compressor->pre_size(compressor, &session->iface, src, src_len, &len));

		size = len + WT_LOG_COMPRESS_SKIP;
		WT_ERR(__wt_scr_alloc(session, size, &citem));

		/* Skip the header bytes of the destination data. */
		dst = (uint8_t *)citem->mem + WT_LOG_COMPRESS_SKIP;
		dst_len = len;
		/*��������ѹ��*/
		compression_failed = 0;
		WT_ERR(compressor->compress(compressor, &session->iface, src, src_len, dst, dst_len, &result_len, &compression_failed));

		result_len += WT_LOG_COMPRESS_SKIP;

		/*ѹ��ʧ�ܻ���ѹ��������ݱ�ѹ��ǰ�����ݻ��󣬲�����ѹ������*/
		if (compression_failed || result_len / log->allocsize >= record->size / log->allocsize)
			WT_STAT_FAST_CONN_INCR(session, log_compress_write_fails);
		else {
			/*ͳ����Ϣ����*/
			WT_STAT_FAST_CONN_INCR(session, log_compress_writes);
			WT_STAT_FAST_CONN_INCRV(session, log_compress_mem, record->size);
			WT_STAT_FAST_CONN_INCRV(session, log_compress_len, result_len);

			/*��ѹ���������滻��δѹ�������ݽ�����־д*/
			memcpy(citem->mem, record->mem, WT_LOG_COMPRESS_SKIP);
			citem->size = result_len;
			ip = citem;
			complrp = (WT_LOG_RECORD *)citem->mem;
			F_SET(complrp, WT_LOG_RECORD_COMPRESSED);
			WT_ASSERT(session, result_len < UINT32_MAX &&
			    record->size < UINT32_MAX);
			complrp->len = WT_STORE_SIZE(result_len);
			complrp->mem_len = WT_STORE_SIZE(record->size);
		}
	}
	/*��־д��*/
	ret = __log_write_internal(session, ip, lsnp, flags);

err:
	__wt_scr_free(session, &citem);
	return ret;
}

static int __log_write_internal(WT_SESSION_IMPL* session, WT_ITEM* record, WT_LSN* lsnp, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN lsn;
	WT_MYSLOT myslot;
	uint32_t rdup_len;
	int free_slot, locked;

	conn = S2C(session);
	log = conn->log;
	free_slot = locked = 0;
	WT_INIT_LSN(&lsn);
	myslot.slot = NULL;

	WT_STAT_FAST_CONN_INCRV(session, log_bytes_payload, record->size);

	/*ȷ��logrec����ĳ���,��������볤�ȵ�buf*/
	rdup_len = __wt_rduppo2((uint32_t)record->size, log->allocsize);

	WT_ERR(__wt_buf_grow(session, record, rdup_len));
	WT_ASSERT(session, record->data == record->mem);

	if (record->size != rdup_len) {
		memset((uint8_t *)record->mem + record->size, 0, rdup_len - record->size);
		record->size = rdup_len;
	}

	/*����checksum*/
	logrec = (WT_LOG_RECORD *)record->mem;
	logrec->len = (uint32_t)record->size;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, record->size);

	WT_STAT_FAST_CONN_INCR(session, log_writes);
	/*ǿ��ˢ��ģʽ,����СIO�ϲ�,����innodb commit_trx = 1��ģʽ*/
	if(!F_ISSET(log, WT_LOG_FORCE_CONSOLIDATE)){
		ret = __log_direct_write(session, record, lsnp, flags);
		if (ret == 0)
			return 0;

		if (ret != EAGAIN)
			WT_ERR(ret);
	}

	F_SET(log, WT_LOG_FORCE_CONSOLIDATE);
	/*��ȡһ��slot�����log����д��λ��,�п��ܻ�spin wait*/
	if ((ret = __wt_log_slot_join(session, rdup_len, flags, &myslot)) == ENOMEM){
		/*�����ʱ���޷�JION SLOT,����ֱ��д��ʽ*/
		while((ret = __log_direct_write(session, record, lsnp, flags)) == EAGAIN)
			;

		WT_ERR(ret);
		/*�޷�join slot������slot buffer�Ƚ�С��������������Ϊ����һ�θ�����join*/
		WT_ERR(__wt_log_slot_grow_buffers(session, 4 * rdup_len));
		return 0;
	}

	WT_ERR(ret);

	/*��β������Ʒǳ����ӣ�ע�⣡������*/
	if (myslot.offset == 0) {
		__wt_spin_lock(session, &log->log_slot_lock);
		locked = 1;
		WT_ERR(__wt_log_slot_close(session, myslot.slot));
		WT_ERR(__log_acquire(session, myslot.slot->slot_group_size, myslot.slot));

		__wt_spin_unlock(session, &log->log_slot_lock);
		locked = 0;

		WT_ERR(__wt_log_slot_notify(session, myslot.slot));
	} 
	else
		WT_ERR(__wt_log_slot_wait(session, myslot.slot));

	WT_ERR(__log_fill(session, &myslot, 0, record, &lsn));

	if (__wt_log_slot_release(myslot.slot, rdup_len) == WT_LOG_SLOT_DONE) {
		WT_ERR(__log_release(session, myslot.slot, &free_slot));
		if (free_slot)
			WT_ERR(__wt_log_slot_free(session, myslot.slot));
	} 
	else if (LF_ISSET(WT_LOG_FSYNC)) {
		/* Wait for our writes to reach disk */
		while (LOG_CMP(&log->sync_lsn, &lsn) <= 0 && myslot.slot->slot_error == 0)
			(void)__wt_cond_wait(session, log->log_sync_cond, 10000);
	} 
	else if (LF_ISSET(WT_LOG_FLUSH)) {
		/* Wait for our writes to reach the OS */
		while (LOG_CMP(&log->write_lsn, &lsn) <= 0 && myslot.slot->slot_error == 0)
			(void)__wt_cond_wait(session, log->log_write_cond, 10000);
	}
err:
	if (locked)
		__wt_spin_unlock(session, &log->log_slot_lock);

	if (ret == 0 && lsnp != NULL)
		*lsnp = lsn;

	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC) && ret == 0 && myslot.slot != NULL)
		ret = myslot.slot->slot_error;

	return ret;
}

/*logrec�ĸ�ʽ������,��д�뵽log�ļ���*/
int __wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(logrec);
	WT_DECL_RET;
	va_list ap_copy;
	const char *rec_fmt = WT_UNCHECKED_STRING(I);
	uint32_t rectype = WT_LOGREC_MESSAGE;
	size_t header_size, len;

	conn = S2C(session);

	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return (0);

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + 1;
	va_end(ap_copy);

	WT_RET(__wt_logrec_alloc(session, sizeof(WT_LOG_RECORD) + len, &logrec));

	/*
	 * We're writing a record with the type (an integer) followed by a
	 * string (NUL-terminated data).  To avoid writing the string into
	 * a buffer before copying it, we write the header first, then the
	 * raw bytes of the string.
	 */
	WT_ERR(__wt_struct_size(session, &header_size, rec_fmt, rectype));
	WT_ERR(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, header_size, rec_fmt, rectype));
	logrec->size += (uint32_t)header_size;

	(void)vsnprintf((char *)logrec->data + logrec->size, len, fmt, ap);

	WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_printf: %s", (char *)logrec->data + logrec->size));

	logrec->size += len;
	WT_ERR(__wt_log_write(session, logrec, NULL, 0));

err:	
	__wt_scr_free(session, &logrec);
	return ret;
}





