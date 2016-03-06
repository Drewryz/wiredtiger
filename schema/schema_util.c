
#include "wt_internal.h"

/*ͨ��name���ֲ��Ҷ�Ӧ��data source�����û�в��ҵ�������NULL*/
WT_DATA_SOURCE* __wt_schema_get_source(WT_SESSION_IMPL *session, const char *name)
{
	WT_NAMED_DATA_SOURCE *ndsrc;

	TAILQ_FOREACH(ndsrc, &S2C(session)->dsrcqh, q)
		if (WT_PREFIX_MATCH(name, ndsrc->prefix))
			return (ndsrc->dsrc);

	return NULL;
}

/*���str���Ƿ��к�"WiredTiger"һ��������������У��׳�һ��������Ϣ*/
int __wt_str_name_check(WT_SESSION_IMPL* session, const char* str)
{
	const char *name, *sep;
	int skipped;

	name = str;
	for(skipped = 0; skipped < 2; skipped++){
		if ((sep = strchr(name, ':')) == NULL)
			break;

		name = sep + 1;
		if (WT_PREFIX_MATCH(name, "WiredTiger"))
			WT_RET_MSG(session, EINVAL, "%s: the \"WiredTiger\" name space may not be used by applications", name);
	}

	/*
	 * Disallow JSON quoting characters -- the config string parsing code
	 * supports quoted strings, but there's no good reason to use them in
	 * names and we're not going to do the testing.
	 * Ҳ������json��ʽ��������
	 */
	if (strpbrk(name, "{},:[]\\\"'") != NULL)
		WT_RET_MSG(session, EINVAL, "%s: WiredTiger objects should not include grouping characters in their names", name);

	return 0;
}

/*���str���Ƿ��к�"WiredTiger"һ��������������У��׳�һ��������Ϣ,����������ϸ�������������ָ�����ַ����ĳ��ȷ�Χ
 *Ҳ����˵���Խ���һ�����ַ�����һ�����ַ����ıȽϡ�
 */
int __wt_name_check(WT_SESSION_IMPL *session, const char *str, size_t len)
{
	WT_DECL_RET;
	WT_DECL_ITEM(tmp);

	WT_RET(__wt_scr_alloc(session, len, &tmp));
	WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)len, str));

	ret = __wt_str_name_check(session, (const char*)tmp->data);

err:	__wt_scr_free(session, &tmp);
	return ret;
}

