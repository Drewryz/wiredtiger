#include "wt_internal.h"

/*���ļ��ж�ȡһ���ַ���,������wt_item��*/
/*
int __wt_getline(WT_SESSION_IMPL *session, WT_ITEM *buf, FILE *fp)
{
	int c;

	WT_RET(__wt_buf_init(session, buf, 100));
	
	while((c = fgetc(fp)) != EOF){
		WT_RET(__wt_buf_extend(session, buf, buf->size + 2));
		if (c == '\n') {
			if (buf->size == 0)
				continue;
			break;
		}

		((char *)buf->mem)[buf->size++] = (char)c;
	}

	if(c == EOF && ferror(fp))
		WT_RET_MSG(session, __wt_errno(), "file read");

	((char *)buf->mem)[buf->size] = '\0';

	return 0;
}*/



