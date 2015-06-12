/*************************************************************************
*conn redo log��д����
*************************************************************************/

#include "wt_internal.h"

/*�������ַ����н���transaction_sync��Ӧ��������Ŀ*/
static int __logmgr_sync_cfg(WT_SESSION_IMPL* session, const char** cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*��ȡtxn sync�Ŀ�����ʶ*/
	WT_RET(__wt_config_gets(session, cfg, "transaction_sync.enabled", &cval));

	/*����������־�ı�ʶ������redo log��flush*/
	if(cval.val)
		FLD_SET(conn->txn_logsync, WT_LOG_FLUSH);
	else
		FLD_CLR(conn->txn_logsync, WT_LOG_FLUSH);

	WT_RET(__wt_config_gets(session, cfg, "transaction_sync.method", &cval));
	/*��WT_LOG_DSYNC��ʶ��λ���,�����������϶�ȡ�Ƿ�������FSYNC��DSYNC*/
	FLD_CLR(conn->txn_logsync, WT_LOG_DSYNC | WT_LOG_FSYNC);
	if (WT_STRING_MATCH("dsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_DSYNC);
	else if (WT_STRING_MATCH("fsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_FSYNC);

	return 0;
}

/*����������log��ص���־����ѡ��ֵ*/
static int __logmgr_config(WT_SESSION_IMPL* session, const char** cfg, int* runp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*��־�������أ�Ĭ���ǹرյ�*/
	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	*runp = cval.val != 0;

	/*��ȡ��־�Ƿ����ѹ����Ŀ*/
	conn->log_compressor = NULL;
	WT_RET(__wt_config_gets_none(session, cfg, "log.compressor", &cval));
	WT_RET(__wt_compressor_config(session, &cval, &conn->log_compressor));

	/*��ȡ��־�ļ���ŵ�·��*/
	WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->log_path));

	if (*runp == 0)
		return (0);

	/*��ȡlog archive��ʶ*/
	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
	if (cval.val != 0)
		FLD_SET(conn->log_flags, WT_CONN_LOG_ARCHIVE);

	/*�����־�ļ����ռ��С*/
	WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
	conn->log_file_max = (wt_off_t)cval.val;
	WT_STAT_FAST_CONN_SET(session, log_max_filesize, conn->log_file_max);

	/*�����־��Ԥ����������*/
	WT_RET(__wt_config_gets(session, cfg, "log.prealloc", &cval));
	if (cval.val != 0) {
		FLD_SET(conn->log_flags, WT_CONN_LOG_PREALLOC);
		conn->log_prealloc = 1;
	}

	/*��ȡ��־���ݵ�ѡ��*/
	WT_RET(__wt_config_gets_def(session, cfg, "log.recover", 0, &cval));
	if (cval.len != 0  && WT_STRING_MATCH("error", cval.str, cval.len))
		FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR);

	WT_RET(__logmgr_sync_cfg(session, cfg));

	return 0;
}

/*����һ����־�鵵����,�൱��ɾ���������־�ļ�*/
static int __log_archive_once(WT_SESSION_IMPL *session, uint32_t backup_file)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t lognum, min_lognum;
	u_int i, locked, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	logcount = 0;
	logfiles = NULL;

	/*һ���Ǵ�С��checkpoint�����й鵵�ģ�����Ӵ���checkpoint��ȥ�鵵����ô���п��ܻᶪʧ����*/
	if (backup_file != 0)
		min_lognum = WT_MIN(log->ckpt_lsn.file, backup_file);
	else
		min_lognum = WT_MIN(log->ckpt_lsn.file, log->sync_lsn.file);

	WT_RET(__wt_verbose(session, WT_VERB_LOG, "log_archive: archive to log number %" PRIu32, min_lognum));

	/*���logĿ¼�µ���־�ļ����б�*/
	WT_RET(__wt_dirlist(session, conn->log_path, WT_LOG_FILENAME, WT_DIRLIST_INCLUDE, &logfiles, &logcount));

	__wt_spin_lock(session, &conn->hot_backup_lock);
	locked = 1;
	if(conn->hot_backup == 0 || backup_file != 0){
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
			if (lognum < min_lognum)
				WT_ERR(__wt_log_remove(session, WT_LOG_FILENAME, lognum)); 
			/*ɾ��Ҫ�鵵����־�ļ�,����ط�ֱ��ɾ�����᲻�᲻�ף�innobase�����ǽ��ļ�
			 *���ݵ�һ��Ŀ¼����ʱ������*/
		}
	}

	/*����������������Դ�ͷ�*/
	__wt_spin_unlock(session, &conn->hot_backup_lock);
	locked = 0;
	__wt_log_files_free(session, logfiles, logcount);
	logfiles = NULL;
	logcount = 0;

	log->first_lsn.file = min_lognum;
	log->first_lsn.offset = 0;

	return ret;

	/*������*/
