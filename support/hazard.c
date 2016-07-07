/****************************************************************
* ʵ��session�����hazard pointer,����hazard pointerԭ������գ�
* https://en.wikipedia.org/wiki/Hazard_pointer
****************************************************************/

#include "wt_internal.h"


/*��һ��page��Ϊhazard pointer���õ�session hazard pointer list��*/
int __wt_hazard_set(WT_SESSION_IMPL* session, WT_REF* ref, int* busyp)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	int restarts = 0;

	btree = S2BT(session);
	conn = S2C(session);
	*busyp = 0;

	/*btree�����������̭page,hazard pointer���������*/
	if (F_ISSET(btree, WT_BTREE_NO_HAZARD))
		return 0;

	for (hp = session->hazard + session->nhazard;; ++hp){
		/*hp����hazard�����鷶Χ,��Ҫ��������������ǳ�ʼɨ�������Χ�Ļ�����λ��hazard�Ŀ�ʼ���ּ���ɨ��,���hazard���Ļ����Ŵ�session->hazard_size��������̲��ܳ���session->hazard_max*/
		if (hp >= session->hazard + session->hazard_size){
			if ((hp >= session->hazard + session->hazard_size) && restarts++ == 0)
				hp = session->hazard;
			else if (session->hazard_size >= conn->hazard_max)	/*����session�洢hazard pointer������,����һ��ϵͳ�쳣*/
				break;
			else /*�Ŵ�hazard size,����ͨ�����ڴ�������ʵ�֣���ֹCPU����ִ����ɳ��򲢷��쳣*/
				WT_PUBLISH(session->hazard_size, WT_MIN(session->hazard_size + WT_HAZARD_INCR, conn->hazard_max));
		}

		/*���λ�ñ�����hazard pointerռ���ˣ�Ѱ����һ����λ*/
		if (hp->page != NULL)
			continue;

		hp->page = ref->page;
		/*���������õ�hazard pointer���������ڴ�����Ϊ��д��Ч����ֹ��ִ���ڴ����Ϻ���Ĵ���*/
		WT_FULL_BARRIER();

		/*�п��ܶ���߳�ͬʱִ��hp->page = ref->page�� ���ʱ����ܳ��������̰߳�page ������ڴ棬��ʱ��Ӧ�÷���hazard pointer����������һ��æ״̬���еȴ�*/
		if (ref->page == hp->page && ref->state == WT_REF_MEM) {
			++session->nhazard;
			return 0;
		}

		hp->page = NULL;
		*busyp = 1;
		return 0;
	}

	return ENOMEM;
}

/*��session hazard�б����һ��hazard pointer*/
int __wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_HAZARD *hp;

	btree = S2BT(session);

	/*btree����page��̭��Ҳ�Ͳ�����hazard pointer*/
	if (F_ISSET(btree, WT_BTREE_NO_HAZARD))
		return 0;

	for (hp = session->hazard + session->hazard_size - 1; hp >= session->hazard; --hp){
		if (hp->page == page){
			/*����ط�����Ҫ���ڴ���������֤����Ϊhp->page������NULL�Ĺ��̣�����Ҫ��֤��ȫ��ȷ*/
			hp->page = NULL;
			--session->nhazard; /*���ֵ�ڻ᲻����ָ����أ�*/
			return 0;
		}
	}

	WT_PANIC_RET(session, EINVAL, "session %p: clear hazard pointer: %p: not found", session, page);
}

/*�����session hazard�б������е�hazard pointer*/
void __wt_hazard_close(WT_SESSION_IMPL* session)
{
	WT_HAZARD *hp;
	int found;

	/*������û��hazard pointer*/
	for (found = 0, hp = session->hazard; hp < session->hazard + session->hazard_size; ++hp){
		if (hp->page != NULL) {
			found = 1;
			break;
		}
	}

	/*hazard arrays�ǿյģ�ֱ�ӷ���*/
	if (session->nhazard == 0 && !found)
		return;

	/*���hazard pointer*/
	for (hp = session->hazard; hp < session->hazard + session->hazard_size; ++hp){
		if (hp->page != NULL) { 
			hp->page = NULL;
			--session->nhazard;
		}
	}

	if (session->nhazard != 0)
		__wt_errx(session, "session %p: close hazard pointer table: count didn't match entries",session);
}




