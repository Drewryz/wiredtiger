/****************************************************************************
*��intpack���̵Ĳ���
****************************************************************************/

#include <assert.h>
#include "wt_internal.h"

void test_pack()
{
	uint8_t buf[10], *p, *end;
	int64_t i;

	for(i = 1; i < (1LL << 60); i <<= 1){
		end = buf;

		/*�޷���shu*/
		assert(__wt_vpack_uint(&end, sizeof(buf), (uint64_t)i) == 0);
		printf("%" PRId64 " ", i);

		for (p = buf; p < end; p++)
			printf("%02x", *p);

		printf("\n");

		end = buf;
		assert(__wt_vpack_int(&end, sizeof(buf), -i) == 0);
		printf("%" PRId64 " ", -i);
		for (p = buf; p < end; p++)
			printf("%02x", *p);

		printf("\n");
	}
}

void test_pack_unpack()
{
	const uint8_t *cp;
	uint8_t buf[10], *p;
	uint64_t ncalls, r, r2, s;
	int i;

	ncalls = 0;

	/*����10�ڴ�*/
	for(i = 0; i < 100000000; i ++){
		for (s = 0; s < 50; s += 5) {
			++ncalls;
			r = 1ULL << s;

			p = buf;
			/*��r pack��buf��*/
			assert(__wt_vpack_uint(&p, sizeof(buf), r) == 0);
			cp = buf;
			/*��buf�е�����unpack��r2*/
			assert(__wt_vunpack_uint(&cp, sizeof(buf), &r2) == 0);
			/*����r��r2��У��*/
			if(r != r2){
				fprintf(stderr, "mismatch!\n");
				break;
			}
		}
	}

	printf("Number of calls: %llu\n", (unsigned long long)ncalls);
}

static size_t pack_struct(char* buf, size_t size, const char *fmt, ...)
{
	char *end, *p;
	va_list ap;
	size_t len;

	len = 0;

	/*����һ����ʽ����structռ�õĳ��ȿռ�*/
	va_start(ap, fmt);
	assert(__wt_struct_sizev(NULL, &len, fmt, ap) == 0);
	va_end(ap);
	 
	assert(len < size);

	/*����һ����ʽ����struct����*/
	va_start(ap, fmt);
	assert(__wt_struct_packv(NULL, buf, size, fmt, ap) == 0);
	va_end(ap);

	printf("%s ", fmt);
	for (p = buf, end = p + len; p < end; p++)
		printf("%02x", *p & 0xff);

	printf("\n");

	return len;
}

static void unpack_struct(char* buf, size_t size, const char* fmt, ...)
{
	va_list ap;
	 
	va_start(ap, fmt);
	assert(__wt_struct_unpackv(NULL, buf, size, fmt, ap) == 0);
	va_end(ap);
}


void test_pack_struct()
{
	char buf[256] = {0};
	int i1, i2, i3;
	char* s;
	/*���л��������*/
	size_t len = pack_struct(buf, 256, "iiS", 0, 101, "zerok");
	/*�����л��������*/
	unpack_struct(buf, len, "iiS", &i1, &i2, &s);

	printf("i1 = %d, i2 = %d, i3 = %s\n", i1, i2, s);
}

int main()
{
	//test_pack();
	//test_pack_unpack();
	//test_pack_struct();
}



