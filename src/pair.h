/* Pairing the console with an on-screen code.
 *
 * There is no good way to type a token on a PS5: no keyboard, no SD card,
 * and FTP is too much to ask. So without a token the payload asks the server
 * for a code (POST /ingest/v1/pair), shows it in a notification ("enter code
 * 123456 to link your account") and every few seconds asks whether it was
 * entered (POST /ingest/v1/pair/poll). Once the code is entered in the
 * "PS5 (CFW)" card on the site, the next poll brings the key; the payload
 * writes it to config.cfg and carries on as usual.
 *
 * A PS5 notification fades in a few seconds and its duration cannot be set,
 * so while the code is valid it is repeated every 10 seconds. A code lives
 * 10 minutes; when it expires the payload takes a new one.
 *
 * Response parsing is pure, covered by a host test.
 */

#ifndef GWM_PAIR_H
#define GWM_PAIR_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "record.h"

#define GWM_PATH_PAIR      "/ingest/v1/pair"
#define GWM_PATH_PAIR_POLL "/ingest/v1/pair/poll"

#define GWM_PAIR_TOAST_EVERY_SEC 10
#define GWM_PAIR_RETRY_SEC       60

typedef struct
{
	int     active;
	char    code[8];
	char    secret[96];
	int     interval;
	int64_t next_poll;
	int64_t next_toast;
	int64_t next_start;
} gwm_pair_t;

/* {"code":"123456","secret":"…","expiresAt":"…","interval":5} */
static int gwm_pair_parse_start(const char *body, gwm_pair_t *p)
{
	int64_t interval = 0;

	if(!gwm_json_get_str(body, "code", p->code, sizeof(p->code)) || strlen(p->code) != 6) return 0;
	if(!gwm_json_get_str(body, "secret", p->secret, sizeof(p->secret)) || !p->secret[0]) return 0;

	p->interval = (gwm_json_get_i64(body, "interval", &interval) && interval >= 2 && interval <= 60)
		? (int)interval : 5;
	return 1;
}

/* Poll response: 1 paired, 0 still waiting, -1 unexpected.
 *
 * The device key is the secret received with the code: the server stores its
 * hash when the code is entered and issues no tokens. An early server version
 * still sent a separate token; if one is present it is used, otherwise the
 * secret. */
static int gwm_pair_parse_poll(const char *body, const char *secret, char *token, size_t size)
{
	char status[16];

	if(!gwm_json_get_str(body, "status", status, sizeof(status))) return -1;
	if(!strcmp(status, "pending")) return 0;
	if(strcmp(status, "paired")) return -1;

	if(!gwm_json_get_str(body, "token", token, size) || !token[0])
		snprintf(token, size, "%s", secret);
	return token[0] ? 1 : -1;
}

static size_t gwm_pair_start_body(char *out, size_t size, const char *profile, const char *device)
{
	char *p = out, *end = out + size;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "platform", GWM_PLATFORM);
	if(device && device[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "device", device);
	}
	if(profile && profile[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "profile", profile);
	}
	p = gwm_lit(p, end, "}");
	return (size_t)(p - out);
}

#ifndef GWM_HOST_TEST

#include "clock.h"
#include "config.h"
#include "console.h"
#include "http.h"
#include "log.h"
#include "version.h"

static void gwm_pair_toast(const gwm_pair_t *p)
{
	gwm_toast(GWM_PRODUCT ": enter code %s to link your account", p->code);
}

/* One pairing step, called every second while there is no token. Returns 1
 * once the key is received and written to the config. */
static int gwm_pair_tick(gwm_pair_t *p, const gwm_config_t *cfg, const char *profile)
{
	char body[256], resp[1024];
	int64_t now = gwm_now_raw();

	if(!p->active)
	{
		if(now < p->next_start) return 0;

		size_t len = gwm_pair_start_body(body, sizeof(body), profile, gwm_console_name());
		int code = gwm_http_request(cfg, GWM_PATH_PAIR, body, len, NULL, resp, sizeof(resp));

		if(code != 200 || !gwm_pair_parse_start(resp, p))
		{
			gwm_log("pairing: no code from the server (%d), retry in %d s", code, GWM_PAIR_RETRY_SEC);
			p->next_start = now + GWM_PAIR_RETRY_SEC;
			return 0;
		}

		gwm_log("pairing: code %s", p->code);
		p->active = 1;
		p->next_poll = now + p->interval;
		p->next_toast = now;
	}

	if(now >= p->next_toast)
	{
		gwm_pair_toast(p);
		p->next_toast = now + GWM_PAIR_TOAST_EVERY_SEC;
	}

	if(now < p->next_poll) return 0;
	p->next_poll = now + p->interval;

	int n = snprintf(body, sizeof(body), "{\"secret\":\"%s\"}", p->secret);
	int code = gwm_http_request(cfg, GWM_PATH_PAIR_POLL, body, (size_t)n, NULL, resp, sizeof(resp));

	/* The code expired or the server forgot it: start over with a new one. */
	if(code == 404 || code == 410)
	{
		gwm_log("pairing: code %s no longer valid (%d), taking a new one", p->code, code);
		p->active = 0;
		p->next_start = now;
		return 0;
	}

	if(code != 200)
	{
		/* Network or server trouble: the code is still valid, ask again later. */
		p->next_poll = now + 30;
		return 0;
	}

	char token[GWM_TOKEN_MAX];
	int state = gwm_pair_parse_poll(resp, p->secret, token, sizeof(token));
	if(state <= 0) return 0;

	if(gwm_config_save_token(GWM_CONFIG_FILE, token) != 0)
	{
		gwm_log("pairing: done, but the key could not be written to " GWM_CONFIG_FILE);
		return 0;
	}

	gwm_log("pairing: done, key saved");
	p->active = 0;
	return 1;
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_PAIR_H */
