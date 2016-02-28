
#include "wt_internal.h"

/*����һ��meta file��Ĭ������*/
static int __metadata_config(WT_SESSION_IMPL* session, char** metaconfp)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	const char* cfg[] = {WT_CONFIG_BASE(session, file_meta), NULL, NULL};
	char* metaconf;

	*metaconfp = NULL;
	metaconf = NULL;

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "key_format=S,value_format=S,id=%d,version=(major=%d,minor=%d)", 
		WT_METAFILE_ID, WT_BTREE_MAJOR_VERSION_MAX, WT_BTREE_MINOR_VERSION_MAX));
	cfg[1] = buf->data;
	WT_ERR(__wt_config_collapse(session, cfg, &metaconf));

	*metaconfp = metaconf;

	if(0){
err:
		__wt_free(session, metaconf);
	}

	__wt_scr_free(session, &buf);
}

/*��ʼ��meta��Ϣ������һ��meta file�ļ�*/
static int __metadata_init(WT_SESSION_IMPL* session)
{
	WT_DECL_RET;

	WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, WT_METAFILE_URI, NULL));

	return ret;
}

/*����hot backup file������*/
static int __metadata_load_hot_backup(WT_SESSION_IMPL* session)
{
	FILE *fp;
	WT_DECL_ITEM(key);
	WT_DECL_ITEM(value);
	WT_DECL_RET;
	int exist;

	/*��鱸���ļ��Ƿ����*/
	WT_RET(__wt_exist(session, WT_METADATA_BACKUP, &exist));
	if(!exist) /*�ļ�������*/
		return 0;

	/*��readģʽ�򿪱���Ԫ�����ļ�*/
	WT_RET(__wt_fopen(session, WT_METADATA_BACKUP, WT_FHANDLE_READ, 0, &fp));

	/* Read line pairs and load them into the metadata file. */
	WT_ERR(__wt_scr_alloc(session, 512, &key));
	WT_ERR(__wt_scr_alloc(session, 512, &value));
	for (;;) {
		/*��ȡ����Ԫ�ļ��е�kv�ԣ������µ�metadata��*/
		WT_ERR(__wt_getline(session, key, fp));
		if (key->size == 0)
			break;

		WT_ERR(__wt_getline(session, value, fp));
		if (value->size == 0)
			WT_ERR(__wt_illegal_value(session, WT_METADATA_BACKUP));

		WT_ERR(__wt_metadata_update(session, key->data, value->data));
	}

	F_SET(S2C(session), WT_CONN_WAS_BACKUP);

err:
	WT_TRET(__wt_fclose(&fp, WT_FHANDLE_READ));
	__wt_scr_free(session, &key);
	__wt_scr_free(session, &value);

	return ret;
}

/*Create any bulk-loaded file stubs.*/
static int __metadata_load_bulk(WT_SESSION_IMPL* session)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	uint32_t allocsize;
	int exist;
	const char *filecfg[] = { WT_CONFIG_BASE(session, file_meta), NULL };
	const char *key;

	/*
	 * If a file was being bulk-loaded during the hot backup, it will appear
	 * in the metadata file, but the file won't exist.  Create on demand.
	 */
	WT_ERR(__wt_metadata_cursor(session, NULL, &cursor));
	while((ret = cursor->next(cursor)) == 0){
		WT_ERR(cursor->get_key(cursor, &key));

		if (!WT_PREFIX_SKIP(key, "file:"))
			continue;

		WT_ERR(__wt_exist(session, key, &exist));
		if (exist)
			continue;

		WT_ERR(__wt_direct_io_size_check(session, filecfg, "allocation_size", &allocsize));
		WT_ERR(__wt_block_manager_create(session, key, allocsize));
	}
	WT_ERR_NOTFOUND_OK(ret);

err:
	if (cursor != NULL)
		WT_TRET(cursor->close(cursor));

	return ret;
}

