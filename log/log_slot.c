/********************************************************************************
*һ������buffer slot��log bufferʵ�֣�ͨ����ͬ��slot�����log buffer�Ĳ���Ч��
*����ϸ�ڿ��Բο���
*http://infoscience.epfl.ch/record/170505/files/aether-smpfulltext.pdf
*����
********************************************************************************/

#include "wt_internal.h"

/*��ʼ��session��Ӧ��log�����log slot*/
int __wt_log_slot_init(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int32_t i;

	/*���session��Ӧ��log����*/
	conn = S2C(session);
	log = conn->log;

	/*����log slot pool��ʼ��*/
	for (i = 0; i < SLOT_POOL; i++) {
		log->slot_pool[i].slot_state = WT_LOG_SLOT_FREE;
		log->slot_pool[i].slot_index = SLOT_INVALID_INDEX;
	}

	/*Ϊlog�趨һ���Ѿ�׼���õ�slot���´�write log�����slot*/
	for(i = 0; i < SLOT_ACTIVE; i++){
		slot = &log->slot_pool[i];
		slot->slot_index = (uint32_t)i;
		slot->slot_state = WT_LOG_SLOT_READY;
		log->slot_array[i] = slot;
	}

	/*Ϊ����slot������־д�뻺����*/
	for(i = 0; i < SLOT_POOL; i++){
		WT_ERR(__wt_buf_init(session, &log->slot_pool[i].slot_buf, WT_LOG_SLOT_BUF_INIT_SIZE));
		/*��ʼ��slot��ʶλ*/
		F_SET(&log->slot_pool[i], SLOT_INIT_FLAGS);
	}

	/*��¼log buffer��ͳ����Ϣ*/
	WT_STAT_FAST_CONN_INCRV(session, log_buffer_size, WT_LOG_SLOT_BUF_INIT_SIZE * SLOT_POOL);
	
	return ret;

err:
	__wt_buf_free(session, &log->slot_pool[i].slot_buf);
	return ret;
}

/*����session��Ӧ��log slot buffer*/
int __wt_log_slot_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int i;

	conn = S2C(session);
	log = conn->log;
	/*�ͷſ��ٵ��ڴ�ռ�*/
	for (i = 0; i < SLOT_POOL; i++)
		__wt_buf_free(session, &log->slot_pool[i].slot_buf);

	return 0;
}

/*ѡȡһ��ready slot������log write������������л�ȷ��д���slot�Լ�д���λ��*/
int __wt_log_slot_join(WT_SESSION_IMPL* session, uint64_t mysize, uint32_t flags, WT_MYSLOT* myslotp)
{
	WT_CONNECTION_IMPL*	conn;
	WT_LOG*				log;
	WT_LOGSLOT*			slot;
	int64_t				cur_state, new_state, old_state;
	uint32_t			allocated_slot, slot_grow_attempts;

	conn = S2C(session);
	log = conn->log;

	slot_grow_attempts = 0;

find_slot:
	/*�����active slots�л�ȡһ���Ѿ�����׼��״̬��slot*/
	allocated_slot = __wt_random(session->rnd) % SLOT_ACTIVE;
	slot = log->slot_array[allocated_slot];
	old_state = slot->slot_state;

join_slot:
	if(old_state < WT_LOG_SLOT_READY){ /*��ʾ���slot�Ѿ����ڲ���ѡȡ״̬����������ѡ����Ϊ��find_slot�Ĺ����п����ж���߳�ͬʱfind_slot,������ѡ�����൱��spin*/
		WT_STAT_FAST_CONN_INCR(session, log_slot_transitions);
		goto find_slot;
	}

	/*
	 * Add in our size to the state and then atomically swap that
	 * into place if it is still the same value.
	 * ����൱����ռд���λ�úͳ���,���λ�ñ�����߳���ռ��
	 * ��������ȥѡȡslot
	 */
	new_state = old_state + (int64_t)mysize;
	if(new_state < old_state){
		WT_STAT_FAST_CONN_INCR(session, log_slot_toobig);
		goto find_slot;
	}

	/*���slot buffer��ʣ��Ŀռ��޷�������д���LOG����������ѡȡ*/
	if(new_state > (int64_t)slot->slot_buf.memsize){
		F_SET(slot, SLOT_BUF_GROW);
		if(++slot_grow_attempts > 5){ /*����5��*/
			WT_STAT_FAST_CONN_INCR(session, log_slot_toosmall);
			return ENOMEM;
		}

		goto find_slot;
	}

	/*����ԭ���Ը���*/
	cur_state = WT_ATOMIC_CAS_VAL8(slot->slot_state, old_state, new_state);
	if(cur_state != old_state){ /*��ʾ�Ѿ����������߳��Ѿ���ռ�����slot,����join��д��λ��*/
		old_state = cur_state;
		WT_STAT_FAST_CONN_INCR(session, log_slot_races);
		goto join_slot;
	}

	WT_ASSERT(session, myslotp != NULL);

	/*����joins��ͳ����Ϣ*/
	WT_STAT_FAST_CONN_INCR(session, log_slot_joins);

	/*ȷ��fsync�ķ�ʽ��DIRECT IO����page cache sync*/
	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
		F_SET(slot, SLOT_SYNC_DIR);

	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(slot, SLOT_SYNC);

	/*ȷ��logд��slot��λ��*/
	myslotp->slot = slot;
	myslotp->offset = (wt_off_t)old_state - WT_LOG_SLOT_READY;

	return 0;
}

