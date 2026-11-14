/* Uploads to the server.
 *
 *   POST /ingest/v1/heartbeat: who is playing, once a minute and on every change;
 *   POST /ingest/v1/sessions:  a batch of closed sessions from the queue;
 *   POST /ingest/v1/trophies:  one trophy queue line per request.
 *
 * Only the sessions response is read, for the "recorded" notification
 * (reply.h). GET /sync and watermark backfill are not used: the queues on
 * disk are the source of truth and the server is idempotent.
 *
 * Status codes: 200 removes the lines from the queue; 400 and 413 remove
 * them too, since the body will not get better and retrying forever would
 * jam the queue; 401, 429, 5xx and network errors keep them for the next
 * attempt.
 */

#ifndef GWM_UPLOAD_H
#define GWM_UPLOAD_H

#include <stdint.h>
#include <string.h>

#include "clock.h"
#include "config.h"
#include "console.h"
#include "http.h"
#include "isotime.h"
#include "log.h"
#include "notif.h"
#include "queue.h"
#include "record.h"
#include "reply.h"
#include "session.h"

#define GWM_PATH_HEARTBEAT "/ingest/v1/heartbeat"
#define GWM_PATH_SESSIONS  "/ingest/v1/sessions"
#define GWM_PATH_TROPHIES  "/ingest/v1/trophies"

#define GWM_SESSION_QUEUE  GWM_DATA_DIR "/sessions.jsonl"

/* A session batch. The server's limits (500 rows, 1 MB) are far higher. */
#define GWM_BATCH_READ (32 * 1024)

/* Trophies per pass: the first run may find dozens, and the poll loop is
 * single-threaded. The rest go in later passes. */
#define GWM_TROPHIES_PER_PASS 10

static char gwm_batch_lines[GWM_BATCH_READ];
static char gwm_batch_body[GWM_BATCH_READ + 64];
static char gwm_batch_reply[6144];

static int gwm_code_consumes(int code)
{
	return code == 200 || code == 400 || code == 413;
}

/* Shifts the timestamps in a chunk of the queue by the clock offset. The
 * queue holds raw console time: offline there is no offset, it arrives with
 * the Date header. Timestamps have a fixed width, so the edit is in place,
 * without parsing JSON. */
static void gwm_batch_fix_times(char *buf, size_t len)
{
	static const char *keys[] = { "\"startedAt\":\"", "\"endedAt\":\"", "\"earnedAt\":\"" };

	if(!gwm_clock_offset) return;

	for(size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
	{
		size_t kl = strlen(keys[k]);
		char *p = buf;

		while((p = strstr(p, keys[k])) != NULL && (size_t)(p - buf) + kl + GWM_ISO_LEN - 1 <= len)
		{
			gwm_iso_shift(p + kl, gwm_clock_offset);
			p += kl;
		}
	}
}

static int gwm_send_heartbeat(const gwm_config_t *cfg, const gwm_session_t *s)
{
	char body[GWM_RECORD_MAX], sent[GWM_ISO_LEN], started[GWM_ISO_LEN];

	gwm_iso_from_unix(sent, gwm_now());
	gwm_iso_from_unix(started, s->start + gwm_clock_offset);

	size_t len = gwm_build_heartbeat(body, sizeof(body), sent, gwm_console_name(), s->active,
	                                 s->account, s->title_id,
	                                 s->title[0] ? s->title : s->title_id, started);

	return gwm_http_post(cfg, GWM_PATH_HEARTBEAT, body, len);
}

/* Returns the HTTP status, GWM_HTTP_ERR_*, or 0 if there was nothing to send. */
static int gwm_upload_sessions(const gwm_config_t *cfg)
{
	size_t len = gwm_queue_read(GWM_SESSION_QUEUE, gwm_batch_lines, sizeof(gwm_batch_lines));
	if(!len) return 0;

	gwm_batch_fix_times(gwm_batch_lines, len);

	/* Lines are ready objects; join them into {"sessions":[...]}. */
	char *p = gwm_batch_body, *end = gwm_batch_body + sizeof(gwm_batch_body);
	int count = 0;

	p = gwm_lit(p, end, "{\"sessions\":[");
	for(char *line = gwm_batch_lines; *line;)
	{
		char *nl = strchr(line, '\n');
		if(!nl) break;
		*nl = 0;
		if(nl > line)
		{
			if(count++) p = gwm_lit(p, end, ",");
			p = gwm_lit(p, end, line);
		}
		line = nl + 1;
	}
	p = gwm_lit(p, end, "]}");

	if(!count) { gwm_queue_consume(GWM_SESSION_QUEUE, len); return 0; }

	int code = gwm_http_request(cfg, GWM_PATH_SESSIONS, gwm_batch_body, (size_t)(p - gwm_batch_body),
	                            cfg->token, gwm_batch_reply, sizeof(gwm_batch_reply));
	gwm_log("sessions: %d -> %d", count, code);

	if(gwm_code_consumes(code)) gwm_queue_consume(GWM_SESSION_QUEUE, len);

	/* "7 min recorded · 41 min total": what the server accepted and counted. */
	if(code == 200)
	{
		char texts[GWM_REPLY_MAX_EACH][256];
		int n = gwm_reply_messages(gwm_batch_reply, gwm_now(), texts, GWM_REPLY_MAX_EACH);
		for(int i = 0; i < n; i++) gwm_toast("%s", texts[i]);
	}
	return code;
}

/* Each trophy queue line is a ready request body. */
static int gwm_upload_trophies(const gwm_config_t *cfg)
{
	int code = 0;

	for(int i = 0; i < GWM_TROPHIES_PER_PASS; i++)
	{
		size_t len = gwm_queue_read(GWM_TROPHY_QUEUE, gwm_batch_lines, sizeof(gwm_batch_lines));
		if(!len) break;

		char *nl = strchr(gwm_batch_lines, '\n');
		size_t line_len = (size_t)(nl - gwm_batch_lines) + 1;

		gwm_batch_fix_times(gwm_batch_lines, line_len);
		code = gwm_http_post(cfg, GWM_PATH_TROPHIES, gwm_batch_lines, line_len - 1);
		gwm_log("trophies: %.*s -> %d", 60, gwm_batch_lines, code);

		if(!gwm_code_consumes(code)) break;
		gwm_queue_consume(GWM_TROPHY_QUEUE, line_len);
	}

	return code;
}

#endif /* GWM_UPLOAD_H */
