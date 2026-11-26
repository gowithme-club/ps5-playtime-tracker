/* The "recorded" notification after sessions are uploaded.
 *
 * The server answers POST /ingest/v1/sessions with the rows it accepted:
 * title, length and totalSeconds, the player's all-time playtime for that
 * game across platforms. The payload shows it after the game is closed:
 *
 *   gowithme.club: Ace Combat 7: Skies Unknown
 *   7 min recorded · 41 min total
 *
 * or, on a console set to Russian (lang.h), «Записано 7 мин · всего 41 мин».
 *
 * Only fresh sessions are announced: a queue built up offline goes out in one
 * batch, and forty notifications about yesterday help no one. More than three
 * fresh sessions at once become a single summary line.
 *
 * Parsing is pure and covered by a host test.
 */

#ifndef GWM_REPLY_H
#define GWM_REPLY_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotime.h"
#include "json.h"
#include "lang.h"
#include "record.h"
#include "version.h"

/* Fresh: ended no more than an hour ago. */
#define GWM_REPLY_RECENT_SEC 3600
/* Up to this many notifications, one per session; more become one summary. */
#define GWM_REPLY_MAX_EACH 3

/* "< 1 min", "7 min", "2 h 5 min"; «7 мин», «2 ч 5 мин» in Russian. */
static void gwm_format_span(int64_t seconds, char *out, size_t size)
{
	const char *min = GWM_TR("min", "мин"), *h = GWM_TR("h", "ч");

	if(seconds < 60)
	{
		snprintf(out, size, "< 1 %s", min);
		return;
	}

	int64_t minutes = seconds / 60;
	if(minutes < 60) snprintf(out, size, "%lld %s", (long long)minutes, min);
	else             snprintf(out, size, "%lld %s %lld %s", (long long)(minutes / 60), h, (long long)(minutes % 60), min);
}

typedef struct
{
	char    title[GWM_TITLE_LEN];
	int64_t seconds;
	int64_t total;    /* 0: the server could not compute it */
	int     recent;
} gwm_reply_row_t;

/* The next {...} object of an array starting at *p. Session rows are flat,
 * but a title may contain braces and quotes, so JSON strings are skipped
 * properly. Returns the object's start and sets *end past it, or NULL when
 * the array is over. */
static const char *gwm_json_next_object(const char *p, const char **end)
{
	while(*p && *p != '{' && *p != ']') p++;
	if(*p != '{') return NULL;

	const char *start = p;
	int depth = 0, in_str = 0;

	for(; *p; p++)
	{
		if(in_str)
		{
			if(*p == '\\' && p[1]) p++;
			else if(*p == '"') in_str = 0;
			continue;
		}
		if(*p == '"') in_str = 1;
		else if(*p == '{') depth++;
		else if(*p == '}' && --depth == 0) { *end = p + 1; return start; }
	}
	return NULL;
}

/* Parses the server's answer. Returns the number of rows in rows (at most
 * max) and sets *accepted to the total the server accepted. */
static int gwm_reply_parse(const char *body, int64_t now, gwm_reply_row_t *rows, int max, int64_t *accepted)
{
	int64_t n = 0;
	int count = 0;

	*accepted = gwm_json_get_i64(body, "accepted", &n) ? n : 0;

	const char *p = gwm_json_find(body, "sessions");
	if(!p || *p != '[') return 0;
	p++;

	const char *end = NULL, *obj;
	while(count < max && (obj = gwm_json_next_object(p, &end)) != NULL)
	{
		char one[1024], ended[GWM_ISO_LEN + 4];
		size_t len = (size_t)(end - obj);
		if(len >= sizeof(one)) len = sizeof(one) - 1;
		memcpy(one, obj, len);
		one[len] = 0;
		p = end;

		gwm_reply_row_t *r = &rows[count];
		memset(r, 0, sizeof(*r));
		gwm_json_get_str(one, "titleName", r->title, sizeof(r->title));
		gwm_json_get_i64(one, "seconds", &r->seconds);
		gwm_json_get_i64(one, "totalSeconds", &r->total);

		/* The server sends endedAt with milliseconds; seconds are enough. */
		int64_t t;
		if(gwm_json_get_str(one, "endedAt", ended, sizeof(ended)) && strlen(ended) >= 19)
		{
			ended[19] = 'Z';
			ended[20] = 0;
			r->recent = gwm_iso_to_unix(ended, &t) && now - t <= GWM_REPLY_RECENT_SEC;
		}
		count++;
	}

	return count;
}

