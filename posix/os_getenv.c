#include "wt_internal.h"

/*��ȡvariableָ���Ļ�������*/
int __wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp)
{
	const char* temp;
	*envp = NULL;

	if(((temp = getenv(variable)) != NULL) && strlen(temp) > 0)
		return __wt_strdup(session, temp, envp);

	return WT_NOTFOUND;
}





