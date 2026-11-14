/* Sessions from the telemetry journal, including games played while the
 * payload was not running.
 *
 * /system_data/priv/system_logger2/nobackup/database/sl2_log.db, table
 * tbl_log. For every game the system writes rows with event_id
 * ApplicationSessionStart and ApplicationSessionEnd sharing an appSessionId;
 * End carries appTitleId, appCategoryType (ps4_game, ...) and totalFgTime,
 * the seconds in the foreground without rest mode and the PS menu. Whose
 * game it was is in column user_id0.
 *
 * The journal is an upload queue for Sony. A jailbroken console stays off
 * PSN, uploads fail and rows pile up, so the journal remembers games played
 * while the payload was not running. Closed sessions therefore come from
 * here, and the payload's own poll (session.h) is a fallback: its session
 * waits in pending.jsonl and is queued only if the journal shows no
 * overlapping session of the same game within GWM_PENDING_WAIT_SEC (the
 * journal is not written, or its rows already went to Sony).
 *
 * Clocks. The journal runs on its own clock, minutes away from the system
 * one. The offset is the difference between the newest timestamp inside the
 * journal and the file's mtime: the last write is the last change to the
 * file. Not always: the file also changes without a new timestamp (the
 * system deletes rows), and the reading then comes out low, e.g. 203 s
 * instead of 254. The error is always in that direction, so the offset is
 * the largest of the last GWM_JOURNAL_OFFSETS readings with a new
 * timestamp; a repeat of the same timestamp does not move the window. A
 * session is queued in system time like everything else, and the Date
 * offset shifts it later (upload.h).
 *
 * The session start comes from appSessionId: microseconds since 0001-01-01
 * on the journal clock, exactly the created_date of the Start row, so Start
 * is never looked up.
 *
 * Row parsing and overlaps are pure, covered by a host test.
 */

#ifndef GWM_JOURNAL_H
#define GWM_JOURNAL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotime.h"
#include "json.h"
#include "record.h"

/* Seconds from 0001-01-01 to 1970-01-01. */
#define GWM_JOURNAL_EPOCH 62135596800LL

/* Two sessions of one game closer than this are the same session seen from
 * two sides: the poll notices the exit up to 10 seconds late, and the
 * journal clock offset is not exact to the second either. */
#define GWM_JOURNAL_SLACK_SEC 120

typedef struct
{
	char    title_id[GWM_TITLE_ID_LEN];
	char    app_session_id[24];
	int64_t start;   /* journal clock */
	int64_t end;
	int64_t seconds;
} gwm_jsess_t;

/* A journal timestamp: "2026-10-07T17:06:56.00Z", with hundredths. */
static int gwm_journal_time(const char *s, int64_t *out)
{
	char iso[GWM_ISO_LEN];

	if(!s || strlen(s) < 19) return 0;
	memcpy(iso, s, 19);
	iso[19] = 'Z';
	iso[20] = 0;
	return gwm_iso_to_unix(iso, out);
}

/* appSessionId "63926989616000000" -> start time on the journal clock. */
static int gwm_journal_session_start(const char *id, int64_t *out)
{
	size_t n = strlen(id);
	int64_t v = 0;

	if(n < 12 || n > 18) return 0;
	for(size_t i = 0; i < n; i++)
	{
		if(id[i] < '0' || id[i] > '9') return 0;
		v = v * 10 + (id[i] - '0');
	}

	*out = v / 1000000 - GWM_JOURNAL_EPOCH;
	return *out > 0;
}

/* A game, not a system app or dialog. */
static int gwm_journal_is_game(const char *log, char *title_id, size_t size)
{
	char cat[32];

	if(!gwm_json_get_str(log, "appCategoryType", cat, sizeof(cat)) || !strstr(cat, "game")) return 0;
	if(!gwm_json_get_str(log, "appTitleId", title_id, size)) return 0;
	return strlen(title_id) == 9 && gwm_title_id_valid(title_id) && strncmp(title_id, "NPXS", 4) != 0;
}

