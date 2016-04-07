
#include "wt_internal.h"

static int __ckpt_server_start(WT_CONNECTION_IMPL* session);

static int __ckpt_server_config(WT_SESSION_IMPL* session, const char** cfg, int *startp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL* conn;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	char* p;

	conn = S2C(session);

	/*��ȡcheckpoint�ļ��ʱ��*/
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.wait", &cval));
	conn->ckpt_usecs = (uint64_t)cval.val * 1000000;
	/*��ȡcheckpoint��log file��С��ֵ*/
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.log_size", &cval));
	conn->ckpt_logsize = (wt_off_t)cval.val;
	/*��λlog����*/
	__wt_log_written_reset(session);

	/*checkpoint��������Ϣ�����ˣ������ܲ�����checkpoint�ļ��ʱ���log file��ֵ*/
	if ((conn->ckpt_usecs == 0 && conn->ckpt_logsize == 0) ||
		(conn->ckpt_logsize && conn->ckpt_usecs == 0 && !FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))){
		*startp = 0;
		return 0;
	}

	*startp = 1;

	/*
	* The application can specify a checkpoint name, which we ignore if it's our default.
	* ��ȡcheckpoint name���ã�������Ƿ�Ϸ�
	*/
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.name", &cval));
	if (cval.len != 0 && !WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
		WT_RET(__wt_checkpoint_name_ok(session, cval.str, cval.len));

		WT_RET(__wt_scr_alloc(session, cval.len + 20, &tmp));
		WT_ERR(__wt_buf_fmt(session, tmp, "name=%.*s", (int)cval.len, cval.str));
		WT_ERR(__wt_strdup(session, tmp->data, &p));

		__wt_free(session, conn->ckpt_config);
		conn->ckpt_config = p;
	}

err:
	__wt_scr_free(session, &tmp);
	return ret;
}

/*checkpoint thread�߳�����*/
static WT_THREAD_RET __ckpt_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);
	wt_session = (WT_SESSION *)session;

	while (F_ISSET(conn, WT_CONN_SERVER_RUN) && F_ISSET(conn, WT_CONN_SERVER_CHECKPOINT)){
		/*�ȴ�����checkpoint���ʱ�����log file�����˶�Ӧ��ֵ������checkpoint*/
		WT_ERR(__wt_cond_wait(session, conn->ckpt_cond, conn->ckpt_usecs));

		/*�����ݿ⽨��һ���µ�checkpoint*/
		WT_ERR(wt_session->checkpoint(wt_session, conn->ckpt_config));

		if (conn->ckpt_logsize) {
			__wt_log_written_reset(session);
			conn->ckpt_signalled = 0;

			/*
			* In case we crossed the log limit during the
			* checkpoint and the condition variable was already
			* signalled, do a tiny wait to clear it so we don't do
			* another checkpoint immediately.
			* ��������������һ��cond_wait,��Ϊ���µ���־һֱ��append,��conn->ckpt_signalled������0�ǣ��ᴥ�����
			* __wt_checkpoint_signal����ֹ�����ʱ��̫����Ƶ����checkpoint,����������һ��1��ĵȴ�
			*/
			WT_ERR(__wt_cond_wait(session, conn->ckpt_cond, 1));
		}
	}

	if (0) {
err:		
		WT_PANIC_MSG(session, ret, "checkpoint server error");
	}
	return (WT_THREAD_RET_VALUE);
}

/*����checkpoint�߳�*/
static int __ckpt_server_start(WT_CONNECTION_IMPL* conn)
{
	WT_SESSION_IMPL *session;

	/* Nothing to do if the server is already running. */
	if (conn->ckpt_session != NULL)
		return 0;

	F_SET(conn, WT_CONN_SERVER_CHECKPOINT);
	/* The checkpoint server gets its own session. */
	WT_RET(__wt_open_internal_session(conn, "checkpoint-server", 1, 1, &conn->ckpt_session));
	session = conn->ckpt_session;

	/*
	* Checkpoint does enough I/O it may be called upon to perform slow
	* operations for the block manager.
	*/
	F_SET(session, WT_SESSION_CAN_WAIT);

	WT_RET(__wt_cond_alloc(session, "checkpoint server", 0, &conn->ckpt_cond));

	/*
	* Start the thread.
	*/
	WT_RET(__wt_thread_create(session, &conn->ckpt_tid, __ckpt_server, session));
	conn->ckpt_tid_set = 1;

	return 0;
}

/*����checkpoint service*/
int __wt_checkpoint_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	int start;

	conn = S2C(session);
	start = 0;

	/* If there is already a server running, shut it down. */
	if (conn->ckpt_session != NULL)
		WT_RET(__wt_checkpoint_server_destroy(session));

	WT_RET(__ckpt_server_config(session, cfg, &start));
	if (start)
		WT_RET(__ckpt_server_start(conn));

	return 0;
}

/*����checkpoint service*/
int __wt_checkpoint_server_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	F_CLR(conn, WT_CONN_SERVER_CHECKPOINT);
	if (conn->ckpt_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->ckpt_cond));
		WT_TRET(__wt_thread_join(session, conn->ckpt_tid));
		conn->ckpt_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->ckpt_cond));

	__wt_free(session, conn->ckpt_config);

	/* Close the server thread's session. */
	if (conn->ckpt_session != NULL) {
		wt_session = &conn->ckpt_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
	}

	/*
	* Ensure checkpoint settings are cleared - so that reconfigure doesn't
	* get confused.
	*/
	conn->ckpt_session = NULL;
	conn->ckpt_tid_set = 0;
	conn->ckpt_cond = NULL;
	conn->ckpt_config = NULL;
	conn->ckpt_usecs = 0;

	return ret;
}

/*��־�ļ��ﵽ�˽���checkpoint��ֵ��֪ͨcheckpoint service�����µ�checkpoint*/
int __wt_checkpoint_signal(WT_SESSION_IMPL *session, wt_off_t logsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	WT_ASSERT(session, WT_CKPT_LOGSIZE(conn));
	if (logsize >= conn->ckpt_logsize && !conn->ckpt_signalled) {
		WT_RET(__wt_cond_signal(session, conn->ckpt_cond));
		conn->ckpt_signalled = 1;
	}
	return 0;
}

