/* Pairing the console with an on-screen code.
 *
 * There is no good way to type a token on a PS5: no keyboard, no SD card,
 * and FTP is too much to ask. So without a token the payload asks the server
 * for a code (POST /ingest/v1/pair), shows it in a notification ("Code
 * 123 456" under "Enter it at gowithme.club/ps5") and every few seconds asks
 * whether it was entered (POST /ingest/v1/pair/poll). The notification names
 * the page because it is all a person sees: someone who only downloaded the
 * file has to learn from it where the code goes. Once the code is entered on the
 * site, the next poll brings the key; the payload writes it to config.cfg
 * and carries on as usual.
 *
 * The tile in Media (tile.h, web.h) shows the code with a QR code and
 * follows a new one by itself, so notifications only send people there:
 * twice, when the first code arrives and two minutes later. A PS5
 * notification fades in a few seconds and its duration cannot be set; with
 * the tile deleted the code itself is the notification, repeated every 10
 * seconds while it is valid. A code lives 10 minutes; when it expires the
 * payload takes a new one.
 *
 * New codes are taken for 20 minutes after the first one: two codes, enough
 * to sign up on the site. Then the payload says once that the console is not
 * linked and shows no more codes until "Show code" on the tile (web.h) or
 * the payload is loaded again. A console nobody
 * links, one in a loader's autoload say, must not repeat a code through every
 * game. Sessions and trophies keep queueing meanwhile.
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

/* The page that explains the tracker and takes the code. */
#define GWM_PAIR_URL "gowithme.club/ps5"

#define GWM_PAIR_TOAST_EVERY_SEC 10
/* Reminders to open the tile: how many, and how far apart. */
#define GWM_PAIR_NUDGES          2
#define GWM_PAIR_NUDGE_AFTER_SEC 120
#define GWM_PAIR_RETRY_SEC       60
#define GWM_PAIR_WINDOW_SEC      (20 * 60)

typedef struct
{
	int     active;
	char    code[8];
	char    secret[96];
	int     interval;
	int64_t next_poll;
	int64_t next_toast;
	int64_t next_start;
	/* No new codes after this; 0 until the first code arrives. */
	int64_t window_end;
	/* No more pairing in this run. */
	int     done;
	/* Reminders to open the tile sent in this run. */
	int     nudges;
} gwm_pair_t;

/* Whether to ask for a new code now: 1 yes, 0 not yet, -1 never again in
 * this run. The window starts with the first code, not with the payload: a
 * console that comes online later still gets its full 20 minutes. */
static int gwm_pair_may_start(const gwm_pair_t *p, int64_t now)
{
	if(p->done || (p->window_end && now >= p->window_end)) return -1;
	return now >= p->next_start;
}

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

static size_t gwm_pair_start_body(char *out, size_t size, const char *profile, const char *device,
                                  const char *console_id)
{
	char *p = out, *end = out + size;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "platform", GWM_PLATFORM);
	if(device && device[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "device", device);
	}
	if(console_id && console_id[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "consoleId", console_id);
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
	gwm_toast(GWM_TR("Code %.3s %s\nEnter it at " GWM_PAIR_URL, "Код %.3s %s\nВведите его на " GWM_PAIR_URL),
	          p->code, p->code + 3);
}

/* One pairing step, called every second while there is no token. tile says
 * whether the tile is on the home screen to send people to. Returns 1 once
 * the key is received and written to the config. */
static int gwm_pair_tick(gwm_pair_t *p, const gwm_config_t *cfg, const char *profile, int tile)
{
	char body[512], resp[1024];
	int64_t now = gwm_now_raw();

	if(p->done) return 0;

	if(!p->active)
	{
		int may = gwm_pair_may_start(p, now);
		if(may < 0)
		{
			gwm_log("pairing: no code entered in %d min, stopping until the next start", GWM_PAIR_WINDOW_SEC / 60);
			p->done = 1;
			if(tile) gwm_toast("%s", GWM_TR("Not linked\nOpen " GWM_PRODUCT " for a code",
			                                "Консоль не привязана\nОткройте " GWM_PRODUCT ", чтобы получить код"));
			else gwm_toast("%s", GWM_TR("Not linked\nLoad the payload again for a code",
			                            "Консоль не привязана\nЗапустите payload ещё раз, чтобы получить код"));
			return 0;
		}
		if(!may) return 0;

		size_t len = gwm_pair_start_body(body, sizeof(body), profile, gwm_console_name(), gwm_console_id());
		int code = gwm_http_request(cfg, GWM_PATH_PAIR, body, len, NULL, resp, sizeof(resp));

		if(code != 200 || !gwm_pair_parse_start(resp, p))
		{
			gwm_log("pairing: no code from the server (%d), retry in %d s", code, GWM_PAIR_RETRY_SEC);
			p->next_start = now + GWM_PAIR_RETRY_SEC;
			return 0;
		}

		gwm_log("pairing: code %s", p->code);
		if(!p->window_end) p->window_end = now + GWM_PAIR_WINDOW_SEC;
		p->active = 1;
		p->next_poll = now + p->interval;
		p->next_toast = now;
	}

	if(now >= p->next_toast)
	{
		if(!tile)
		{
			gwm_pair_toast(p);
			p->next_toast = now + GWM_PAIR_TOAST_EVERY_SEC;
		}
		else if(p->nudges < GWM_PAIR_NUDGES)
		{
			gwm_toast("%s", GWM_TR("Link this PS5\nOpen " GWM_PRODUCT, "Привяжите эту PS5\nОткройте " GWM_PRODUCT));
			p->nudges++;
			p->next_toast = now + GWM_PAIR_NUDGE_AFTER_SEC;
		}
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
