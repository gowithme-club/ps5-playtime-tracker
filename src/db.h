/* Reading the system's SQLite databases.
 *
 * Small databases are opened as an in-memory copy: the file is read whole
 * and handed to SQLite through sqlite3_deserialize. The payload takes no
 * lock on the file and cannot get in the way of the system:
 * notification2.db and app.db are live, SceShellCore writes them. The price
 * is a chance of reading mid-write; the query then fails, or is not started
 * when a -journal file is present, and runs again on the next poll.
 *
 * notification2.db holds at most 1000 rows per user, app.db a few hundred
 * kilobytes. The telemetry journal (journal.h) is about 100 MB and written
 * several times a second, so copying it every poll is out. It is opened in
 * place, read-only, through the unix-none VFS, again without locks, and
 * SQLite reads only the pages a query needs. The caller checks such a read:
 * the file's stamp (gwm_db_stamp) must match before and after.
 */

#ifndef GWM_DB_H
#define GWM_DB_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "sqlite3.h"

#define GWM_DB_MAX_BYTES (16 * 1024 * 1024)

/* Whether a write is in progress: in rollback journal mode SQLite keeps a
 * -journal file next to the database until the transaction ends. */
static int gwm_db_busy(const char *path)
{
	char journal[256];
	struct stat st;

	snprintf(journal, sizeof(journal), "%s-journal", path);
	return stat(journal, &st) == 0 && st.st_size > 0;
}

static sqlite3 *gwm_db_open_copy(const char *path)
{
	struct stat st;
	sqlite3 *db = NULL;

	if(stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > GWM_DB_MAX_BYTES) return NULL;
	if(gwm_db_busy(path)) return NULL;

	FILE *f = fopen(path, "rb");
	if(!f) return NULL;

	sqlite3_int64 len = (sqlite3_int64)st.st_size;
	unsigned char *buf = sqlite3_malloc64((sqlite3_uint64)len);
	if(!buf) { fclose(f); return NULL; }

	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);

	if((sqlite3_int64)got != len || sqlite3_open(":memory:", &db) != SQLITE_OK)
	{
		sqlite3_free(buf);
		if(db) sqlite3_close(db);
		return NULL;
	}

	/* FREEONCLOSE hands the buffer to SQLite; sqlite3_close frees it. */
	if(sqlite3_deserialize(db, "main", buf, len, len,
	                       SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY) != SQLITE_OK)
	{
		sqlite3_close(db);
		return NULL;
	}

	return db;
}

/* A database stamp: the change counter from the header (bytes 24-27, which
 * SQLite bumps on every transaction in rollback journal mode), size and
 * mtime. Returns 0 if the file is missing, unreadable or being written. */
typedef struct
{
	uint32_t counter;
	off_t    size;
	time_t   mtime;
} gwm_db_stamp_t;

static int gwm_db_stamp(const char *path, gwm_db_stamp_t *s)
{
	struct stat st;
	unsigned char h[28];

	if(gwm_db_busy(path) || stat(path, &st) != 0) return 0;

	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	size_t got = fread(h, 1, sizeof(h), f);
	fclose(f);
	if(got != sizeof(h)) return 0;

	s->counter = (uint32_t)h[24] << 24 | (uint32_t)h[25] << 16 | (uint32_t)h[26] << 8 | h[27];
	s->size = st.st_size;
	s->mtime = st.st_mtime;
	return 1;
}

static int gwm_db_stamp_same(const gwm_db_stamp_t *a, const gwm_db_stamp_t *b)
{
	return a->counter == b->counter && a->size == b->size && a->mtime == b->mtime;
}

/* A database in place, read-only and without locks. The page cache is small:
 * journal queries touch hundreds of one-kilobyte pages. */
static sqlite3 *gwm_db_open_inplace(const char *path)
{
	sqlite3 *db = NULL;

	if(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, "unix-none") != SQLITE_OK)
	{
		if(db) sqlite3_close(db);
		return NULL;
	}

	sqlite3_exec(db, "PRAGMA cache_size = -256", NULL, NULL, NULL);
	return db;
}

#endif /* GWM_DB_H */
