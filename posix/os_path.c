#include "wt_internal.h"

/*�ж�һ���ļ����Ƿ������·��,���������·��������1,���򷵻�0*/
int __wt_absolute_path(const char *path)
{
	return (path[0] == '/' ? 1 : 0)
}

/*���Ŀ¼�ָ���*/
const char* __wt_path_separator()
{
	return ("/");
}


