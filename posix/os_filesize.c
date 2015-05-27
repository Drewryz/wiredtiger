#include "wt_internal.h"

/*ͨ��WT_FH�ṹ��ȡ�ļ���С*/
int __wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	struct stat sb;
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fstat", fh->name));

	WT_SYSCALL_RETRY(fstat(fh->fd, &sb), ret);
	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}

	WT_RET_MSG(session, ret, "%s: fstat", fh->name);
}

/*ͨ���ļ�����ȡ�ļ���С*/
int __wt_filesize_name(WT_SESSION_IMPL *session, const char *filename, wt_off_t *sizep)
{
	struct stat sb;
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, filename, &path));

	WT_SYSCALL_RETRY(stat(path, &sb), ret);

	__wt_free(session, path);

	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}

	WT_RET_MSG(session, ret, "%s: fstat", filename);
}