/* An ApplicationSessionEnd row: log is its JSON, created its created_date.
 * Returns 1 for a game with a parsable time. */
static int gwm_journal_parse_end(const char *log, const char *created, gwm_jsess_t *s)
{
	int64_t fg = -1;

	memset(s, 0, sizeof(*s));
	if(!log || !gwm_journal_time(created, &s->end)) return 0;
	if(!gwm_journal_is_game(log, s->title_id, sizeof(s->title_id))) return 0;

	gwm_json_get_str(log, "appSessionId", s->app_session_id, sizeof(s->app_session_id));
	gwm_json_get_i64(log, "totalFgTime", &fg);

	int has_start = gwm_journal_session_start(s->app_session_id, &s->start) &&
	                s->start <= s->end && s->end - s->start <= 31 * 86400;

	if(fg < 0)
	{
		if(!has_start) return 0;
		fg = s->end - s->start;
	}
	if(!has_start) s->start = s->end - fg;

	s->seconds = fg;
	return 1;
}

/* Recent offset readings and the one chosen. The pure part is for the test. */
#define GWM_JOURNAL_OFFSETS 8

typedef struct
{
	int64_t v[GWM_JOURNAL_OFFSETS];
	int n, next;
	int64_t latest; /* last new timestamp, not just another file mtime */
} gwm_offsets_t;

static int64_t gwm_offsets_add(gwm_offsets_t *o, int64_t measured)
{
	o->v[o->next] = measured;
	o->next = (o->next + 1) % GWM_JOURNAL_OFFSETS;
	if(o->n < GWM_JOURNAL_OFFSETS) o->n++;

	int64_t best = o->v[0];
	for(int i = 1; i < o->n; i++) if(o->v[i] > best) best = o->v[i];
	return best;
}

/* The same timestamp with a changing mtime is not a new reading: otherwise
 * eight deletes or retries would push the right offset out of the window. */
static int64_t gwm_offsets_sample(gwm_offsets_t *o, int64_t latest, int64_t mtime)
{
	if(!o->n || latest > o->latest)
	{
		o->latest = latest;
		return gwm_offsets_add(o, latest - mtime);
	}
	int64_t best = o->v[0];
	for(int i = 1; i < o->n; i++) if(o->v[i] > best) best = o->v[i];
	return best;
}

/* rowid can go down when the last row is deleted. Starting over is right
 * only when the database generation changes, not on max(rowid) alone. */
static int64_t gwm_journal_cursor(int64_t last, const char *old_epoch, const char *epoch)
{
	return old_epoch[0] && epoch[0] && strcmp(old_epoch, epoch) ? 0 : last;
}

/* A stable session identity independent of both clock offsets. */
static size_t gwm_journal_record(char *out, size_t size, const char *account,
                                 const gwm_jsess_t *js, int uid, const char *title,
                                 const char *started, const char *ended)
{
	size_t len = gwm_build_session(out, size, account, js->title_id, title, started, ended, js->seconds);
	int64_t raw_start;
	if(!len || !gwm_journal_session_start(js->app_session_id, &raw_start)) return len;
	char id[96];
	snprintf(id, sizeof(id), "ps5-journal:%08x:%s:%s", (unsigned)uid, js->title_id, js->app_session_id);
	char *p = out + len - 1;
	p = gwm_lit(p, out + size, ",");
	p = gwm_json_kv(p, out + size, "sourceSessionId", id);
	p = gwm_lit(p, out + size, "}");
	return (size_t)(p - out);
}

static int gwm_spans_overlap(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
	return a0 <= b1 + GWM_JOURNAL_SLACK_SEC && b0 <= a1 + GWM_JOURNAL_SLACK_SEC;
}

/* Recently queued sessions, so one game is not sent twice, from the
 * journal and from the poll. System time. Poll sessions are kept on disk
 * (polled.txt): the journal can be unreadable for days and then return the
 * same games, and the server, which matches sessions by exact startedAt,
 * would count them twice. 256 is weeks of play. */
#define GWM_SPANS 256

