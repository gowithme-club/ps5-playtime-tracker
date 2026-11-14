/* Integration with real SQLite and files; PS5 services are stubbed. */

#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include "config.h"
#undef GWM_DATA_DIR
#define GWM_DATA_DIR "."
#define GWM_GAME_H
#define GWM_USER_H
#define GWM_UPLOAD_H
#define GWM_LOG_H
#define GWM_SESSION_QUEUE GWM_DATA_DIR "/sessions.jsonl"
#define GWM_LOG_FILE GWM_DATA_DIR "/log.txt"
#define GWM_LOG_OLD_FILE GWM_DATA_DIR "/log.old.txt"
static void gwm_log(const char *fmt, ...) { (void)fmt; }
static int gwm_game_title(const char *id, char *out, size_t n) { snprintf(out,n,"%s",id); return 1; }
static void gwm_user_account(const char *a, int uid, char *out, size_t n) { (void)uid; snprintf(out,n,"%s",a); }
static int gwm_user_foreground(void) { return 1; }
#include "journal.h"

int main(void)
{
 char dir[] = "/tmp/gwm_journal_io_XXXXXX";
 CHECK(mkdtemp(dir) != NULL);
 CHECK(chdir(dir) == 0);
 sqlite3 *db = NULL; sqlite3_open(":memory:", &db);
 sqlite3_exec(db, "CREATE TABLE tbl_log(user_id0 INTEGER,created_date TEXT,updated_date TEXT,log TEXT,event_id TEXT)", NULL,NULL,NULL);
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,updated_date,log,event_id) VALUES(10,1,'2026-10-28T00:10:39Z','2026-10-28T00:10:39Z','{\"appCategoryType\":\"ps5_game\",\"appTitleId\":\"PPSA94004\",\"totalFgTime\":301}','ApplicationSessionEnd')", NULL,NULL,NULL);
 gwm_jread_t r = {0}; r.from = 11; r.offset_known = 1;
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(r.from == 11 && r.last_row == 11 && r.n_found == 0);
 r.from = gwm_journal_cursor(11, "old", "new");
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(r.n_found == 1 && r.last_row == 10);
 sqlite3_close(db);

 gwm_state_t state = {0}, loaded;
 state.journal_last_row = 12345;
 snprintf(state.journal_epoch, sizeof(state.journal_epoch), "2026-10-10T10:51:14.00Z");
 gwm_state_save(&state); gwm_state_load(&loaded);
 CHECK(loaded.journal_last_row == 12345);
 CHECK_STR(loaded.journal_epoch, state.journal_epoch);

 char line[GWM_RECORD_MAX];
 size_t len = gwm_build_session(line,sizeof(line)-1,"Player","CUSA03980","BioShock",
   "2026-10-28T00:00:00Z","2026-10-28T00:01:00Z",60);
 line[len++]='\n';
 FILE *f=fopen(GWM_PENDING_FILE,"w");
 while(ftell(f)+(long)len <= GWM_QUEUE_MAX_BYTES) fwrite(line,1,len,f);
 long before=ftell(f); fclose(f);
 int64_t end; gwm_iso_to_unix("2026-10-28T00:01:00Z",&end);
 CHECK(gwm_pending_settle(NULL,1,end+10) == 0);
 CHECK(gwm_file_size(GWM_PENDING_FILE) == before);
 unlink(GWM_PENDING_FILE); unlink(GWM_STATE_FILE);
 CHECK(chdir("/") == 0); rmdir(dir);
 return gwm_report("journal IO");
}
