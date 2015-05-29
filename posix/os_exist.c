
#include "wt_internal.h"

/*�ж��ļ�filename�Ƿ����,�����ϵͳ����stat*/
int __wt_exist(WT_SESSION_IMPL* session, const char* filename, int* existp)
{
	struct stat sb;
	char* path;

	WT_DECL_RET;
	*existp = 0;
	
	WT_RET(__wt_filename(session, filename, &path));
	WT_SYSCALL_RETRY(stat(path, &sb), ret);

	__wt_free(session, path);

	if(ret == 0){
		*existp = 1;
		return 0;
	}

	if(ret == ENOENT)
		return 0;

	WT_RET_MSG(session, ret, "%s:fstat", filename);
}