typedef struct
{
	struct { char title_id[GWM_TITLE_ID_LEN]; int64_t start, end; } s[GWM_SPANS];
	int n, next;
} gwm_spans_t;

static void gwm_spans_add(gwm_spans_t *l, const char *title_id, int64_t start, int64_t end)
{
	snprintf(l->s[l->next].title_id, GWM_TITLE_ID_LEN, "%s", title_id);
	l->s[l->next].start = start;
	l->s[l->next].end = end;
	l->next = (l->next + 1) % GWM_SPANS;
	if(l->n < GWM_SPANS) l->n++;
}

static int gwm_spans_hit(const gwm_spans_t *l, const char *title_id, int64_t start, int64_t end)
{
	for(int i = 0; i < l->n; i++)
		if(!strcmp(l->s[i].title_id, title_id) && gwm_spans_overlap(l->s[i].start, l->s[i].end, start, end))
			return 1;
	return 0;
}

/* Game and bounds from a session queue line (record.h). */
static int gwm_session_line_span(const char *line, char *title_id, size_t size, int64_t *start, int64_t *end)
{
	char a[32], b[32];

	return gwm_json_get_str(line, "titleId", title_id, size) &&
	       gwm_json_get_str(line, "startedAt", a, sizeof(a)) && strlen(a) == GWM_ISO_LEN - 1 &&
	       gwm_json_get_str(line, "endedAt", b, sizeof(b)) && strlen(b) == GWM_ISO_LEN - 1 &&
	       gwm_iso_to_unix(a, start) && gwm_iso_to_unix(b, end);
}

#ifndef GWM_HOST_TEST

#include <sys/stat.h>

#include "db.h"
#include "game.h"
#include "log.h"
#include "queue.h"
#include "session.h"
#include "state.h"
#include "upload.h"
#include "user.h"

#define GWM_JOURNAL_DB "/system_data/priv/system_logger2/nobackup/database/sl2_log.db"
#define GWM_PENDING_FILE GWM_DATA_DIR "/pending.jsonl"
#define GWM_POLLED_FILE GWM_DATA_DIR "/polled.txt"

/* The journal is read in place while the system writes to it several times
 * a second. A pass that overlaps a write is thrown away whole, so it must be
 * short: at most GWM_JOURNAL_BATCH End rows and GWM_JOURNAL_WINDOW rowids at
 * a time, the rest on later polls. */
#define GWM_JOURNAL_BATCH  50
#define GWM_JOURNAL_WINDOW 4000
/* Attempts per poll if the journal changes mid-read. */
#define GWM_JOURNAL_TRIES 3
/* The clock offset comes from the latest rows, not the whole table: a full
 * scan reads 20 MB. The system writes a fresh row every few seconds. */
#define GWM_JOURNAL_CALIBRATE_ROWS 256
/* With no waiting sessions the journal is read at most this often: it only
 * brings games played without the payload, which are not urgent. */
#define GWM_JOURNAL_IDLE_SEC 60
/* After this many failed polls in a row, write to the log. */
#define GWM_JOURNAL_FAILS_LOG 6

/* How long a poll session waits for the journal to show it. */
#define GWM_PENDING_WAIT_SEC 300
/* The journal exists but has been unreadable this long: give up and trust the poll. */
#define GWM_PENDING_GIVE_UP_SEC 1800
/* Lines of pending.jsonl the pass answers "in the journal" for. */
#define GWM_PENDING_LINES 128

/* Journal clock minus system clock. */
static int64_t gwm_journal_offset = 0;
static int gwm_journal_offset_known = 0;
static gwm_offsets_t gwm_journal_offsets;

static gwm_spans_t gwm_journal_spans;  /* queued from the journal */
static gwm_spans_t gwm_live_spans;     /* queued from the poll, copied to polled.txt */

static struct { int64_t next_check; int forced; int fails; int missing_logged; } gwm_journal_seen = { 0, 1, 0, 0 };

/* What one pass read. It reaches the queue and state only if the journal
 * did not change during the pass. */
