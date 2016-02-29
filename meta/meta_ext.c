
#include "wt_internal.h"

/*meta���ⲿ�ӿ�ʵ�֣�insert����*/
int __wt_ext_metadata_insert(WT_EXTENSION_API* wt_api, WT_SESSION *wt_session, const char *key, const char *value)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	return (__wt_metadata_insert(session, key, value));
}

/*meta���ⲿ�ӿ�ʵ�֣�remove����*/
int __wt_ext_metadata_remove(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	return (__wt_metadata_remove(session, key));
}

/*meta���ⲿ�ӿ�ʵ�֣�update����*/
int __wt_ext_metadata_update(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key, const char *value)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	return (__wt_metadata_update(session, key, value));
}

/*�ⲿ�ӿ�ʵ�֣�search����*/
int __wt_ext_metadata_search(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key, char **valuep)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_api->conn;
	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = conn->default_session;

	return (__wt_metadata_search(session, key, valuep));
}

int __wt_metadata_get_ckptlist(WT_SESSION *session, const char *name, WT_CKPT **ckptbasep)
{
	return (__wt_meta_ckptlist_get((WT_SESSION_IMPL *)session, name, ckptbasep));
}

void __wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
{
	__wt_meta_ckptlist_free((WT_SESSION_IMPL *)session, ckptbase);
}