/*closeһ��slot����ֹ�����߳��ٶ���join, ����������Ƚ����ready array��ɾ������ѡȡһ���µ�slot�������Ĺ���*/
int __wt_log_slot_close(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *newslot;
	int64_t old_state;
	int32_t yields;
	uint32_t pool_i, switch_fails;

	conn = S2C(session);
	log = conn->log;
	switch_fails = 0;

retry:
	/*��õ�ǰlog bufferʹ�õ�slot*/
	pool_i = log->pool_index;
	newslot = &(log->slot_pool[pool_i]);
	/*�����һ��index����slot pool�ķ�Χ����ص�pool�ĵ�һ����Ԫ��*/
	if(++log->pool_index >= SLOT_POOL){
		log->pool_index = 0;
	}

	/*��ǰ��slot���ǿ��Թرյ�״̬�����еȴ�����*/
	if(newslot->slot_state != WT_LOG_SLOT_FREE){
		WT_STAT_FAST_CONN_INCR(session, log_slot_switch_fails);

		/* slot churn�������߳�Ϊ�˵ȴ�slot��close״̬��ռ�ù����CPUʱ����Ƶģ����slot_churn > 0,���ʾ��
		 * slot��WT_LOG_SLOT_FREE�����close�߳���Ҫ�ͷ�CPU����Ȩ���ò���ϵͳ�ٽ���һ�ι�ƽ����*/
		if(++switch_fails % SLOT_POOL == 0 && slot->slot_churn < 5)
			++slot->slot_churn;

		__wt_yield();
		goto retry;
	}
	else if(slot->slot_churn > 0){
		--slot->slot_churn;
		WT_ASSERT(session, slot->slot_churn >= 0);
	}

	/*�ͷ�CPU����Ȩ���������߳��л��������ռCPUʱ��Ƭ*/
	for(yields = slot->slot_churn; yields >= 0; yields --)
		__wt_yield();

	/*����״̬����*/
	WT_STAT_FAST_CONN_INCR(session, log_slot_closes);

	/*����new slot ready��ʶ*/
	newslot->slot_state = WT_LOG_SLOT_READY;

	/*�µ�slot����close��slot��ready array�е�λ��*/
	newslot->slot_index = slot->slot_index; 
	/*����һ���µ�slot����ready���н������join*/
	log->slot_array[newslot->slot_index] = &log->slot_pool[pool_i];

	/*ԭ���Խ�close��slot����ΪWT_LOG_SLOT_PENDING״̬*/
	old_state = WT_ATOMIC_STORE8(slot->slot_state, WT_LOG_SLOT_PENDING);
	/*����log buffer�е����ݳ���*/
	slot->slot_group_size = (uint64_t)(old_state - WT_LOG_SLOT_READY);

	WT_STAT_FAST_CONN_INCRV(session, log_slot_consolidated, (uint64_t)slot->slot_group_size);

	return 0;
}

/*��slot��״ֻ̬Ϊwritting״̬*/
int __wt_log_slot_notify(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_UNUSED(session);

	slot->slot_state = (int64_t)WT_LOG_SLOT_DONE - (int64_t)slot->slot_group_size;
	return (0);
}

