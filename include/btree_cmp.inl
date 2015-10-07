/*******************************************************************
*btree�ıȽϺ���
*******************************************************************/

/*�Ƚ�user��item���ڴ����ݵĴ�С*/
static inline int __wt_lex_compare(const WT_ITEM* user_item, const WT_ITEM* tree_item)
{
	size_t len, usz, tsz;
	const uint8_t* userp, *treep;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz);

	userp = user_item->data;
	treep = tree_item->data;

	for(; len > 0; --len, ++userp, ++treep){
		if(*userp != *treep)
			return (*userp < *treep ? -1 : 1);
	}

	return ((usz == tsz) ? 0 : ((usz < tsz) ? -1 : 1));
}

static inline int __wt_compare(WT_SESSION_IMPL* session, WT_COLLATOR* collator, const WT_ITEM* user_item, 
	const WT_ITEM* tree_item, int *cmpp)
{
	if(collator == NULL){
		*cmpp = __wt_lex_compare(user_item, tree_item);
		return 0;
	}

	return collator->compare(collator, &session->iface, user_item, tree_item, cmpp);
}

/*������ʼ��matchp֮������ݱȽϣ�ֻ�ȽϺ�����������ݴ�С,�ȽϹ������ݳ��Ȼ����ӵ�matchp��*/
static inline int __wt_lex_compare_skip(const WT_ITEM *user_item, const WT_ITEM *tree_item, size_t *matchp)
{
	size_t len, usz, tsz;
	const uint8_t *userp, *treep;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz) - *matchp;

	userp = (uint8_t *)user_item->data + *matchp;
	treep = (uint8_t *)tree_item->data + *matchp;

	for (; len > 0; --len, ++userp, ++treep, ++*matchp)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

static inline int __wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
	const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp, size_t *matchp)
{
	if (collator == NULL) {
		*cmpp = __wt_lex_compare_skip(user_item, tree_item, matchp);
		return 0;
	}
	/*��ָ��У�����ķ�ʽ���бȽ�*/
	return collator->compare(collator, &session->iface, user_item, tree_item, cmpp);
}

