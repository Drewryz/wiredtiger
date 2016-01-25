#include "wt_internal.h"

/*�ȴ�op �ŶӶ��о��������Խ��ж�������*/
static int __async_op_dequeue(WT_CONNECTION_IMPL* conn, WT_SESSION_IMPL* session, WT_ASYNC_OP_IMPL** op)
{
	WT_ASYNC *async;
	uint64_t cur_tail, last_consume, my_consume, my_slot, prev_slot;
	uint64_t sleep_usec;
	uint32_t tries;

	async = conn->async;
	*op = NULL;

retry:
	tries = 0;
	sleep_usec = 100;
	WT_ORDERED_READ(last_consume, async->alloc_tail);

	/*����spin wait, ���async�Ƿ���Խ��빤��״̬*/
	while(last_consume == async->head && async->flush_state != WT_ASYNC_FLUSHING){
		WT_STAT_FAST_CONN_INCR(session, async_nowork);
		if(++tries < MAX_ASYNC_YIELD)
			__wt_yield();
		else{
			__wt_sleep(0, sleep_usec);
			sleep_usec = WT_MIN(sleep_usec * 2, MAX_ASYNC_SLEEP_USECS);
		}

		if(!F_ISSET(session, WT_SESSION_SERVER_ASYNC))
			return 0;
		if(!F_ISSET(conn, WT_CONN_SERVER_ASYNC))
			return 0;

		WT_RET(WT_SESSION_CHECK_PANIC(session));
		/*����last_consume��������һ��ѭ���ж�*/
		WT_ORDERED_READ(last_consume, async->alloc_tail);
	}
	/*�ڽ��빤��״̬ǰ���ٴ��ж�,��ֹ������*/
	if (async->flush_state == WT_ASYNC_FLUSHING)
		return 0;

	my_consume = last_consume + 1;
	if (!WT_ATOMIC_CAS8(async->alloc_tail, last_consume, my_consume))
		goto retry;

	/*ȷ��ִ�е�op slot�Ͷ���*/
	my_slot = my_consume % async->async_qsize;
	prev_slot = last_consume % async->async_qsize;
	*op = (WT_ASYNC_OP_IMPL*)WT_ATOMIC_STORE8(async->async_queue[my_slot], NULL);

	WT_ASSERT(session, async->cur_queue > 0);
	WT_ASSERT(session, *op != NULL);
	WT_ASSERT(session, (*op)->state == WT_ASYNCOP_ENQUEUED);
	(void)WT_ATOMIC_SUB4(async->cur_queue, 1);
	(*op)->state = WT_ASYNCOP_WORKING;

	if(*op == &async->flush_op)
		WT_PUBLISH(async->flush_state, WT_ASYNC_FLUSHING);

	WT_ORDERED_READ(cur_tail, async->tail_slot);
	while (cur_tail != prev_slot) {
		__wt_yield();
		WT_ORDERED_READ(cur_tail, async->tail_slot);
	}
	WT_PUBLISH(async->tail_slot, my_slot);

	return 0;
}

/*worker�̵߳ȴ�*/
static int __async_flush_wait(WT_SESSION_IMPL* session, WT_ASYNC* async, uint64_t my_gen)
{
	WT_DECL_RET;

	while(async->flush_state == WT_ASYNC_FLUSHING && async->flush_gen == my_gen)
		WT_ERR(__wt_cond_wait(session, async->flush_cond, 10000));
err:
	return ret;
}

static int __async_worker_cursor(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
	WT_ASYNC_WORKER_STATE *worker, WT_CURSOR **cursorp)
{
	WT_ASYNC_CURSOR *ac;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	wt_session = (WT_SESSION *)session;
	*cursorp = NULL;

	/*compact����Ҫcursor*/
	if(op->optype == WT_AOP_COMPACT)
		return 0;

	WT_ASSERT(session, op->format != NULL);
	STAILQ_FOREACH(ac, &worker->cursorqh, q){
		if (op->format->cfg_hash == ac->cfg_hash && op->format->uri_hash == ac->uri_hash) {
			/*
			 * If one of our cached cursors has a matching
			 * signature, use it and we're done.
			 */
			*cursorp = ac->c;
			return 0;
		}
	}

	/*��cursor queue��û���ҵ�ƥ���cursor,ֱ�ӹ���һ��cursor��������cursor queue��*/
	WT_RET(__wt_calloc_one(session, &ac));
	WT_ERR(wt_session->open_cursor(wt_session, op->format->uri, NULL, op->format->config, &c));
	ac->cfg_hash = op->format->cfg_hash;
	ac->uri_hash = op->format->uri_hash;
	ac->c = c;
	STAILQ_INSERT_HEAD(&worker->cursorqh, ac, q);
	worker->num_cursors++;
	*cursorp = c;

	return 0;

err:
	__wt_free(session, ac);
}

