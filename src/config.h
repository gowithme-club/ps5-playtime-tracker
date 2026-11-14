/* Payload config: /data/gowithme.club/config.cfg
 *
 *   token   = ...
 *   account = Player      # name on the server; defaults to the console profile name
 *   popups  = true        # on-screen notifications; false runs silently
 *   host    = gowithme.club
 *   port    = 80
 *
 * Without a token the payload sends nothing but pairing requests and keeps
 * sessions and trophies queued until it has one.
 *
 * Line parsing is separate from file I/O and covered by a host test.
 */

#ifndef GWM_CONFIG_H
#define GWM_CONFIG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define GWM_DATA_DIR    "/data/gowithme.club"
#define GWM_CONFIG_FILE GWM_DATA_DIR "/config.cfg"

#define GWM_TOKEN_MAX   128
#define GWM_ACCOUNT_MAX 64
#define GWM_HOST_MAX    96

typedef struct
{
	char     token[GWM_TOKEN_MAX];
	char     account[GWM_ACCOUNT_MAX];
	char     host[GWM_HOST_MAX];
	uint16_t port;
	/* Whether to show notifications. On by default: whoever launched the
	 * payload should see it is alive without reading the log. */
	int      popups;
	int      valid;
} gwm_config_t;

static void gwm_cfg_defaults(gwm_config_t *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", "gowithme.club");
	c->port = 80;
	c->popups = 1;
}

/* Trims spaces and quotes around a value, in place. */
static void gwm_trim(char *s)
{
	char *b = s;
	while(*b == ' ' || *b == '\t' || *b == '"') b++;

	char *e = b + strlen(b);
	while(e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
	                e[-1] == '\n' || e[-1] == '"')) e--;
	*e = 0;

	if(b != s) memmove(s, b, strlen(b) + 1);
}

/* true/false, yes/no, on/off, 1/0. Anything else keeps the current value:
 * a typo must not silently turn something off. */
static int gwm_cfg_bool(const char *val, int fallback)
{
	if(!strcasecmp(val, "true") || !strcasecmp(val, "yes") ||
	   !strcasecmp(val, "on") || !strcmp(val, "1")) return 1;

	if(!strcasecmp(val, "false") || !strcasecmp(val, "no") ||
	   !strcasecmp(val, "off") || !strcmp(val, "0")) return 0;

	return fallback;
}

/* One "key = value" line. Lines starting with # or ; are comments. */
static void gwm_cfg_line(gwm_config_t *c, char *line)
{
	while(*line == ' ' || *line == '\t') line++;
	if(!*line || *line == '#' || *line == ';') return;

	char *eq = strchr(line, '=');
	if(!eq) return;

	*eq = 0;
	char *key = line, *val = eq + 1;

	gwm_trim(key);
	gwm_trim(val);

	if(!strcmp(key, "token"))        snprintf(c->token, sizeof(c->token), "%s", val);
	else if(!strcmp(key, "account")) snprintf(c->account, sizeof(c->account), "%s", val);
	else if(!strcmp(key, "host"))    snprintf(c->host, sizeof(c->host), "%s", val);
	else if(!strcmp(key, "popups"))  c->popups = gwm_cfg_bool(val, c->popups);
	else if(!strcmp(key, "port"))
	{
		unsigned long p = 0;
		for(const char *q = val; *q >= '0' && *q <= '9' && p < 65536; q++)
			p = p * 10 + (unsigned long)(*q - '0');
		if(p > 0 && p < 65536) c->port = (uint16_t)p;
	}
}

/* Parses the whole config text. The text is modified (lines are cut in place). */
static void gwm_cfg_parse(gwm_config_t *c, char *text)
{
	gwm_cfg_defaults(c);

	char *line = text;
	while(line && *line)
	{
		char *nl = strchr(line, '\n');
		if(nl) *nl = 0;
		gwm_cfg_line(c, line);
		line = nl ? nl + 1 : NULL;
	}

	c->valid = (c->token[0] != 0);
}

/* Config text with a new token: the "token = ..." line goes first, old token
 * lines are dropped and everything else (account, host, comments) is kept,
 * so pairing (pair.h) never overwrites what was written by hand. Returns the
 * length, or 0 if it does not fit. */
static size_t gwm_cfg_with_token(const char *in, const char *token, char *out, size_t size)
{
	int n = snprintf(out, size, "token = %s\n", token);
	if(n < 0 || (size_t)n >= size) return 0;
	size_t len = (size_t)n;

	const char *line = in;
	while(line && *line)
	{
		const char *nl = strchr(line, '\n');
		size_t ll = nl ? (size_t)(nl - line) + 1 : strlen(line);

		const char *k = line;
		while(*k == ' ' || *k == '\t') k++;
		int is_token = !strncmp(k, "token", 5) && (k[5] == ' ' || k[5] == '\t' || k[5] == '=');

		if(!is_token)
		{
			if(len + ll + 1 >= size) return 0;
			memcpy(out + len, line, ll);
			len += ll;
			if(!nl) out[len++] = '\n';
		}
		line = nl ? nl + 1 : NULL;
	}

	out[len] = 0;
	return len;
}

static int gwm_config_load(gwm_config_t *c, const char *path)
{
	char buf[1024];
	size_t got = 0;
	FILE *f = fopen(path, "r");

	if(f)
	{
		got = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	buf[got] = 0;

	gwm_cfg_parse(c, buf);
	return c->valid;
}

/* Writes the token to the config through a temp file and rename. */
static int gwm_config_save_token(const char *path, const char *token)
{
	char in[1024], out[1280], tmp[256];
	size_t got = 0;
	FILE *f = fopen(path, "r");

	if(f)
	{
		got = fread(in, 1, sizeof(in) - 1, f);
		fclose(f);
	}
	in[got] = 0;

	size_t len = gwm_cfg_with_token(in, token, out, sizeof(out));
	if(!len) return -1;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if(!f) return -1;
	int failed = fwrite(out, 1, len, f) != len;
	if(fclose(f) != 0) failed = 1;
	if(failed) { remove(tmp); return -1; }
	return rename(tmp, path);
}

#endif /* GWM_CONFIG_H */