typedef struct
{
	gwm_offsets_t offsets;
	int64_t offset;
	int offset_known;

	struct { gwm_jsess_t s; int64_t row; int uid; } found[GWM_JOURNAL_BATCH];
	int n_found;
	int64_t from;      /* journal_last_row the pass started from */
	int64_t last_row;  /* last row read */
	int64_t max_row;
	char epoch[32];
	int more;

	signed char covered[GWM_PENDING_LINES];
} gwm_jread_t;

static gwm_jread_t gwm_jread;

/* Titles by title id: on the first run the journal may return hundreds of
 * sessions, and each must not copy app.db again. */
static void gwm_journal_title(const char *title_id, char *out, size_t size)
{
	static struct { char id[GWM_TITLE_ID_LEN]; char name[GWM_TITLE_LEN]; } cache[16];
	static int cached = 0;

	for(int i = 0; i < cached; i++)
		if(!strcmp(cache[i].id, title_id)) { snprintf(out, size, "%s", cache[i].name); return; }

	if(!gwm_game_title(title_id, out, size)) snprintf(out, size, "%s", title_id);

	if(cached < 16)
	{
		snprintf(cache[cached].id, GWM_TITLE_ID_LEN, "%s", title_id);
		snprintf(cache[cached].name, GWM_TITLE_LEN, "%s", out);
		cached++;
	}
}

/* polled.txt: "PPSA35901 1760000000 1760000600" per line, oldest first. */
static void gwm_polled_save(void)
{
	const char *tmp = GWM_POLLED_FILE ".tmp";
	const gwm_spans_t *l = &gwm_live_spans;
	FILE *f = fopen(tmp, "w");

	if(!f) return;
	for(int i = 0; i < l->n; i++)
	{
		int k = (l->next - l->n + i + GWM_SPANS) % GWM_SPANS;
		fprintf(f, "%s %lld %lld\n", l->s[k].title_id, (long long)l->s[k].start, (long long)l->s[k].end);
	}
	int failed = fflush(f) != 0 || ferror(f);
	fsync(fileno(f));
	fclose(f);

	if(failed) unlink(tmp);
	else rename(tmp, GWM_POLLED_FILE);
}

/* Once per run. */
static void gwm_polled_load(void)
{
	static int loaded = 0;
	char id[GWM_TITLE_ID_LEN];
	long long a, b;

	if(loaded) return;
	loaded = 1;

	FILE *f = fopen(GWM_POLLED_FILE, "r");
	if(!f) return;
	while(fscanf(f, "%15s %lld %lld", id, &a, &b) == 3) gwm_spans_add(&gwm_live_spans, id, a, b);
	fclose(f);
}

/* Clock offset from the latest rows. 0 on an SQLite error. */
static int gwm_journal_calibrate(sqlite3 *db, time_t mtime, gwm_jread_t *r)
{
	sqlite3_stmt *q = NULL;
	char latest[32] = "";
	int rc;

	if(sqlite3_prepare_v2(db,
	       "SELECT created_date, updated_date FROM tbl_log ORDER BY rowid DESC LIMIT ?",
	       -1, &q, NULL) != SQLITE_OK)
		return 0;

	sqlite3_bind_int(q, 1, GWM_JOURNAL_CALIBRATE_ROWS);
	while((rc = sqlite3_step(q)) == SQLITE_ROW)
		for(int i = 0; i < 2; i++)
		{
			const char *t = (const char *)sqlite3_column_text(q, i);
			if(t && strcmp(t, latest) > 0) snprintf(latest, sizeof(latest), "%s", t);
		}
	sqlite3_finalize(q);
	if(rc != SQLITE_DONE) return 0;

	int64_t t;
	if(gwm_journal_time(latest, &t))
	{
		r->offset = gwm_offsets_sample(&r->offsets, t, (int64_t)mtime);
		r->offset_known = 1;
	}
	return 1;
}

