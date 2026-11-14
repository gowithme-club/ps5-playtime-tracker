/* The session state machine.
 *
 * Every poll compares what runs now with what ran before. Closing a session
 * yields a ready queue line, exactly as it goes into POST
 * /ingest/v1/sessions. The open session is not queued: the server sees it
 * through the heartbeat.
 *
 * Rest mode: the game stays in memory and the payload freezes with the rest
 * of the system. After waking, the poll sees the same game, and sleep would
 * count as play. So if more than GWM_SLEEP_GAP_SEC passed between polls, the
 * session closes at the last poll before the gap, and a new one opens if the
 * game is still running.
 *
 * Times are raw system time; the offset is added at send time.
 *
 * Pure logic (time is a parameter), covered by a host test.
 */

#ifndef GWM_SESSION_H
#define GWM_SESSION_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotime.h"
#include "record.h"

/* The threshold between sleep and a delay. Polls run every 10 seconds, but
 * the loop can stall on the network for several 10-second timeouts. */
#define GWM_SLEEP_GAP_SEC 120

/* Shorter sessions are a launch and quit, a failed load and the like. They
 * tell the server nothing. */
#define GWM_MIN_SESSION_SEC 10

#define GWM_ACCOUNT_LEN 64

typedef struct
{
	char    title_id[GWM_TITLE_ID_LEN];
	char    title[GWM_TITLE_LEN];
	/* The owner is fixed at the start: the profile can change without
	 * leaving the game, and the session must not close under another name. */
	char    account[GWM_ACCOUNT_LEN];
	int64_t start;
	int64_t last_seen;
	int     active;
} gwm_session_t;

static void gwm_session_init(gwm_session_t *s)
{
	memset(s, 0, sizeof(*s));
}

/* Closes the session at end. Returns the length of the queue line
 * (including the newline), or 0 if there is nothing to write. */
static size_t gwm_session_close(gwm_session_t *s, int64_t end, char *out, size_t out_size)
{
	char started[GWM_ISO_LEN], ended[GWM_ISO_LEN];
	size_t len = 0;

	if(!s->active) return 0;

	int64_t seconds = end - s->start;
	if(seconds >= GWM_MIN_SESSION_SEC && out_size > 1)
	{
		gwm_iso_from_unix(started, s->start);
		gwm_iso_from_unix(ended, end);

		len = gwm_build_session(out, out_size - 1, s->account, s->title_id,
		                        s->title[0] ? s->title : s->title_id,
		                        started, ended, seconds);
		out[len++] = '\n';
		out[len] = 0;
	}

	gwm_session_init(s);
	return len;
}

static void gwm_session_open(gwm_session_t *s, int64_t now, const char *title_id,
                             const char *title, const char *account)
{
	snprintf(s->title_id, sizeof(s->title_id), "%s", title_id);
	snprintf(s->title, sizeof(s->title), "%s", title ? title : "");
	snprintf(s->account, sizeof(s->account), "%s", account ? account : "");
	s->start = now;
	s->last_seen = now;
	s->active = 1;
}

/* Called on every poll. An empty title_id means no game. Returns the length
 * of the closed line in out (0 if nothing closed). Sets *changed when what
 * the heartbeat shows has changed. */
static size_t gwm_session_update(gwm_session_t *s, int64_t now,
                                 const char *title_id, const char *title,
                                 const char *account,
                                 char *out, size_t out_size, int *changed)
{
	int playing = title_id && title_id[0];
	size_t len = 0;

	*changed = 0;

	/* The clock jumped back (set by hand or synced), so durations cannot be
	 * measured with it. Close at the last moment seen. */
	int jumped_back = s->active && now < s->last_seen;
	int slept = s->active && now - s->last_seen > GWM_SLEEP_GAP_SEC;

	if(s->active && (slept || jumped_back))
	{
		len = gwm_session_close(s, s->last_seen, out, out_size);
		*changed = 1;
	}

	if(s->active && playing && strcmp(s->title_id, title_id) == 0)
	{
		s->last_seen = now;
		return len;
	}

	if(s->active)
	{
		/* Another game or back to the menu. This cannot clash with a
		 * session closed above: closing clears active. */
		len = gwm_session_close(s, now, out, out_size);
		*changed = 1;
	}

	if(playing)
	{
		gwm_session_open(s, now, title_id, title, account);
		*changed = 1;
	}

	return len;
}

#endif /* GWM_SESSION_H */
