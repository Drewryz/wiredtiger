
#include "wt_internal.h"

/*��ȡwiredtiger�İ汾��*/
const char* wiredtiger_version(int *majorp, int *minorp, int *patchp)
{
	if (majorp != NULL)
		*majorp = WIREDTIGER_VERSION_MAJOR;
	if (minorp != NULL)
		*minorp = WIREDTIGER_VERSION_MINOR;
	if (patchp != NULL)
		*patchp = WIREDTIGER_VERSION_PATCH;

	return WIREDTIGER_VERSION_STRING;
}


