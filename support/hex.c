
#include "wt_internal.h"

static const u_char hex[] = "0123456789abcdef";

/*��srcת���ɿɼ���16���ƴ�*/
static inline void __fill_hex(const uint8_t* src, size_t src_max, uint8_t* dest, size_t dest_max, size_t* lenp)
{
	uint8_t* dest_orig;
	
	dest_orig = dest;
	if(dest_max > 0)
		-- dest_max;
	for(; src_max > 0 && dest_max > 1; src_max -= 1, dest_max -= 2, ++src){
		*dest++ = hex[(*src & 0xf0) >> 4];
		*dest++ = hex[*src & 0x0f];
	}

	*dest++ = '\0';
	if(lenp != NULL)
		*lenp = WT_PTRDIFF(dest, dest_orig);
}

/*������������ת����16�����ַ����������������to��*/
int __wt_raw_to_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
{
	size_t len;

	len = size * 2 + 1;
	WT_RET(__wt_buf_init(session, to, len));
	__fill_hex(from, size, to->mem, len, &to->size);

	return 0;
}

/*�������ƴ�ת����16���ƴ���ÿ���ֽ�������"\"����*/
int __wt_raw_to_esc_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
{
	size_t i;
	const uint8_t* p;
	u_char*	t;

	WT_RET(__wt_buf_init(session, to, size * 3 + 1));

	for(p = from, to = to->mem, i = size; i > 0; --i, ++p){
		if (isprint((int)*p)) {
			if (*p == '\\')
				*t++ = '\\';
			*t++ = *p;
		} 
		else {
			*t++ = '\\';
			*t++ = hex[(*p & 0xf0) >> 4];
			*t++ = hex[*p & 0x0f];
		}
	}

	*t++ = '\0';
	to->size = WT_PTRDIFF(t, to->mem);
	return 0;
}

/*16�����ַ�����ת��һ����Ӧ�Ķ�������*/
int __wt_hex2byte(const u_char *from, u_char *to)
{
	uint8_t byte;
	/*��λת��*/
	switch (from[0]) {
	case '0': byte = 0; break;
	case '1': byte = 1 << 4; break;
	case '2': byte = 2 << 4; break;
	case '3': byte = 3 << 4; break;
	case '4': byte = 4 << 4; break;
	case '5': byte = 5 << 4; break;
	case '6': byte = 6 << 4; break;
	case '7': byte = 7 << 4; break;
	case '8': byte = 8 << 4; break;
	case '9': byte = 9 << 4; break;
	case 'a': byte = 10 << 4; break;
	case 'b': byte = 11 << 4; break;
	case 'c': byte = 12 << 4; break;
	case 'd': byte = 13 << 4; break;
	case 'e': byte = 14 << 4; break;
	case 'f': byte = 15 << 4; break;
	default:
		return (1);
	}
	/*��λת��*/
	switch (from[1]) {
	case '0': break;
	case '1': byte |= 1; break;
	case '2': byte |= 2; break;
	case '3': byte |= 3; break;
	case '4': byte |= 4; break;
	case '5': byte |= 5; break;
	case '6': byte |= 6; break;
	case '7': byte |= 7; break;
	case '8': byte |= 8; break;
	case '9': byte |= 9; break;
	case 'a': byte |= 10; break;
	case 'b': byte |= 11; break;
	case 'c': byte |= 12; break;
	case 'd': byte |= 13; break;
	case 'e': byte |= 14; break;
	case 'f': byte |= 15; break;
	default:
		return (1);
	}
	*to = byte;

	return 0;
}

static int __hex_fmterr(WT_SESSION_IMPL *session)
{
	WT_RET_MSG(session, EINVAL, "Invalid format in hexadecimal string");
}

int __wt_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
{
	return (__wt_nhex_to_raw(session, from, strlen(from), to));
}

/*��16����������ת���ɶ����ƴ������Ҵ���item��*/
int __wt_nhex_to_raw(WT_SESSION_IMPL *session, const char *from, size_t size, WT_ITEM *to)
{
	const u_char *p;
	u_char *t;

	if (size % 2 != 0)
		return (__hex_fmterr(session));

	WT_RET(__wt_buf_init(session, to, size / 2));

	for (p = (u_char *)from, t = to->mem; size > 0; p += 2, size -= 2, ++t){
		if (__wt_hex2byte(p, t))
			return (__hex_fmterr(session));
	}

	to->size = WT_PTRDIFF(t, to->mem);
	return 0;
}

/*����'\'��ʽ����16���ƴ�ת���ɶ����ƴ������Ҵ���item��*/
int __wt_esc_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
{
	const u_char *p;
	u_char *t;

	WT_RET(__wt_buf_init(session, to, strlen(from)));

	for (p = (u_char *)from, t = to->mem; *p != '\0'; ++p, ++t) {
		if ((*t = *p) != '\\')
			continue;
		++p;
		if (p[0] != '\\') {
			if (p[0] == '\0' || p[1] == '\0' || __wt_hex2byte(p, t))
				return (__hex_fmterr(session));
			++p;
		}
	}
	to->size = WT_PTRDIFF(t, to->mem);
	return (0);
}