/*�ȴ�slot leader�ķ���дλ��*/
int __wt_log_slot_wait(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	int yield_count = 0;

	WT_UNUSED(session);

	while (slot->slot_state > WT_LOG_SLOT_DONE){
		if (++yield_count < 1000)
			__wt_yield();
		else
			__wt_sleep(0, 200);
	}

	return 0;
}

int64_t __wt_log_slot_release(WT_LOGSLOT *slot, uint64_t size)
{
	int64_t newsize;

	/*
	 * Add my size into the state.  When it reaches WT_LOG_SLOT_DONE
	 * all participatory threads have completed copying their piece.
	 */
	newsize = WT_ATOMIC_ADD8(slot->slot_state, (int64_t)size);
	return newsize;
}

/*��slot��Ϊ����ΪSLOT_FREE״̬���Ա�����SLOT READY,��������̻���slot flag��
 *���flag������Ҫgrow buffer�����󣬴˺������slot buf�ط��䣬�ռ�����ԭ����2��*/
int __wt_log_slot_free(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_DECL_RET;

	ret = 0;

	if(F_ISSET(slot, SLOT_BUF_GROW)){
		/*����ͳ����Ϣ����*/
		WT_STAT_FAST_CONN_INCR(session, log_buffer_grow);
		WT_STAT_FAST_CONN_INCRV(session, log_buffer_size, slot->slot_buf.memsize);

		WT_ERR(__wt_buf_grow(session, &slot->slot_buf, slot->slot_buf.memsize * 2));
	}

err:
	/*����slot״̬����*/
	slot->flags = SLOT_INIT_FLAGS;
	slot->slot_state = WT_LOG_SLOT_FREE;

	return ret;
}

/*��slot pool�е�slot����grow buffer��飬���Զ�Ӧ��slot buffer��grow����*/
int __wt_log_slot_grow_buffers(WT_SESSION_IMPL *session, size_t newsize)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int64_t orig_state;
	uint64_t old_size, total_growth;
	int i;

	conn = S2C(session);
	log = conn->log;
	total_growth = 0;

	/*����ͳ�Ƽ���*/
	WT_STAT_FAST_CONN_INCR(session, log_buffer_grow);

	/*���spin lock�Ƿ�ֹ�����߳�ͬʱgrow buffer*/
	__wt_spin_lock(session, &log->log_slot_lock);
	for(i < 0; i < SLOT_POOL; i++){
		slot = &log->slot_pool[i];

		/*����ʹ�õ�slot����grow buffer֮��,Ҳ�ܿ���Ӧ��slotԭ�Ӳ���spin*/
		if (slot->slot_state != WT_LOG_SLOT_FREE && slot->slot_state != WT_LOG_SLOT_READY)
			continue;

		/*��ǿ��û������grow buffer��slot����grow*/
		if(slot->slot_buf.memsize > 10 * newsize && !F_ISSET(slot, SLOT_BUF_GROW))
			continue;

		/*��ռslot*/
		orig_state = WT_ATOMIC_CAS_VAL8(slot->slot_state, WT_LOG_SLOT_FREE, WT_LOG_SLOT_PENDING);
		if (orig_state != WT_LOG_SLOT_FREE) {
			orig_state = WT_ATOMIC_CAS_VAL8(slot->slot_state, WT_LOG_SLOT_READY, WT_LOG_SLOT_PENDING);
			if (orig_state != WT_LOG_SLOT_READY)
				continue;
		}
		
		/*����grow buffer,ÿһ�Ŵ�ԭ����2���ռ�*/
		old_size = slot->slot_buf.memsize;
		F_CLR(slot, SLOT_BUF_GROW);
		WT_ERR(__wt_buf_grow(session, &slot->slot_buf, WT_MAX(slot->slot_buf.memsize * 2, newsize)));

		/*buffer grow������ɣ���Ҫ��slotԭ����״̬���û���*/
		slot->slot_state = orig_state;
		total_growth += slot->slot_buf.memsize - old_size;

		slot->slot_state = orig_state;
	}

err:
	__wt_spin_unlock(session, &log->log_slot_lock);
	WT_STAT_FAST_CONN_INCRV(session, log_buffer_size, total_growth);

	return ret;
}





