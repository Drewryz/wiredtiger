#include "wiredtiger.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

typedef struct
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
}db_info_t;

typedef struct{
	uint32_t start;
	uint32_t end;
}item_t;

static int wcount = 0;
uint64_t total_count = 0;
WT_CONNECTION *conn;

#define TAB_META "block_compressor=zlib,key_format=i,value_format=S,internal_page_max=16KB,leaf_page_max=16KB,leaf_value_max=16KB,os_cache_max=1GB"
#define RAW_META "key_format=i,value_format=S,internal_page_max=16KB,leaf_page_max=64KB,leaf_value_max=64KB,os_cache_max=1GB"

static int setup(db_info_t* info, int create_table)
{
	int ret;
	ret = conn->open_session(conn, NULL, "isolation=snapshot", &info->session);
	if (ret != 0){
		printf("open_session failed!\n");
		return ret;
	}

	if (create_table){
		info->session->create(info->session, "table:mytable", RAW_META);
	}

	ret = info->session->open_cursor(info->session, "table:mytable", NULL, NULL, &info->cursor);
	if (ret != 0){
		printf("open_cursor failed!\n");
		return ret;
	}

	return ret;
}

static void clean(db_info_t* info)
{
	if (info->cursor != NULL){
		info->cursor->close(info->cursor);
		info->cursor = NULL;
	}

	if (info->session != NULL){
		info->session->close(info->session, NULL);
		info->session = NULL;
	}
}

static void* write_thr(void* arg)
{
	db_info_t db = { NULL, NULL };
	uint32_t key;
	char value[1024] = { 0 };
	int ret;
	item_t* item = arg;

	if (setup(&db, 0) != 0){
		printf("write thread setup db failed!\n");
		return NULL;
	}

	for (key = item->start; key < item->end; key++){
		db.cursor->set_key(db.cursor, key);
		sprintf(value, "%uzerokwerii939kdd,cgrfkg-@$$$**%u&XXZZamvbzc44k445i0915323/=-d2224===++--dkeiwnd,.,.,aamggnvcxvzczz|<>!-slsdshssrq2934745755mbikdd()!!%u",key, key, key);
		db.cursor->set_value(db.cursor, value);
		if ((ret = db.cursor->insert(db.cursor)) != 0){
			printf("insert k/v failed, code = %u\n", ret);
		}
	}

	clean(&db);

	return NULL;
}

#define COUNT		1000000
#define THREAD_NUM	1
#define READ_COUNT  20000

static void* read_thr(void* arg)
{
	db_info_t db = { NULL, NULL };
	uint32_t key;
	char value[1024] = { 0 };
	int ret, i;
	int per_count;
	char *val;
	struct timeval e;
	gettimeofday(&e, NULL);

	srand(e.tv_usec);

	if (setup(&db, 0) != 0){
		printf("write thread setup db failed!\n");
		return NULL;
	}

	for (i = 0; i < READ_COUNT; i++){
		per_count = total_count / READ_COUNT;
		key = rand() % per_count + i * per_count;

		db.cursor->reset(db.cursor);
		db.cursor->set_key(db.cursor, key);
		ret = db.cursor->search(db.cursor);
		if (ret == 0){
			ret = db.cursor->get_key(db.cursor, &key);
			ret = db.cursor->get_value(db.cursor, &val);
		}
		else
			printf("search failed!\n");
	}

	clean(&db);

	return NULL;
}


#define WT_CONFIG "create,cache_size=1GB,log=(archive=,compressor=,enabled=false,file_max=200MB,path=,prealloc=,recover=on), extensions=[/usr/local/lib/libwiredtiger_zlib.so],statistics=(all=1)"

static FILE *logfp;				/* Log file */

static int  handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);

int main(int argc, const char* argv[])
{
	db_info_t db = { NULL, NULL };
	uint32_t key;
	char value[1024] = { 0 };
	char *val;
	int ret;
	int per_count = 0;

	int i;
	item_t item[THREAD_NUM];
	pthread_t ids[THREAD_NUM];

	struct timeval e, b;
	uint32_t delay = 0;

	static WT_EVENT_HANDLER event_handler = {
		handle_error,
		handle_message,
		NULL,
		NULL	/* Close handler. */
	};

	if (argc != 2)
		return;
	
	srand(time(NULL));

	total_count = atoi(argv[1]) * COUNT;

	const char* home = "WT_HOME";

	ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	logfp = fopen("test.log", "w");

	if ((ret = wiredtiger_open(home, &event_handler, WT_CONFIG, &conn) != 0)){
		printf("wiredtiger_open failed!\n");
		exit(0);
	}

	if (setup(&db, 1) != 0)
		goto close_conn;

	gettimeofday(&b, NULL);

	for (i = 0; i < THREAD_NUM; i++){
		item[i].start = i * (total_count / THREAD_NUM);
		item[i].end = (i + 1)*(total_count / THREAD_NUM);
		pthread_create(ids + i, NULL, write_thr, &item[i]);
	}

	for (i = 0; i < THREAD_NUM; i++)
		pthread_join(ids[i], NULL);

	printf("insert finished!\n");

	if ((ret = db.session->checkpoint(db.session, NULL)) != 0)
		printf("create checkpiont failed!\n");
	else
		printf("finish checkpoint!\n");
	

	gettimeofday(&e, NULL);
	delay = 1000 * (e.tv_sec - b.tv_sec) + (e.tv_usec - b.tv_usec) / 1000;
	printf("inerst row count = %u, insert tps = %u\n", total_count, total_count*1000/delay);

	getchar();
	
	gettimeofday(&b, NULL);
	for (i = 0; i < THREAD_NUM; i++){
		pthread_create(ids + i, NULL, read_thr, NULL);
	}
	for (i = 0; i < THREAD_NUM; i++)
		pthread_join(ids[i], NULL);

	gettimeofday(&e, NULL);
	delay = 1000 * (e.tv_sec - b.tv_sec) + (e.tv_usec - b.tv_usec) / 1000;
	printf("query tps = %u\n", (THREAD_NUM * READ_COUNT) * 1000 / delay);
	
	printf("finish query\n");
	getchar();

	clean(&db);

	printf("find clean session\n");
	getchar();

close_conn:
	if ((ret = conn->close(conn, NULL)) != 0)
		printf("wiredtiger_close failed!\n");
	else{
		printf("last getchar!\n");
		getchar();
	}

	fclose(logfp);
}

static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *errmsg)
{
	(void)(handler);
	(void)(session);
	(void)(error);

	return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
	(void)(handler);
	(void)(session);

	if (logfp != NULL)
		return (fprintf(logfp, "%s\n", message) < 0 ? -1 : 0);

	return (printf("%s\n", message) < 0 ? -1 : 0);
}

