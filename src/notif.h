/* Earned trophies from the notification database.
 *
 * /system_data/priv/mms/notification2.db, table notification. A trophy is a
 * row with bundle_name = 'local:Trophy': user_id is whose, created_date_time
 * is when (milliseconds, system clock, same as time()), raw_data is the
 * trophy itself (trophy.h).
 *
 * id is AUTOINCREMENT, so "new" means an id above the last one processed.
 * The first run processes everything in the database: the system keeps
 * notifications up to 30 days and 1000 per user, so a payload installed
 * after playing can still pick up recent trophies.
 *
 * Notifications are not an archive: the user can delete them and the system
 * evicts them, in practice within hours. They have to be read while the
 * payload runs. A processed row goes straight to the queue on disk, after
 * which the notification database no longer matters.
 */

#ifndef GWM_NOTIF_H
#define GWM_NOTIF_H

#include <stdint.h>
#include <sys/stat.h>

#include "db.h"
#include "isotime.h"
#include "log.h"
#include "queue.h"
#include "record.h"
#include "state.h"
#include "trophy.h"
#include "user.h"

#define GWM_NOTIF_DB       "/system_data/priv/mms/notification2.db"
#define GWM_TROPHY_QUEUE   GWM_DATA_DIR "/trophies.jsonl"

/* Notifications processed per poll. The first run may find hundreds of
 * rows, and the poll loop is single-threaded. */
#define GWM_NOTIF_BATCH 50

/* File change marker, so the database is not read every 10 seconds for nothing. */
static struct { time_t mtime; off_t size; int forced; } gwm_notif_seen = { 0, 0, 1 };

/* Returns the number of trophies queued. */
static int gwm_notif_poll(gwm_state_t *state, const char *cfg_account)
{
	struct stat st;
	sqlite3 *db;
	sqlite3_stmt *q = NULL;
	int queued = 0, more = 0;

	if(stat(GWM_NOTIF_DB, &st) != 0) return 0;
	if(!gwm_notif_seen.forced && st.st_mtime == gwm_notif_seen.mtime && st.st_size == gwm_notif_seen.size)
		return 0;

	db = gwm_db_open_copy(GWM_NOTIF_DB);
	if(!db) return 0; /* being written or unreadable: try next time */

	/* The database was recreated (console reset): ids restarted, so start over. */
	if(sqlite3_prepare_v2(db, "SELECT max(id) FROM notification", -1, &q, NULL) == SQLITE_OK)
	{
		if(sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 0) != SQLITE_NULL &&
		   sqlite3_column_int64(q, 0) < state->notif_last_id)
		{
			gwm_log("notifications: max(id) %lld < %lld, database recreated",
			        (long long)sqlite3_column_int64(q, 0), (long long)state->notif_last_id);
			state->notif_last_id = 0;
		}
		sqlite3_finalize(q);
	}

	if(sqlite3_prepare_v2(db,
	       "SELECT id, user_id, created_date_time, raw_data FROM notification "
	       "WHERE bundle_name = 'local:Trophy' AND id > ? ORDER BY id LIMIT ?",
	       -1, &q, NULL) != SQLITE_OK)
	{
		gwm_log("notifications: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 0;
	}

	sqlite3_bind_int64(q, 1, state->notif_last_id);
	sqlite3_bind_int(q, 2, GWM_NOTIF_BATCH);

	int64_t last = state->notif_last_id;
	int rows = 0, rc;

	while((rc = sqlite3_step(q)) == SQLITE_ROW)
	{
		int64_t id = sqlite3_column_int64(q, 0);
		int uid = (int)sqlite3_column_int64(q, 1);
		int64_t created_ms = sqlite3_column_int64(q, 2);
		const char *raw = (const char *)sqlite3_column_text(q, 3);
		gwm_trophy_t t;

		rows++;

		if(raw && gwm_trophy_parse(raw, &t))
		{
			char account[GWM_ACCOUNT_LEN], earned[GWM_ISO_LEN], line[GWM_RECORD_MAX];

			gwm_user_account(cfg_account, uid, account, sizeof(account));
			gwm_iso_from_unix(earned, created_ms / 1000);

			size_t len = gwm_build_trophy(line, sizeof(line) - 1, account, &t, earned);
			line[len++] = '\n';

			/* The queue refused (full or disk error): keep the mark, the
			 * trophy is processed again on the next poll. */
			if(gwm_queue_append(GWM_TROPHY_QUEUE, line, len) != 0)
			{
				gwm_log("trophy %s #%lld: queue refused", t.set_id, (long long)t.id);
				break;
			}

			gwm_log("trophy %s #%lld \"%s\" (%s) at %s",
			        t.set_id, (long long)t.id, t.name, t.grade, earned);
			queued++;
		}
		else
		{
			gwm_log("notification %lld: not a trophy or unparsable", (long long)id);
		}

		last = id;
	}

	more = (rc == SQLITE_ROW) || rows == GWM_NOTIF_BATCH;
	sqlite3_finalize(q);
	sqlite3_close(db);

	if(last != state->notif_last_id)
	{
		state->notif_last_id = last;
		gwm_state_save(state);
	}

	/* Everything processed: remember the file. Otherwise (batch limit or an
	 * error) read again on the next poll even if the file has not changed. */
	gwm_notif_seen.mtime = st.st_mtime;
	gwm_notif_seen.size = st.st_size;
	gwm_notif_seen.forced = more;

	return queued;
}

#endif /* GWM_NOTIF_H */