/* New End rows after r->from. 0 on an SQLite error. */
static int gwm_journal_scan_ends(sqlite3 *db, gwm_jread_t *r)
{
	sqlite3_stmt *q = NULL;
	int rows = 0, rc;

	if(sqlite3_prepare_v2(db, "SELECT ifnull(max(rowid), 0) FROM tbl_log", -1, &q, NULL) != SQLITE_OK) return 0;
	rc = sqlite3_step(q);
	r->max_row = sqlite3_column_int64(q, 0);
	sqlite3_finalize(q);
	if(rc != SQLITE_ROW) return 0;

	r->last_row = r->from;

	/* Deleting the last rows does not move the saved mark back. */
	if(r->max_row <= r->from) return 1;

	int64_t upper = r->from + GWM_JOURNAL_WINDOW;
	if(upper > r->max_row) upper = r->max_row;

	if(sqlite3_prepare_v2(db,
	       "SELECT rowid, user_id0, created_date, log FROM tbl_log "
	       "WHERE event_id = 'ApplicationSessionEnd' AND rowid > ? AND rowid <= ? ORDER BY rowid LIMIT ?",
	       -1, &q, NULL) != SQLITE_OK)
		return 0;

	sqlite3_bind_int64(q, 1, r->from);
	sqlite3_bind_int64(q, 2, upper);
	sqlite3_bind_int(q, 3, GWM_JOURNAL_BATCH);

	while((rc = sqlite3_step(q)) == SQLITE_ROW)
	{
		int64_t row = sqlite3_column_int64(q, 0);
		gwm_jsess_t js;

		rows++;
		r->last_row = row;

		if(gwm_journal_parse_end((const char *)sqlite3_column_text(q, 3), (const char *)sqlite3_column_text(q, 2), &js) &&
		   js.seconds >= GWM_MIN_SESSION_SEC)
		{
			r->found[r->n_found].s = js;
			r->found[r->n_found].row = row;
			r->found[r->n_found].uid = (int)sqlite3_column_int64(q, 1);
			r->n_found++;
		}
	}
	sqlite3_finalize(q);
	if(rc != SQLITE_DONE) return 0;

	/* The whole window was scanned, so the mark moves to its end rather than
	 * to the last End row: otherwise every poll would page through thousands
	 * of telemetry rows after it again. rowids only grow, so no new rows can
	 * appear below max(rowid). */
	if(rows < GWM_JOURNAL_BATCH) r->last_row = upper;
	r->more = r->last_row < r->max_row;
	return 1;
}

/* Whether the journal has a session of this game overlapping [js, je]
 * (journal clock): closed, or still running, i.e. the last Start without its
 * End. Rows go from newest to oldest down to the start of [js, je]: an End
 * before it cannot overlap, and a running session started earlier means the
 * payload was launched mid-game; an End will close it seconds after exit.
 * 1 yes, 0 no, -1 SQLite error. */
