#include "wt_internal.h"

/*���ַ���ת���ɶ�Ӧ���޷���uint64_t�������磺��1234567�� ����ת��1234567*/
uint64_t __wt_strtouq(const char *nptr, char **endptr, int base)
{
	return strtouq(nptr, endptr, base);
}

