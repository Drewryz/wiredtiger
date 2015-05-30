#include "wt_internal.h"

#include <linux/falloc.h>
#include <sys/syscall.h>

void __wt_fallocate_config(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);

	fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
	fh->fallocate_requires_locking = 0;

	fh->fallocate_available = WT_FALLOCATE_AVAILABLE;
	fh->fallocate_requires_locking = 1;
}

/*Ϊ�ļ�Ԥ������ռ䣬��offset��Ԥ��len���ȵĿռ�,TODO:��Ҫȷ���᲻���Զ������ļ�?*/
static int __wt_std_fallocate(WT_FH* fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

	WT_SYSCALL_RETRY(fallocate(fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
	return ret;
}

/*�����ں˺������ļ�Ԥ���ռ䴦��*/
static int __wt_sys_fallocate(WT_FH* fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

	WT_SYSCALL_RETRY(syscall(SYS_fallocate, fh->fd, FALLOC_FL_KEEP_SIZE, offset, len), ret);
}

/*����posix�������ļ��ռ�Ԥ��,����ļ���СС��len + offset�����Զ������������*/
static int __wt_posix_fallocate(WT_FH* fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

	WT_SYSCALL_RETRY(posix_fallocate(fh->fd, offset, len), ret);
	return ret;
}

/*���ļ����пռ�Ԥ���ĵ��ú���*/
int __wt_fallocate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_DECL_RET;

	switch(fh->fallocate_available){
	case WT_FALLOCATE_POSIX:
		WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: posix_fallocate", fh->name));
		if((ret = __wt_posix_fallocate(fh, offset, len)) == 0)
			return 0;

		WT_RET_MSG(session, ret, "%s: posix_fallocate", fh->name);

	case WT_FALLOCATE_STD:
		WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fallocate", fh->name));
		if ((ret = __wt_std_fallocate(fh, offset, len)) == 0)
			return 0;

		WT_RET_MSG(session, ret, "%s: fallocate", fh->name);

	case WT_FALLOCATE_SYS:
		WT_RET(__wt_verbose(
			session, WT_VERB_FILEOPS, "%s: sys_fallocate", fh->name));
		if ((ret = __wt_sys_fallocate(fh, offset, len)) == 0)
			return 0;

		WT_RET_MSG(session, ret, "%s: sys_fallocate", fh->name);

	case WT_FALLOCATE_AVAILABLE:
		/* �ȵ���__wt_std_fallocate�����ʧ���ٵ���__wt_sys_fallocate�� ���ʧ�����__wt_posix_fallocate
		 * ���ȫ��ʧ�ܣ��ͻ��ߵ�WT_FALLOCATE_NOT_AVAILABLE���״̬*/
		if ((ret = __wt_std_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_STD;
			fh->fallocate_requires_locking = 0;
			return (0);
		}
		if ((ret = __wt_sys_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_SYS;
			fh->fallocate_requires_locking = 0;
			return (0);
		}
		if ((ret = __wt_posix_fallocate(fh, offset, len)) == 0) {
			fh->fallocate_available = WT_FALLOCATE_POSIX;
			fh->fallocate_requires_locking = 0;
			return (0);
		}

	case WT_FALLOCATE_NOT_AVAILABLE:
	default:
		fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
		return ENOTSUP;
	}
}