/*async workerִ��һ��op����*/
static int __async_worker_execop(WT_SESSION_IMPL* session, WT_ASYNC_OP_IMPL* op, WT_CURSOR* cursor)
{
	WT_ASYNC_OP* asyncop;
	WT_ITEM val;
	WT_SESSION* wt_session;

	asyncop = (WT_ASYNC_OP*)op;

	/*ȷ��������key/value*/
	if(op->optype !=  WT_AOP_COMPACT){
		WT_RET(__wt_cursor_get_raw_key(&asyncop->c, &val));
		__wt_cursor_set_raw_key(cursor, &val);

		if (op->optype == WT_AOP_INSERT || op->optype == WT_AOP_UPDATE) {
			WT_RET(__wt_cursor_get_raw_value(&asyncop->c, &val));
			__wt_cursor_set_raw_value(cursor, &val);
		}
	}

	/*opִ��*/
	switch(op->optype){
		case WT_AOP_COMPACT:
			wt_session = &session->iface;
			WT_RET(wt_session->compact(wt_session, op->format->uri, op->format->config));
			break;
		case WT_AOP_INSERT:
			WT_RET(cursor->insert(cursor));
			break;
		case WT_AOP_UPDATE:
			WT_RET(cursor->update(cursor));
			break;
		case WT_AOP_REMOVE:
			WT_RET(cursor->remove(cursor));
			break;
		case WT_AOP_SEARCH:
			WT_RET(cursor->search(cursor));
			WT_RET(__wt_cursor_get_raw_value(cursor, &val));
			__wt_cursor_set_raw_value(&asyncop->c, &val);
			break;
		case WT_AOP_NONE:
		default:
			WT_RET_MSG(session, EINVAL, "Unknown async optype %d\n", op->optype);
	}

	return 0;
}
/*ִ��async op����*/
static int __async_worker_op(WT_SESSION_IMPL* session, WT_ASYNC_OP_IMPL* op, WT_ASYNC_WORKER_STATE* worker)
{
	WT_ASYNC_OP *asyncop;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	int cb_ret;

	asyncop = (WT_ASYNC_OP *)op;

	cb_ret = 0;

	/*�������ɾ��ģ�����һ������*/
	wt_session = &session->iface;
	if(op->optype != WT_AOP_COMPACT)
		WT_RET(wt_session->begin_transaction(wt_session, NULL));

	/*ȷ��ִ�в�����cursor*/
	WT_ASSERT(session, op->state == WT_ASYNCOP_WORKING);
	WT_RET(__async_worker_cursor(session, op, worker, &cursor));

	ret = __async_worker_execop(session, op, cursor);
	if (op->cb != NULL && op->cb->notify != NULL)
		cb_ret = op->cb->notify(op->cb, asyncop, ret, 0);

	/*����ִ�гɹ������ύ������ִ��ʧ����ع�*/
	if (op->optype != WT_AOP_COMPACT) {
		if ((ret == 0 || ret == WT_NOTFOUND) && cb_ret == 0)
			WT_TRET(wt_session->commit_transaction(wt_session, NULL));
		else
			WT_TRET(wt_session->rollback_transaction(wt_session, NULL));
		F_CLR(&asyncop->c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		WT_TRET(cursor->reset(cursor));
	}
	
	/*�ͷ�op slot,�Ա�async op����ؽ����ظ������������*/
	WT_PUBLISH(op->state, WT_ASYNCOP_FREE);

	return ret;
}

/*async workers�߳�ִ���庯��*/
WT_THREAD_RET __wt_async_worker(void* arg)
{
	WT_ASYNC *async;
	WT_ASYNC_CURSOR *ac, *acnext;
	WT_ASYNC_OP_IMPL *op;
	WT_ASYNC_WORKER_STATE worker;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	uint64_t flush_gen;

	session = arg;
	conn = S2C(session);
	async = conn->async;

	worker.num_cursors = 0;
	STAILQ_INIT(&worker.cursorqh);

	while(F_ISSET(conn, WT_CONN_SERVER_ASYNC) && F_ISSET(session, WT_SESSION_SERVER_ASYNC)){
		/*����ŶӶ����Ƿ��������״̬*/
		WT_ERR(__async_op_dequeue(conn, session, &op));
		if(op != NULL && op != &async->flush_op){
			(void)__async_worker_op(session, op, &worker);
			WT_ERR(WT_SESSION_CHECK_PANIC(session));
		}
		else if(async->flush_state == WT_ASYNC_FLUSHING){
			WT_ORDERED_READ(flush_gen, async->flush_gen);
			if(WT_ATOMIC_ADD4(async->flush_count, 1) == conn->async_workers){ /*worker�߳�ȫ�������ˣ�������Щ�߳��ڵȴ���֪ͨ�����߳̽����ȴ�������ִ��*/
				WT_PUBLISH(async->flush_state, WT_ASYNC_FLUSH_COMPLETE);
				WT_ERR(__wt_cond_signal(session, async->flush_cond));
			}
			else
				WT_ERR(__async_flush_wait(session, async, flush_gen));
		}
	}

	if(0){
err: 
		WT_PANIC_MSG(session, ret, "async worker error");
	}

	ac = STAILQ_FIRST(&worker.cursorqh);
	while (ac != NULL) {
		acnext = STAILQ_NEXT(ac, q);
		WT_TRET(ac->c->close(ac->c));
		__wt_free(session, ac);
		ac = acnext;
	}

	return (WT_THREAD_RET_VALUE);
}


