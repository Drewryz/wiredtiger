#include "wt_internal.h"

/*�жϽ���ִ��Ȩ��*/
int __wt_has_priv(void)
{
	return (getuid() != geteuid() || getgid() != getegid());
}



