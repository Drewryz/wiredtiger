/***********************************************************************
* ����һ��page ref�� k/v��ֵ�Ե�����
***********************************************************************/

#include "wt_internal.h"

int __wt_kv_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_CURSOR *cursor;
	WT_PAGE *page;
	WT_ROW *rip;
	uint8_t v;

	switch (page->type){
	case WT_PAGE_COL_FIX:
		cursor->recno = cbt->recno;
		/*cursor��Ӧ����һ��upd,ֱ�ӷ���value*/
		if (upd != NULL){
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return 0;
		}

		v = __bit_getv_recno(page, cbt->iface.recno, btree->bitcnt);
		return __wt_buf_set(session, &cursor->value, &v, 1);
		
	case WT_PAGE_COL_VAR:
		cursor->recno = cbt->recno;

		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}
		/*��ö�Ӧ��cell,��ͨ��cell�õ�K/Vֵ*/
		cell = WT_COL_PTR(page, &page->pg_var_d[cbt->slot]);
		break;

	case WT_PAGE_ROW_LEAF:
		rip = &page->pg_row_d[cbt->slot];

		if (cbt->ins != NULL){ /*�����k/v��*/
			cursor->key.data = WT_INSERT_KEY(cbt->ins);
			cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
		}
		else if (cbt->compare == 0){/*�Ƚ�����λ���˶�Ӧ��k/v��*/
			cursor->key.data = cbt->search_key.data;
			cursor->key.size = cbt->search_key.size;
		}
		else
			WT_RET(__wt_row_leaf_key(session, page, rip, &cursor->key, 0)); /*����key��ֵ*/

		/*ֵ����append/update list���У��ӵ���ȡ*/
		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}
		/*����ֱ��ͨ��ripָ����value*/
		if (__wt_row_leaf_value(page, rip, &cursor->value))
			return 0;

		/*û�ж�λ����Ӧvalue���ڴ�λ�ã���ʾû���ҵ�*/
		if (cell = __wt_row_leaf_value_cell(page, rip, NULL) == NULL){
			cursor->value.size = 0;
			return 0;
		}
		break;

		WT_ILLEGAL_VALUE(session);
	}
	/*ͨ��cell��������Ӧ��valueֵ*/
	__wt_cell_unpack(cell, &unpack);
	WT_RET(__wt_page_cell_data_ref(session, page, &unpack, &cursor->value));

	return 0;
}