static int gwm_journal_covers(sqlite3 *db, const char *title_id, int64_t js, int64_t je)
{
	sqlite3_stmt *q = NULL;
	char needle[48], ended[16][24];
	int n_ended = 0, seen_start = 0, covered = 0, rc;

	snprintf(needle, sizeof(needle), "\"appTitleId\":\"%s\"", title_id);

	if(sqlite3_prepare_v2(db,
	       "SELECT event_id, created_date, log FROM tbl_log "
	       "WHERE event_id IN ('ApplicationSessionStart', 'ApplicationSessionEnd') ORDER BY rowid DESC",
	       -1, &q, NULL) != SQLITE_OK)
		return -1;

	while((rc = sqlite3_step(q)) == SQLITE_ROW)
	{
		const char *ev = (const char *)sqlite3_column_text(q, 0);
		const char *created = (const char *)sqlite3_column_text(q, 1);
		int64_t t;

		if(!ev || !gwm_journal_time(created, &t)) continue;
		if(t < js - GWM_JOURNAL_SLACK_SEC) break;

		const char *log = (const char *)sqlite3_column_text(q, 2);
		gwm_jsess_t s;

		if(!log || !strstr(log, needle)) continue;

		if(!strcmp(ev, "ApplicationSessionEnd"))
		{
			if(!gwm_journal_parse_end(log, created, &s)) continue;
			if(gwm_spans_overlap(s.start, s.end, js, je)) { covered = 1; break; }
			if(n_ended < 16) snprintf(ended[n_ended++], sizeof(ended[0]), "%s", s.app_session_id);
		}
		else if(!seen_start)
		{
			/* Rows go from newest to oldest, so the last session's End,
			 * if there is one, has already been seen above. */
			char id[24] = "";
			int open = 1;

			seen_start = 1;
			gwm_json_get_str(log, "appSessionId", id, sizeof(id));
			for(int i = 0; i < n_ended; i++) if(!strcmp(ended[i], id)) open = 0;

			if(open && t <= je + GWM_JOURNAL_SLACK_SEC) { covered = 1; break; }
		}
	}

	sqlite3_finalize(q);
	if(rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
	return covered;
}

/* For each line of pending.jsonl, whether the journal has it. 0 on an SQLite error. */
static int gwm_journal_pending_covered(sqlite3 *db, gwm_jread_t *r)
{
	static char buf[16384];
	int i = 0;

	memset(r->covered, 0, sizeof(r->covered));
	if(!r->offset_known || !gwm_queue_read(GWM_PENDING_FILE, buf, sizeof(buf))) return 1;

	for(char *line = buf; *line && i < GWM_PENDING_LINES; i++)
	{
		char *nl = strchr(line, '\n');
		if(!nl) break;
		*nl = 0;

		char title_id[GWM_TITLE_ID_LEN];
		int64_t start, end;

		if(gwm_session_line_span(line, title_id, sizeof(title_id), &start, &end))
		{
			int c = gwm_journal_covers(db, title_id, start + r->offset, end + r->offset);
			if(c < 0) return 0;
			r->covered[i] = (signed char)c;
		}
		line = nl + 1;
	}
	return 1;
}

/* One pass over the journal. Returns 1 if it was read and did not change
 * meanwhile; otherwise *why says why not. */
static int gwm_journal_read(gwm_state_t *state, gwm_jread_t *r, char *why, size_t why_size)
{
	gwm_db_stamp_t before, after;

	memset(r, 0, sizeof(*r));
	r->offsets = gwm_journal_offsets;
	r->offset = gwm_journal_offset;
	r->offset_known = gwm_journal_offset_known;
	r->from = state->journal_last_row;

	if(!gwm_db_stamp(GWM_JOURNAL_DB, &before)) { snprintf(why, why_size, "being written"); return 0; }

	sqlite3 *db = gwm_db_open_inplace(GWM_JOURNAL_DB);
	if(!db) { snprintf(why, why_size, "cannot be opened"); return 0; }

	/* The config's created_date is set when the database is created and does
	 * not change as tbl_log rows rotate. A small table, one row by rowid. */
	sqlite3_stmt *generation = NULL;
	int epoch_ok = sqlite3_prepare_v2(db,
	    "SELECT created_date FROM tbl_config ORDER BY rowid LIMIT 1", -1, &generation, NULL) == SQLITE_OK;
	if(epoch_ok && sqlite3_step(generation) == SQLITE_ROW)
	{
		const char *epoch = (const char *)sqlite3_column_text(generation, 0);
		if(epoch) snprintf(r->epoch, sizeof(r->epoch), "%s", epoch);
	}
	sqlite3_finalize(generation);
	if(!epoch_ok || !r->epoch[0])
	{
		snprintf(why, why_size, "unknown journal generation");
		sqlite3_close(db);
		return 0;
	}
	r->from = gwm_journal_cursor(r->from, state->journal_epoch, r->epoch);
	if(r->from != state->journal_last_row)
	{
		memset(&r->offsets, 0, sizeof(r->offsets));
		r->offset_known = 0;
	}

	int ok = gwm_journal_calibrate(db, before.mtime, r) &&
	         (!r->offset_known || gwm_journal_scan_ends(db, r)) &&
	         gwm_journal_pending_covered(db, r);
	if(!ok) snprintf(why, why_size, "%s", sqlite3_errmsg(db));
	sqlite3_close(db);

	if(ok && !(gwm_db_stamp(GWM_JOURNAL_DB, &after) && gwm_db_stamp_same(&before, &after)))
	{
		snprintf(why, why_size, "changed during the read");
		ok = 0;
	}
	return ok;
}

/* Queue what the pass read and update state. Returns how many were queued. */
static int gwm_journal_commit(gwm_state_t *state, const char *cfg_account, gwm_jread_t *r)
{
	int queued = 0;

	if(!r->offset_known) return 0;

	if(!gwm_journal_offset_known || r->offset - gwm_journal_offset > 5 || gwm_journal_offset - r->offset > 5)
		gwm_log("journal: its clock minus system = %lld s", (long long)r->offset);
	gwm_journal_offsets = r->offsets;
	gwm_journal_offset = r->offset;
	gwm_journal_offset_known = 1;

	if(r->from != state->journal_last_row)
		gwm_log("journal: generation changed from %s to %s, database recreated",
		        state->journal_epoch, r->epoch);

	for(int i = 0; i < r->n_found; i++)
	{
		const gwm_jsess_t *js = &r->found[i].s;
		int64_t start = js->start - gwm_journal_offset, end = js->end - gwm_journal_offset;

		if(gwm_spans_hit(&gwm_live_spans, js->title_id, start, end))
		{
			gwm_log("journal: %s already sent from the poll, skipping", js->title_id);
			continue;
		}

		char account[GWM_ACCOUNT_LEN], title[GWM_TITLE_LEN], started[GWM_ISO_LEN], ended[GWM_ISO_LEN];
		char line[GWM_RECORD_MAX];

		gwm_user_account(cfg_account, r->found[i].uid > 0 ? r->found[i].uid : gwm_user_foreground(),
		                 account, sizeof(account));
		gwm_journal_title(js->title_id, title, sizeof(title));
		gwm_iso_from_unix(started, start);
		gwm_iso_from_unix(ended, end);

		size_t len = gwm_journal_record(line, sizeof(line) - 1, account, js, r->found[i].uid, title,
		                                started, ended);
		line[len++] = '\n';

		/* The queue refused: the mark stops before this row, which will be
		 * processed again. */
		if(gwm_queue_append(GWM_SESSION_QUEUE, line, len) != 0)
		{
			gwm_log("journal: session %s not queued", js->title_id);
			r->last_row = r->found[i].row - 1;
			r->more = 1;
			break;
		}

		gwm_spans_add(&gwm_journal_spans, js->title_id, start, end);
		gwm_log("journal: %s %s - %s, %lld s", js->title_id, started, ended, (long long)js->seconds);
		queued++;
	}

	if(r->last_row != state->journal_last_row || strcmp(state->journal_epoch, r->epoch))
	{
		snprintf(state->journal_epoch, sizeof(state->journal_epoch), "%s", r->epoch);
		state->journal_last_row = r->last_row;
		gwm_state_save(state);
	}

	return queued;
}

/* Poll sessions from pending.jsonl: shown by the journal, drop; not shown
 * in time, queue. covered is the pass's answer per line, NULL when the
 * journal cannot be read now. */
static int gwm_pending_settle(const signed char *covered, int journal_exists, int64_t now)
{
	static char buf[16384];
	char keep[sizeof(buf)];
	size_t kept = 0;
	int queued = 0, i = 0, saved = 0;

	size_t len = gwm_queue_read(GWM_PENDING_FILE, buf, sizeof(buf));
	if(!len) return 0;

	for(char *line = buf; *line; i++)
	{
		char *nl = strchr(line, '\n');
		if(!nl) break;
		size_t line_len = (size_t)(nl - line) + 1;

		char title_id[GWM_TITLE_ID_LEN];
		int64_t start, end;
		*nl = 0;

		if(!gwm_session_line_span(line, title_id, sizeof(title_id), &start, &end))
		{
			gwm_log("pending: unparsable line dropped: %.80s", line);
		}
		else if(gwm_spans_hit(&gwm_journal_spans, title_id, start, end) ||
		        (covered && i < GWM_PENDING_LINES && covered[i] > 0))
		{
			gwm_log("pending: %s found in the journal, poll session dropped", title_id);
		}
		else if(!journal_exists || (covered && now - end >= GWM_PENDING_WAIT_SEC) || now - end >= GWM_PENDING_GIVE_UP_SEC)
		{
			*nl = '\n';
			if(gwm_queue_append(GWM_SESSION_QUEUE, line, line_len) == 0)
			{
				gwm_spans_add(&gwm_live_spans, title_id, start, end);
				saved = 1;
				gwm_log("pending: %s not in the journal, sending the poll session", title_id);
				queued++;
			}
			else if(kept + line_len < sizeof(keep))
			{
				memcpy(keep + kept, line, line_len);
				kept += line_len;
			}
		}
		else if(kept + line_len < sizeof(keep))
		{
			*nl = '\n';
			memcpy(keep + kept, line, line_len);
			kept += line_len;
		}

		line = nl + 1;
	}

	if(saved) gwm_polled_save();

	/* Append what remains first, then cut what was read: a crash in between
	 * repeats a line, and the server collapses identical lines. */
	if(kept && gwm_queue_append(GWM_PENDING_FILE, keep, kept) != 0) return queued;
	gwm_queue_consume(GWM_PENDING_FILE, len);

	return queued;
}

/* A poll session closed: wait for the journal. */
static void gwm_pending_add(const char *line, size_t len)
{
	if(gwm_queue_append(GWM_PENDING_FILE, line, len) != 0)
		gwm_log("pending: session not saved: %.*s", (int)len - 1, line);
	else
		gwm_log("poll session: %.*s", (int)len - 1, line);
}

/* Called on every poll. Returns how many sessions were queued. */
static int gwm_journal_poll(gwm_state_t *state, const char *cfg_account, int64_t now)
{
	struct stat st;
	char why[128] = "";
	int queued = 0, ok = 0;
	int exists = stat(GWM_JOURNAL_DB, &st) == 0;
	int waiting = gwm_file_size(GWM_PENDING_FILE) > 0;

	/* The journal should confirm a waiting session quickly: that decides
	 * when the "recorded" notification arrives. */
	if(!waiting && !gwm_journal_seen.forced && now < gwm_journal_seen.next_check) return 0;
	gwm_journal_seen.next_check = now + GWM_JOURNAL_IDLE_SEC;

	gwm_polled_load();

	if(!exists)
	{
		if(!gwm_journal_seen.missing_logged) gwm_log("journal %s missing, sessions come from the poll only", GWM_JOURNAL_DB);
		gwm_journal_seen.missing_logged = 1;
	}
	else
	{
		gwm_journal_seen.missing_logged = 0;
		for(int i = 0; i < GWM_JOURNAL_TRIES && !ok; i++) ok = gwm_journal_read(state, &gwm_jread, why, sizeof(why));
	}

	if(ok)
	{
		if(gwm_journal_seen.fails >= GWM_JOURNAL_FAILS_LOG)
			gwm_log("journal: readable again after %d failed polls", gwm_journal_seen.fails);
		gwm_journal_seen.fails = 0;
		queued += gwm_journal_commit(state, cfg_account, &gwm_jread);
	}
	else if(exists)
	{
		/* A pass that overlaps a write is routine. Log only when it drags on:
		 * poll sessions then go out only after half an hour. */
		gwm_journal_seen.fails++;
		if(gwm_journal_seen.fails == GWM_JOURNAL_FAILS_LOG || gwm_journal_seen.fails % 360 == 0)
			gwm_log("journal: unreadable for %d polls in a row (%s), poll sessions go out in %d min",
			        gwm_journal_seen.fails, why, GWM_PENDING_GIVE_UP_SEC / 60);
	}

	if(waiting) queued += gwm_pending_settle(ok ? gwm_jread.covered : NULL, exists, now);

	/* Not read, or not all of it: again on the next poll. */
	gwm_journal_seen.forced = exists && (!ok || gwm_jread.more);

	return queued;
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_JOURNAL_H */