err:
	__wt_err(session, ret, "log archive server error");
	if (locked)
		__wt_spin_unlock(session, &conn->hot_backup_lock);

	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);

	return ret;
}

/*����һ����־�ļ�Ԥ����*/
static int __log_prealloc_once(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	u_int i, reccount;
	char **recfiles;

	conn = S2C(session);
	log = conn->log;
	reccount = 0;
	recfiles = NULL;

	/*�����Ѿ���logĿ¼�´��ڵ�Ԥ�����ļ�����*/
	WT_ERR(__wt_dirlist(session, conn->log_path, WT_LOG_PREPNAME, WT_DIRLIST_INCLUDE, &recfiles, &reccount));
	__wt_log_files_free(session, recfiles, reccount);
	recfiles = NULL;

	/*������ǰ��Ԥ�����ļ��ظ��������Ѿ����ڵ�Ԥ�����ļ�����ô���ظ����õĴ��� ��Ԥ������ļ�����ӣ���Ϊ����Ԥ������ļ���*/
	if (log->prep_missed > 0) {
		conn->log_prealloc += log->prep_missed;
		WT_ERR(__wt_verbose(session, WT_VERB_LOG, "Now pre-allocating up to %" PRIu32, conn->log_prealloc));
		log->prep_missed = 0;
	}

	WT_STAT_FAST_CONN_SET(session, log_prealloc_max, conn->log_prealloc);
	/*����Ԥ������־�ļ�*/
	for (i = reccount; i < (u_int)conn->log_prealloc; i++) {
		WT_ERR(__wt_log_allocfile(session, ++log->prep_fileid, WT_LOG_PREPNAME, 1));
		WT_STAT_FAST_CONN_INCR(session, log_prealloc_files);
	}

	return ret;

err:
	__wt_err(session, ret, "log pre-alloc server error");
	if (recfiles != NULL)
		__wt_log_files_free(session, recfiles, reccount);

	return ret;
}

int __wt_log_truncate_files(WT_SESSION_IMPL *session, WT_CURSOR *cursor, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t backup_file, locked;

	WT_UNUSED(cfg);
	conn = S2C(session);
	log = conn->log;

	if (F_ISSET(conn, WT_CONN_SERVER_RUN) && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE))
		WT_RET_MSG(session, EINVAL, "Attempt to archive manually while a server is running");

	/*ȷ���鵵�������־�ļ����*/
	backup_file = 0;
	if (cursor != NULL)
		backup_file = WT_CURSOR_BACKUP_ID(cursor);

	WT_ASSERT(session, backup_file <= log->alloc_lsn.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG, "log_truncate_files: Archive once up to %" PRIu32, backup_file));

	/*������־�鵵,�����õ���һ����д������*/
	WT_RET(__wt_writelock(session, log->log_archive_lock));
	locked = 1;
	WT_ERR(__log_archive_once(session, backup_file));
	WT_ERR(__wt_writeunlock(session, log->log_archive_lock));
	locked = 0;

err:
	if(locked)
		WT_RET(__wt_writeunlock(session, log->log_archive_lock));

	return ret;
}

/*��close_fh��Ӧ���ļ�����fsync�͹رղ���,һ��ֻ�еȵ�log_close_cond�źŴ����Ż����һ��close�������,��һ���߳��庯��*/
static WT_THREAD_RET __log_close_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *close_fh;
	WT_LOG *log;
	WT_LSN close_end_lsn, close_lsn;
	WT_SESSION_IMPL *session;
	int locked;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = 0;

	while(F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)){
		/*close fh�����ļ��ȴ�close�����������ļ�ĩβ��Ӧ��LSNλ���Ѿ�С������д��־���ļ�LSN,��������ļ���������֧��д�����Թر�*/
		close_fh = log->log_close_fh;
		if (close_fh != NULL && 
			(ret = __wt_log_extract_lognum(session, close_fh->name, &close_lsn.file)) == 0 && close_lsn.file < log->write_lsn.file) {

				log->log_close_fh = NULL;

				close_lsn.offset = 0;
				close_end_lsn = close_lsn;
				close_end_lsn.file++;

				/*����fsync����*/
				WT_ERR(__wt_fsync(session, close_fh));
				__wt_spin_lock(session, &log->log_sync_lock);

				/*�ر��ļ�*/
				locked = 1;
				WT_ERR(__wt_close(session, &close_fh));
				log->sync_lsn = close_end_lsn;
				/*����һ��log_sync_cond��ʾsync_lsn�����������µ�ֵ*/
				WT_ERR(__wt_cond_signal(session, log->log_sync_cond));
				locked = 0;

				__wt_spin_unlock(session, &log->log_sync_lock);
		}
		else{
			/*�ȴ���һ���ļ���close cond*/
			WT_ERR(__wt_cond_wait(session, conn->log_close_cond, WT_MILLION));
		}
	}

	return WT_THREAD_RET_VALUE;

