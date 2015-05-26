#include "wt_internal.h"

/*��selectϵͳ��������ʱ*/
void __wt_sleep(uint64_t seconds, uint64_t micro_seconds)
{
	struct timeval t;

	t.tv_sec = (time_t)(seconds + micro_seconds / 1000000);
	t.tv_usec = (suseconds_t)(micro_seconds % 1000000);

	select(0, NULL, NULL, NULL, &t);
}


