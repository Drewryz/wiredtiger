/************************************************************************
*bloom��������ʵ�֣�Ϊ�˼ӿ��sstable�Ҷ���Ƶ�
************************************************************************/
#include <stdint.h>

struct __wt_bloom
{
	const char*			uri;
	char*				config;			/*bloom�������ַ���*/
	uint8_t*			bitstring;		/*bloom bit map*/
	WT_SESSION_IMPL*	session;
	WT_CURSOR*			c;				/**/
	
	uint32_t			k;				/*hash��λ�Ĵ���*/
	uint32_t			factor;			/*ÿ��item(������Ϊ�ֽ�)ռ�õ�bit��*/
	uint64_t			m;				/*bloom slots�ܵ�bit��*/
	uint64_t			n;				/*bloom slots�ܵ�item��*/
};

struct __wt_bloom_hash
{
	uint64_t h1;
	uint64_t h2;
};