/*��meta turtle�ļ����м��ͳ�ʼ��*/
int __wt_turtle_init(WT_SESSION_IMPL* session)
{
	WT_DECL_RET;
	int exist, exist_incr;
	char *metaconf;

	metaconf = NULL;

	/*����turtle.set�ļ����ڣ��Ƚ���ɾ��*/
	WT_RET(__wt_remove_if_exists(session, WT_METADATA_TURTLE_SET));

	WT_RET(__wt_exist(session, WT_INCREMENTAL_BACKUP, &exist_incr));
	WT_RET(__wt_exist(session, WT_METADATA_TURTLE, &exist));
	if(exist){
		if (exist_incr)
			WT_RET_MSG(session, EINVAL, "Incremental backup after running recovery is not allowed.");

		return 0;
	}

	if(exist_incr)
		F_SET(S2C(session), WT_CONN_WAS_BACKUP);

	/* Create the metadata file. */
	WT_RET(__metadata_init(session));

	/* Load any hot-backup information. */
	WT_RET(__metadata_load_hot_backup(session));

	/* Create any bulk-loaded file stubs. */
	WT_RET(__metadata_load_bulk(session));

	/* Create the turtle file. */
	WT_RET(__metadata_config(session, &metaconf));
	WT_ERR(__wt_turtle_update(session, WT_METAFILE_URI, metaconf));

	/* Remove the backup files, we'll never read them again. */
	WT_ERR(__wt_backup_file_remove(session));

err:
	__wt_free(session, metaconf);
	return ret;
}

/*��ȡturtle�ļ�,���ҵ�key��Ӧ��valueֵ����*/
int __wt_turtle_read(WT_SESSION_IMPL* session, const char* key, char* valuep)
{
	FILE *fp;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	int exist, match;

	*valuep = NULL;

	WT_RET(__wt_exist(session, WT_METADATA_TURTLE, &exist));
	if (!exist)
		return (strcmp(key, WT_METAFILE_URI) == 0 ? __metadata_config(session, valuep) : WT_NOTFOUND);

	WT_RET(__wt_fopen(session, WT_METADATA_TURTLE, WT_FHANDLE_READ, 0, &fp));

	/* Search for the key. */
	WT_ERR(__wt_scr_alloc(session, 512, &buf));
	for (match = 0;;) {
		WT_ERR(__wt_getline(session, buf, fp));
		if (buf->size == 0)
			WT_ERR(WT_NOTFOUND);
		if (strcmp(key, buf->data) == 0)
			match = 1;

		/* Key matched: read the subsequent line for the value. */
		WT_ERR(__wt_getline(session, buf, fp));
		if (buf->size == 0)
			WT_ERR(__wt_illegal_value(session, WT_METADATA_TURTLE));
		if (match)
			break;
	}

	/* Copy the value for the caller. */
	WT_ERR(__wt_strdup(session, buf->data, valuep));

err:
	WT_TRET(__wt_fclose(&fp, WT_FHANDLE_READ));
	__wt_scr_free(session, &buf);

	return ret;
}

/*��turtle�ļ��е�key��Ӧ��ֵ����Ϊvalue*/
int __wt_turtle_update(WT_SESSION_IMPL* session, const char* key, const char* value)
{
	WT_FH *fh;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	int vmajor, vminor, vpatch;
	const char *version;

	fh = NULL;

	/*�ȴ�һ����ʱ�ļ�����д��key/valueֵ��д����Ϻ���ʱ�ļ��޸�Ϊ��ʽ��Wiredtiger.turtle�ļ���
	 *��������Ŀ���Ƿ�ֹ��д�Ĺ��̶����������Ϣ������ʱ�ļ����滻���Ա����������
	 */
	WT_RET(__wt_open(session, WT_METADATA_TURTLE_SET, 1, 1, WT_FILE_TYPE_TURTLE, &fh));

	version = wiredtiger_version(&vmajor, &vminor, &vpatch);
	WT_ERR(__wt_scr_alloc(session, 2 * 1024, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "%s\n%s\n%s\n" "major=%d,minor=%d,patch=%d\n%s\n%s\n",
		WT_METADATA_VERSION_STR, version, WT_METADATA_VERSION, vmajor, vminor, vpatch, key, value));

	WT_ERR(__wt_write(session, fh, 0, buf->size, buf->data));

	/* Flush the handle and rename the file into place. */
	ret = __wt_sync_and_rename_fh(session, &fh, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE);

	/* Close any file handle left open, remove any temporary file. */
err:	
	WT_TRET(__wt_close(session, &fh));
	WT_TRET(__wt_remove_if_exists(session, WT_METADATA_TURTLE_SET));

	__wt_scr_free(session, &buf);
	return ret;
}






