#include "wt_internal.h"

/*��op�����ŶӶ����л�ȡһ��op����ִ��*/
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

static int __async_flush_wait(WT_SESSION_IMPL* session, WT_ASYNC* async, uint64_t my_gen)
{
	WT_DECL_RET;

	while(async->flush_state == WT_ASYNC_FLUSHING && async->flush_gen == my_gen)
		WT_ERR(__wt_cond_wait(session, async->flush_cond, 10000));
err:
	return ret;
}


