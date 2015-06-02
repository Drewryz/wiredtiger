/****************************************************************************
*��pack/unpack��API��װ,����printf/scanf�Ŀɱ������װ
****************************************************************************/

#include "wt_internal.h"


/*�Ը�ʽ���ṹ����pack,�ڴ����buffer,�൱��printf*/
int wiredtiger_struct_pack(WT_SESSION* wt_session, void* buffer, size_t size, const char* fmt, ...)
{	
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_packv(session, buffer, size, fmt, ap);
	va_end(ap);

	return ret;
}

/*�Ը�ʽ�������ݵ�pack���Ƚ��м����pack*/
int wiredtiger_struct_size(WT_SESSION* wt_session, size_t* sizep, const char* fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL* session;
	va_list ap;

	session = (WT_SESSION_IMPL* )wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_sizev(session, sizep, fmt, ap);
	va_end(ap);

	return ret;
}

/*��buffer�е����ݸ�ʽ��unpack���൱��scanf*/
int wiredtiger_struct_unpack(WT_SESSION *wt_session, const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(session, buffer, size, fmt, ap);
	va_end(ap);

	return ret;
}

/*��wt_api�����pack*/
int __wt_ext_struct_pack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
	void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;
	
	/*���wt_session = NULL,��wt_api�л�ȡһ��Ĭ�ϵ�session�������ò���*/
	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
		((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_packv(session, buffer, size, fmt, ap);
	va_end(ap);

	return ret;
}

int __wt_ext_struct_size(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
	size_t *sizep, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
		((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_sizev(session, sizep, fmt, ap);
	va_end(ap);

	return ret;
}

int __wt_ext_struct_unpack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
	const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
		((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(session, buffer, size, fmt, ap);
	va_end(ap);

	return ret;
}