err:
	__wt_err(session, ret, "log close server error");
	if (locked)
		__wt_spin_unlock(session, &log->log_sync_lock);

	return WT_THREAD_RET_VALUE;
}

typedef struct {
	WT_LSN		lsn;
	uint32_t	slot_index;
} WT_LOG_WRLSN_ENTRY;

/*WT_LOG_WRLSN_ENTRY�ıȽ�������,��ʵ���ǵ�lsn�ıȽϣ�Ϊ�˱�֤lsn˳����д��*/
static int WT_CDECL __log_wrlsn_cmp(const void *a, const void *b)
{
	WT_LOG_WRLSN_ENTRY *ae, *be;

	ae = (WT_LOG_WRLSN_ENTRY *)a;
	be = (WT_LOG_WRLSN_ENTRY *)b;
	return LOG_CMP(&ae->lsn, &be->lsn);
}

/**/
static WT_THREAD_RET __log_wrlsn_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG*				log;
	WT_LOG_WRLSN_ENTRY	written[SLOT_POOL];
	WT_LOGSLOT*			slot;
	WT_SESSION_IMPL*	session;
	size_t				written_i;
	uint32_t			i, save_i;
	int					yield;

	session = (WT_SESSION_IMPL*)arg;
	conn = S2C(session);
	log = conn->log;
	yield = 0;

	while(F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)){

		i = 0;
		written_i = 0;

		/*���ﲻ��Ҫ��slot pool���ж��̱߳�������Ϊslot pool�Ǹ���̬������*/
		while(i < SLOT_POOL){
			save_i = i;
			slot = &log->slot_pool[i++];
			if (slot->slot_state != WT_LOG_SLOT_WRITTEN) /*���˵���WRITTEN״̬*/
				continue;

			written[written_i].slot_index = save_i;
			written[written_i++].lsn = slot->slot_release_lsn;
		}

		if (written_i > 0) {
			yield = 0;
			/*��LSN��С��������,��ΪҪ��slot��������ˢ��*/
			qsort(written, written_i, sizeof(WT_LOG_WRLSN_ENTRY), __log_wrlsn_cmp);
			/*
			 * We know the written array is sorted by LSN.  Go
			 * through them either advancing write_lsn or stop
			 * as soon as one is not in order.
			 */
			for (i = 0; i < written_i; i++) {
				if (LOG_CMP(&log->write_lsn, &written[i].lsn) != 0)
					break;
				/*
				 * If we get here we have a slot to process.
				 * Advance the LSN and process the slot.
				 */
				slot = &log->slot_pool[written[i].slot_index];
				WT_ASSERT(session, LOG_CMP(&written[i].lsn, &slot->slot_release_lsn) == 0);
				
				log->write_lsn = slot->slot_end_lsn;
				WT_ERR(__wt_cond_signal(session,log->log_write_cond));

				WT_STAT_FAST_CONN_INCR(session, log_write_lsn);

				/*
				 * Signal the close thread if needed.���԰�file page cache������sync��������
				 */
				if (F_ISSET(slot, SLOT_CLOSEFH))
					WT_ERR(__wt_cond_signal(session, conn->log_close_cond));

				WT_ERR(__wt_log_slot_free(session, slot));
			}
		}

		if (yield++ < 1000)
			__wt_yield();
		else
			/* Wait until the next event. */
			WT_ERR(__wt_cond_wait(session, conn->log_wrlsn_cond, 100000));
	}

	return WT_THREAD_RET_VALUE;

err:
	__wt_err(session, ret, "log wrlsn server error");
	return WT_THREAD_RET_VALUE;
}



