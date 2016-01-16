/***************************************************************
* ������ⲿAPI�ӿ�
***************************************************************/

#include "wt_internal.h"

/*����sessionִ�������ID����������̻���cacheռ���ʣ����ռ��̫�߾ͻ�evict page,
 *���txn idû�����ɣ�������һ��ȫ�ֵ�txn id����
 */
uint64_t __wt_ext_transaction_id(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session)
{
	WT_SESSION_IMPL* session;

	(void)wt_api;
	session = (WT_SESSION_IMPL*)wt_session;

	__wt_txn_id_check(session);
	return (session->txn.id);
}

/*���session�ĸ��뼶��*/
int __wt_ext_transaction_isolation_level(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session)
{
	WT_SESSION_IMPL* session;
	WT_TXN* txn;

	(void)wt_api;					/* Unused parameters */
	session = (WT_SESSION_IMPL *)wt_session;
	txn = &session->txn;

	if (txn->isolation == TXN_ISO_READ_COMMITTED)
		return WT_TXN_ISO_READ_COMMITTED;
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED)
		return WT_TXN_ISO_READ_UNCOMMITTED;
	return WT_TXN_ISO_SNAPSHOT;
}

/*�����ⲿ��һ������ص�notify����*/
int __wt_ext_transaction_notify(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, WT_TXN_NOTIFY *notify)
{
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	(void)wt_api;					/* Unused parameters */

	session = (WT_SESSION_IMPL* )wt_session;
	txn = &session->txn;

	if(txn->notify == notify)
		return 0;
	if(txn->notify != NULL)
		return ENOMEM;

	txn->notify = notify;

	return 0;
}

/*���������δ����������ID*/
uint64_t __wt_ext_transaction_oldest(WT_EXTENSION_API* wt_api)
{
	return (((WT_CONNECTION_IMPL *)wt_api->conn)->txn_global.oldest_id);
}

/*�ж�transaction_id�Ƿ��sessionִ�е�����ɼ�*/
int __wt_ext_transaction_visible(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, uint64_t transaction_id)
{
	(void)wt_api;					/* Unused parameters */
	return (__wt_txn_visible((WT_SESSION_IMPL *)wt_session, transaction_id));
}




