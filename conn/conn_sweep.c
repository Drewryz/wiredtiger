
#include "wt_internal.h"

/*��connect list�еĴ��ڹر�״̬��dhandleɾ��*/
static int __sweep_remove_handles(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *dhandle_next;
	WT_DECL_RET;

	conn = S2C(session);
	dhandle = SLIST_FIRST(&conn->dhlh);

	for (; dhandle != NULL; dhandle = dhandle_next){
		dhandle_next = SLIST_NEXT(dhandle, l);
		/*Ԫ���ݵ�dhandle����ɾ��*/
		if (WT_IS_METADATA(dhandle))
			continue;
		/*��״̬��dhandle����ɾ��*/
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;
		/* Make sure we get exclusive access. */
		if ((ret = __wt_try_writelock(session, dhandle->rwlock)) == EBUSY)
			continue;
		WT_RET(ret);

		/*���ڱ����õ�dhandle����ɾ�����п��������ĵط�����������*/
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) || dhandle->session_inuse != 0 || dhandle->session_ref != 0){
			WT_RET(__wt_writeunlock(session, dhandle->rwlock));
			continue;
		}

		/*����dhandle,����connection list��ɾ��*/
		WT_WITH_DHANDLE(session, dhandle, ret = __wt_conn_dhandle_discard_single(session, 0));
		/* If the handle was not successfully discarded, unlock it. */
		if (ret != 0)
			WT_TRET(__wt_writeunlock(session, dhandle->rwlock));
		WT_RET_BUSY_OK(ret);
		WT_STAT_FAST_CONN_INCR(session, dh_conn_ref);
	}

	return (ret == EBUSY ? 0 : ret);
}

/*��session��Ӧ��connection�йرղ���ʹ�õ�dhandle*/
static int __sweep(WT_SESSION_IMPL* session)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	time_t now;
	int closed_handles;

	conn = S2C(session);
	closed_handles = 0;

	WT_RET(__wt_seconds(session, &now));

	WT_STAT_FAST_CONN_INCR(session, dh_conn_sweeps);
	LIST_FOREACH(dhandle, &conn->dhlh, l){
		/*���ر�Ԫ���ݵ�dhandle*/
		if (WT_IS_METADATA(dhandle))
			continue;
		
		/*dhandle�Ѿ��ر���*/
		if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) && dhandle->session_inuse == 0 && dhandle->session_ref == 0) {
			++closed_handles;
			continue;
		}

		/*�ӳٹرյ�ʱ�仹δ����dhandle�����ر�*/
		if (dhandle->session_inuse != 0 || now <= dhandle->timeofdeath + conn->sweep_idle_time)
			continue;

		if (dhandle->timeofdeath == 0) {
			dhandle->timeofdeath = now;
			WT_STAT_FAST_CONN_INCR(session, dh_conn_tod);
			continue;
		}
		/*
		* We have a candidate for closing; if it's open, acquire an
		* exclusive lock on the handle and close it.
		*
		* The close would require I/O if an update cannot be written
		* (updates in a no-longer-referenced file might not yet be
		* globally visible if sessions have disjoint sets of files
		* open).  In that case, skip it: we'll retry the close the
		* next time, after the transaction state has progressed.
		*
		* We don't set WT_DHANDLE_EXCLUSIVE deliberately, we want
		* opens to block on us rather than returning an EBUSY error to
		* the application.
		*/
		if ((ret = __wt_try_writelock(session, dhandle->rwlock)) == EBUSY)
			continue;
		WT_RET(ret);

		/* Only sweep clean trees where all updates are visible.*/
		btree = dhandle->handle;
		if (btree->modified || !__wt_txn_visible_all(session, btree->rec_max_txn))
			goto unlock;

		/*���Թر�dhanle*/
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)){
			WT_WITH_DHANDLE(session, dhandle, ret = __wt_conn_btree_sync_and_close(session, 0, 0));
			/* We closed the btree handle, bump the statistic. */
			if (ret == 0)
				WT_STAT_FAST_CONN_INCR(session, dh_conn_handles);
		}
		/*���ùرռ�����*/
		if (dhandle->session_inuse == 0 && dhandle->session_ref == 0)
			++closed_handles;
unlock:
		WT_TRET(__wt_writeunlock(session, dhandle->rwlock));
		WT_RET_BUSY_OK(ret);
	}
	/*��connection list����ɾ���رյ�dhandle*/
	if (closed_handles > 0) {
		WT_WITH_DHANDLE_LOCK(session, ret = __sweep_remove_handles(session));
		WT_RET(ret);
	}

	return 0;
}

/*handle sweep�߳�����*/
static WT_THREAD_RET __sweep_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN) && F_ISSET(conn, WT_CONN_SERVER_SWEEP)){
		WT_ERR(__wt_cond_wait(session, conn->sweep_cond, (uint64_t)conn->sweep_interval * WT_MILLION));

		WT_ERR(__sweep(session));
	}

	if (0){
err:
		WT_PANIC_MSG(session, ret, "handle sweep server error");
	}

	return WT_THREAD_RET_VALUE;
}

/*��ȡcfg����sweep��������*/
int __wt_sweep_config(WT_SESSION_IMPL* session, const char* cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*��ȡclosed handle���ӳ��ͷ�ʱ��*/
	WT_RET(__wt_config_gets(session, cfg, "file_manager.close_idle_time", &cval));
	conn->sweep_idle_time = (time_t)cval.val;
	/*��ȡ�߳���������ʱ��*/
	WT_RET(__wt_config_gets(session, cfg, "file_manager.close_scan_interval", &cval));
	conn->sweep_interval = (time_t)cval.val;

	return 0;
}

/*����sweep thread*/
int __wt_sweep_create(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_SERVER_SWEEP);

	WT_RET(__wt_open_internal_session(conn, "sweep-server", 1, 1, &conn->sweep_session));
	session = conn->sweep_session;

	/*
	* Handle sweep does enough I/O it may be called upon to perform slow
	* operations for the block manager.
	*/
	F_SET(session, WT_SESSION_CAN_WAIT);

	WT_RET(__wt_cond_alloc(session, "handle sweep server", 0, &conn->sweep_cond));

	WT_RET(__wt_thread_create(session, &conn->sweep_tid, __sweep_server, session));
	conn->sweep_tid_set = 1;

	return (0);
}

/*ֹͣsweep thread*/
int __wt_sweep_destroy(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	F_CLR(conn, WT_CONN_SERVER_SWEEP);
	if (conn->sweep_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->sweep_cond));
		WT_TRET(__wt_thread_join(session, conn->sweep_tid));
		conn->sweep_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->sweep_cond));

	if (conn->sweep_session != NULL) {
		wt_session = &conn->sweep_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		conn->sweep_session = NULL;
	}

	return ret;
}



