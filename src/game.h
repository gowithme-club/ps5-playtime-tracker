/* What is running.
 *
 * A game is a process for which sceKernelGetAppInfo returns a title id that
 * is not a system one (NPXS...). System apps, dialogs and daemons have title
 * ids too, so NPXS is filtered out.
 *
 * In app_info the title id is at offset 16. The SDK's ps sample reads it at
 * 20 and gets the last five characters ("40082" instead of "NPXS40082").
 *
 * The game's name comes from app.db (tbl_contentinfo.titleName), written by
 * the system at install time. The server requires a title in every session.
 */

#ifndef GWM_GAME_H
#define GWM_GAME_H

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include "db.h"
#include "record.h"

#define GWM_APP_DB "/system_data/priv/mms/app.db"

typedef struct
{
	uint32_t app_id;
	uint64_t unknown1;
	char     title_id[14];
	char     unknown2[0x3c];
} gwm_app_info_t;

_Static_assert(__builtin_offsetof(gwm_app_info_t, title_id) == 16,
               "the title id is at offset 16, see the comment at the top");

int sceKernelGetAppInfo(pid_t pid, gwm_app_info_t *info);

/* Title id of the running game in out, or an empty string. */
static int gwm_game_detect(char out[GWM_TITLE_ID_LEN])
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
	size_t size = 0;
	int found = 0;

	out[0] = 0;

	if(sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size) return 0;

	/* The list can grow between the two calls, so leave headroom. */
	size += size / 4;
	char *buf = malloc(size);
	if(!buf) return 0;

	if(sysctl(mib, 4, buf, &size, NULL, 0) != 0) { free(buf); return 0; }

	for(char *p = buf; p + sizeof(int) <= buf + size && !found;)
	{
		struct kinfo_proc *ki = (struct kinfo_proc *)p;
		if(ki->ki_structsize <= 0 || p + ki->ki_structsize > buf + size) break;
		p += ki->ki_structsize;

		gwm_app_info_t info;
		memset(&info, 0, sizeof(info));
		if(sceKernelGetAppInfo(ki->ki_pid, &info) != 0) continue;
		if(!gwm_title_id_valid(info.title_id)) continue;
		if(!strncmp(info.title_id, "NPXS", 4)) continue;

		memcpy(out, info.title_id, 9);
		out[9] = 0;
		found = 1;
	}

	free(buf);
	return found;
}

/* The name for a title id from app.db. Returns 1 if found. */
static int gwm_game_title(const char *title_id, char *out, size_t size)
{
	sqlite3 *db = gwm_db_open_copy(GWM_APP_DB);
	sqlite3_stmt *st = NULL;
	int found = 0;

	out[0] = 0;
	if(!db) return 0;

	if(sqlite3_prepare_v2(db, "SELECT titleName FROM tbl_contentinfo WHERE titleId = ?",
	                      -1, &st, NULL) == SQLITE_OK)
	{
		sqlite3_bind_text(st, 1, title_id, -1, SQLITE_STATIC);
		if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
		{
			snprintf(out, size, "%s", (const char *)sqlite3_column_text(st, 0));
			found = out[0] != 0;
		}
		sqlite3_finalize(st);
	}

	sqlite3_close(db);
	return found;
}

#endif /* GWM_GAME_H */
