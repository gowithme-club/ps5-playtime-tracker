/* Request bodies for the ingest API:
 *
 *   - a closed session object: these lines wait in the queue and are joined
 *     into a POST /ingest/v1/sessions batch;
 *   - the heartbeat body;
 *   - a POST /ingest/v1/trophies body with a single trophy.
 *
 * Trophies: the API asks for the full state of a set, but the server diffs
 * against what it knows and never rolls anything back when fewer trophies
 * arrive. So each trophy notification goes out as its own body with one
 * trophy: the payload has no full state for PS4 sets (trophy.img is
 * encrypted), and the server merges what it gets.
 *
 * Pure string code, covered by a host test.
 */

#ifndef GWM_RECORD_H
#define GWM_RECORD_H

#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "json.h"

/* One queue line, with room for long titles and trophy descriptions. */
#define GWM_RECORD_MAX 2048

#define GWM_TITLE_ID_LEN 16
#define GWM_TITLE_LEN    160

/* The server's platform key. One key for everything from this console, PS5
 * games (PPSA...) and PS4 games (CUSA...) alike: the server tells them apart
 * by title id, and the key says where the data came from. Not "ps5": that is
 * the server's official PSN integration, and the two must stay separate. */
#define GWM_PLATFORM "ps5-cfw"

/* Four capital letters and five digits: CUSA03980, PPSA01234, NPXS40082. */
static int gwm_title_id_valid(const char *s)
{
	for(int i = 0; i < 4; i++) if(!isupper((unsigned char)s[i])) return 0;
	for(int i = 4; i < 9; i++) if(!isdigit((unsigned char)s[i])) return 0;
	return 1;
}

static const char *gwm_platform_for(const char *title_id)
{
	(void)title_id;
	return GWM_PLATFORM;
}

static size_t gwm_build_session(char *out, size_t out_size,
                                const char *account,
                                const char *title_id, const char *title,
                                const char *started_at, const char *ended_at,
                                int64_t seconds)
{
	char *p = out;
	const char *end = out + out_size;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "account", account);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "titleId", title_id);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "title", title);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "platform", gwm_platform_for(title_id));
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "startedAt", started_at);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "endedAt", ended_at);
	p = gwm_lit(p, end, ",\"seconds\":");
	p = gwm_i64(p, end, seconds);
	p = gwm_lit(p, end, "}");

	return (size_t)(p - out);
}

/* An empty current means the console is on but nothing is being played.
 * device_name is the console name from its settings (console.h); an empty
 * one is not sent. */
static size_t gwm_build_heartbeat(char *out, size_t out_size,
                                  const char *sent_at, const char *device_name, int playing,
                                  const char *account,
                                  const char *title_id, const char *title,
                                  const char *started_at)
{
	char *p = out;
	const char *end = out + out_size;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "sentAt", sent_at);
	if(device_name && device_name[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "deviceName", device_name);
	}
	p = gwm_lit(p, end, ",\"current\":[");

	if(playing)
	{
		p = gwm_lit(p, end, "{");
		p = gwm_json_kv(p, end, "account", account);
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "titleId", title_id);
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "titleName", title);
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "platform", gwm_platform_for(title_id));
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "startedAt", started_at);
		p = gwm_lit(p, end, "}");
	}

	p = gwm_lit(p, end, "]}");
	return (size_t)(p - out);
}

/* A trophy as read from a notification (trophy.h) or a trophy file (trophy2.h). */
typedef struct
{
	char    set_id[24];       /* NPWR10824_00 */
	int64_t id;               /* number within the set */
	char    grade[12];        /* bronze|silver|gold|platinum */
	char    name[192];
	char    detail[512];
	char    set_name[GWM_TITLE_LEN];
	int     legacy;           /* trophy of a PS4 game */
	int     hidden;           /* hidden until earned (trophy2.h) */
} gwm_trophy_t;

static size_t gwm_build_trophy(char *out, size_t out_size,
                               const char *account, const gwm_trophy_t *t,
                               const char *earned_at)
{
	char *p = out;
	const char *end = out + out_size;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "account", account);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "platform", GWM_PLATFORM);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "setId", t->set_id);

	if(t->set_name[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "title", t->set_name);
	}

	p = gwm_lit(p, end, ",\"trophies\":[{\"id\":");
	p = gwm_i64(p, end, t->id);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "earnedAt", earned_at);

	/* Optional fields: the server uses them until it has PSN data. */
	if(t->name[0])   { p = gwm_lit(p, end, ","); p = gwm_json_kv(p, end, "name", t->name); }
	if(t->detail[0]) { p = gwm_lit(p, end, ","); p = gwm_json_kv(p, end, "detail", t->detail); }
	if(t->grade[0])  { p = gwm_lit(p, end, ","); p = gwm_json_kv(p, end, "grade", t->grade); }

	p = gwm_lit(p, end, "}]}");
	return (size_t)(p - out);
}

#endif /* GWM_RECORD_H */