/* What this console has recorded, from the heartbeat answer's
 * "recorded":{"seconds":…,"games":…,"trophies":…}: the tile's page shows it.
 * -1 everywhere until a server sends it. */
typedef struct
{
	int64_t seconds, games, trophies;
} gwm_recorded_t;

/* 1 and the three numbers when the answer carries the object whole. */
static int gwm_reply_recorded(const char *body, gwm_recorded_t *out)
{
	const char *v = gwm_json_find(body, "recorded");
	const char *close = v && *v == '{' ? strchr(v, '}') : NULL;
	char obj[160];
	gwm_recorded_t r;

	if(!close || (size_t)(close - v + 1) >= sizeof(obj)) return 0;
	memcpy(obj, v, (size_t)(close - v + 1));
	obj[close - v + 1] = 0;

	if(!gwm_json_get_i64(obj, "seconds", &r.seconds) || !gwm_json_get_i64(obj, "games", &r.games)
		|| !gwm_json_get_i64(obj, "trophies", &r.trophies)) return 0;
	if(r.seconds < 0 || r.games < 0 || r.trophies < 0) return 0;
	*out = r;
	return 1;
}

/* Whose profile the tile shows, from the heartbeat answer. The server answers
 * for the profile open on the console (record.h): the page of the person who
 * owns it, or, when it is nobody's on the site while the console's owner has
 * one of their own there, that it can be claimed ("profileUnclaimed") and,
 * once the tile asked, the code that claims it ("profileCode"). What the
 * answer leaves out is cleared: a profile that changed hands must not keep
 * the last one's page or code. Only an https address on the site is taken. */
typedef struct
{
	char url[256];
	int  unclaimed;
	char code[8];
} gwm_profile_t;

static void gwm_reply_profile(const char *body, gwm_profile_t *out)
{
	char code[sizeof(out->code)];
	int unclaimed = 0;

	memset(out, 0, sizeof(*out));
	if(gwm_json_get_str(body, "profileUrl", out->url, sizeof(out->url)) && strncmp(out->url, "https://gowithme.club/", 22))
		out->url[0] = 0;
	if(gwm_json_get_bool(body, "profileUnclaimed", &unclaimed) && unclaimed)
	{
		out->unclaimed = 1;
		out->url[0] = 0;
		if(gwm_json_get_str(body, "profileCode", code, sizeof(code)) && strlen(code) == 6
			&& strspn(code, "0123456789") == 6)
			snprintf(out->code, sizeof(out->code), "%s", code);
	}
}

/* Notification texts. Returns how many; each goes to texts[i]. */
static int gwm_reply_messages(const char *body, int64_t now, char texts[][256], int max_texts)
{
	gwm_reply_row_t rows[8];
	int64_t accepted = 0;
	int count = gwm_reply_parse(body, now, rows, 8, &accepted), recent = 0, out = 0;
	int64_t sum = 0;
	char span[24], total[24];

	for(int i = 0; i < count; i++)
	{
		sum += rows[i].seconds;
		if(rows[i].recent) recent++;
	}
	if(!recent || max_texts < 1) return 0;

	/* Many at once: one line. The sum is shown only when the server listed
	 * every accepted row; otherwise it would be the sum of a part. */
	if(recent > GWM_REPLY_MAX_EACH || accepted > count)
	{
		gwm_format_span(sum, span, sizeof(span));
		if(accepted == count)
			snprintf(texts[0], 256, GWM_TR("%lld sessions recorded\n%s played", "Записано сессий: %lld\nСыграно %s"),
			         (long long)accepted, span);
		else
			snprintf(texts[0], 256, GWM_TR("%lld sessions recorded", "Записано сессий: %lld"), (long long)accepted);
		return 1;
	}

	for(int i = 0; i < count && out < max_texts; i++)
	{
		if(!rows[i].recent) continue;
		gwm_format_span(rows[i].seconds, span, sizeof(span));
		if(rows[i].total > 0)
		{
			gwm_format_span(rows[i].total, total, sizeof(total));
			snprintf(texts[out++], 256, GWM_TR("%.80s\n%s recorded · %s total", "%.80s\nЗаписано %s · всего %s"),
			         rows[i].title, span, total);
		}
		else
		{
			snprintf(texts[out++], 256, GWM_TR("%.80s\n%s recorded", "%.80s\nЗаписано %s"), rows[i].title, span);
		}
	}
	return out;
}

#endif /* GWM_REPLY_H */
