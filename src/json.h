/* JSON: building request bodies and reading fields from foreign JSON.
 *
 * Building appends to a bounded buffer, with no allocations. Reading is for the raw_data of trophy notifications: one flat
 * set of keys, each key we read (trophyId, trophyGrade and so on) appearing
 * once. So no full parser is needed, just a search for "key": and parsing
 * the value after it.
 *
 * Pure string code, covered by tests/json_test.c.
 */

#ifndef GWM_JSON_H
#define GWM_JSON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- build */

static char *gwm_lit(char *dst, const char *end, const char *src)
{
	while(*src && dst < end - 1) *dst++ = *src++;
	*dst = 0;
	return dst;
}

static char *gwm_i64(char *dst, const char *end, int64_t v)
{
	char tmp[24];
	int n = 0;
	uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;

	do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while(u);
	if(v < 0) tmp[n++] = '-';

	while(n && dst < end - 1) *dst++ = tmp[--n];
	*dst = 0;
	return dst;
}

/* JSON escaping. Game and trophy names come from system databases and may
 * contain quotes; UTF-8 passes through as is. */
static char *gwm_json_str(char *dst, const char *end, const char *src)
{
	static const char hex[] = "0123456789abcdef";

	while(*src && dst < end - 7)
	{
		unsigned char c = (unsigned char)*src++;

		if(c == '"' || c == '\\')  { *dst++ = '\\'; *dst++ = (char)c; }
		else if(c == '\n')         { *dst++ = '\\'; *dst++ = 'n'; }
		else if(c == '\r')         { *dst++ = '\\'; *dst++ = 'r'; }
		else if(c == '\t')         { *dst++ = '\\'; *dst++ = 't'; }
		else if(c < 0x20)
		{
			*dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
			*dst++ = hex[(c >> 4) & 0xF]; *dst++ = hex[c & 0xF];
		}
		else *dst++ = (char)c;
	}
	*dst = 0;
	return dst;
}

/* A "key":"value" pair with the value escaped. */
static char *gwm_json_kv(char *dst, const char *end, const char *key, const char *val)
{
	dst = gwm_lit(dst, end, "\"");
	dst = gwm_lit(dst, end, key);
	dst = gwm_lit(dst, end, "\":\"");
	dst = gwm_json_str(dst, end, val);
	return gwm_lit(dst, end, "\"");
}

/* ----------------------------------------------------------------- read */

/* Pointer to the value after "key":, or NULL. Only a quoted string followed
 * by a colon counts, not any occurrence of the word: "trophyName" can also
 * appear as a value somewhere in the text. */
static const char *gwm_json_find(const char *json, const char *key)
{
	size_t kl = strlen(key);
	const char *p = json;

	while((p = strstr(p, key)) != NULL)
	{
		if(p > json && p[-1] == '"' && p[kl] == '"')
		{
			const char *v = p + kl + 1;
			while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
			if(*v == ':')
			{
				v++;
				while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
				return v;
			}
		}
		p += kl;
	}
	return NULL;
}

static int gwm_hex4(const char *s, unsigned *out)
{
	unsigned v = 0;
	for(int i = 0; i < 4; i++)
	{
		char c = s[i];
		v <<= 4;
		if(c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
		else if(c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if(c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0;
	}
	*out = v;
	return 1;
}

/* Code point to UTF-8. Returns the bytes written, or 0 if there is no room. */
static size_t gwm_utf8_put(char *dst, size_t room, unsigned cp)
{
	if(cp < 0x80 && room >= 1)    { dst[0] = (char)cp; return 1; }
	if(cp < 0x800 && room >= 2)
	{
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if(cp < 0x10000 && room >= 3)
	{
		dst[0] = (char)(0xE0 | (cp >> 12));
		dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if(cp >= 0x10000 && room >= 4)
	{
		dst[0] = (char)(0xF0 | (cp >> 18));
		dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

/* String value of a key, unescaped (including \uXXXX and surrogate pairs:
 * SQLite returns raw_data as is, and the system writes non-ASCII text both as
 * UTF-8 and as \u escapes). If it does not fit it is cut on a character
 * boundary. Returns 1 if the key is found and its value is a string. */
static int gwm_json_get_str(const char *json, const char *key, char *out, size_t size)
{
	const char *v = gwm_json_find(json, key);
	size_t n = 0;

	if(!size) return 0;
	out[0] = 0;
	if(!v || *v != '"') return 0;
	v++;

	while(*v && *v != '"')
	{
		char tmp[4];
		size_t put = 0;

		if(*v == '\\')
		{
			v++;
			switch(*v)
			{
				case 'n': tmp[0] = '\n'; put = 1; break;
				case 'r': tmp[0] = '\r'; put = 1; break;
				case 't': tmp[0] = '\t'; put = 1; break;
				case 'b': tmp[0] = '\b'; put = 1; break;
				case 'f': tmp[0] = '\f'; put = 1; break;
				case 'u':
				{
					unsigned cp = 0, lo = 0;
					if(!gwm_hex4(v + 1, &cp)) return 0;
					v += 4;
					if(cp >= 0xD800 && cp <= 0xDBFF && v[1] == '\\' && v[2] == 'u' &&
					   gwm_hex4(v + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
					{
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						v += 6;
					}
					put = gwm_utf8_put(tmp, sizeof(tmp), cp);
					break;
				}
				case 0: return 0;
				default: tmp[0] = *v; put = 1; break; /* \" \\ \/ */
			}
			v++;
		}
		else
		{
			tmp[0] = *v++;
			put = 1;
		}

		if(n + put >= size) break;
		memcpy(out + n, tmp, put);
		n += put;
	}

	/* A cut in the middle of a multibyte character drops the whole
	 * character, so the server never gets broken UTF-8. */
	if(*v != '"' && n)
	{
		size_t i = n - 1;
		while(i && ((unsigned char)out[i] & 0xC0) == 0x80) i--;

		unsigned char lead = (unsigned char)out[i];
		size_t need = (lead < 0x80) ? 1 : (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
		if(n - i < need) n = i;
	}

	out[n] = 0;
	return 1;
}

/* An integer, as a number (7) or a string ("7"): the system writes trophyId as a string. */
static int gwm_json_get_i64(const char *json, const char *key, int64_t *out)
{
	const char *v = gwm_json_find(json, key);
	int neg = 0, digits = 0;
	int64_t n = 0;

	if(!v) return 0;
	if(*v == '"') v++;
	if(*v == '-') { neg = 1; v++; }

	while(*v >= '0' && *v <= '9')
	{
		n = n * 10 + (*v++ - '0');
		digits++;
	}

	if(!digits) return 0;
	*out = neg ? -n : n;
	return 1;
}

static int gwm_json_get_bool(const char *json, const char *key, int *out)
{
	const char *v = gwm_json_find(json, key);

	if(!v) return 0;
	if(strncmp(v, "true", 4) == 0)  { *out = 1; return 1; }
	if(strncmp(v, "false", 5) == 0) { *out = 0; return 1; }
	return 0;
}

#endif /* GWM_JSON_H */
