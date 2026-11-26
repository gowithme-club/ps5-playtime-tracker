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
static int gwm_user_id_of(const char *a) { return a && !strcmp(a, "Mira") ? 317125740 : -1; }
#include "journal.h"

int main(void)
{
 char dir[] = "/tmp/gwm_journal_io_XXXXXX";
 CHECK(mkdtemp(dir) != NULL);
 CHECK(chdir(dir) == 0);
 sqlite3 *db = NULL; sqlite3_open(":memory:", &db);
 sqlite3_exec(db, "CREATE TABLE tbl_log(user_id0 INTEGER,user_id1 INTEGER DEFAULT -1,user_id2 INTEGER DEFAULT -1,user_id3 INTEGER DEFAULT -1,created_date TEXT,updated_date TEXT,log TEXT,log_header TEXT,event_id TEXT)", NULL,NULL,NULL);
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,updated_date,log,event_id) VALUES(10,1,'2026-10-28T00:10:39Z','2026-10-28T00:10:39Z','{\"appCategoryType\":\"ps5_game\",\"appTitleId\":\"PPSA94004\",\"totalFgTime\":301}','ApplicationSessionEnd')", NULL,NULL,NULL);
 gwm_jread_t r = {0}; r.from = 11; r.offset_known = 1;
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(r.from == 11 && r.last_row == 11 && r.n_found == 0);
 r.from = gwm_journal_cursor(11, "old", "new");
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(r.n_found == 1 && r.last_row == 10);
 sqlite3_close(db);

 /* Played or left, from Start and End rows; who a session belongs to. Rows
  * shaped like the console's: Mira (0x12e6f46c) starts Burnout, Fire signs
  * in on a second controller and the journal names nobody else. */
 db = NULL; sqlite3_open(":memory:", &db);
 sqlite3_exec(db, "CREATE TABLE tbl_log(user_id0 INTEGER,user_id1 INTEGER DEFAULT -1,user_id2 INTEGER DEFAULT -1,user_id3 INTEGER DEFAULT -1,created_date TEXT,updated_date TEXT,log TEXT,log_header TEXT,event_id TEXT)", NULL,NULL,NULL);
 const char *start_burnout = "INSERT INTO tbl_log(rowid,user_id0,created_date,log,log_header,event_id) VALUES(20,317125740,'2026-11-26T17:48:29.00Z',"
   "'{\"appCategoryType\":\"ps5_native_game\",\"appTitleId\":\"PPSA92006\",\"appSessionId\":\"63931312109000000\"}',"
   "'{\"localUserIds\":[\"12e6f46c\"]}','ApplicationSessionStart')";
 sqlite3_exec(db, start_burnout, NULL,NULL,NULL);
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,log,event_id) VALUES(21,317125739,'2026-11-26T17:48:50.00Z','{\"isGuestUser\":false}','ConsoleLogin')", NULL,NULL,NULL);
 memset(&r, 0, sizeof(r)); r.from = 15; r.offset_known = 1;
 CHECK(gwm_journal_scan_ends(db, &r));
 int users[GWM_PLAYERS_MAX], n_users;
 CHECK(gwm_jfg_playing(&r.jfg, "PPSA92006", users, &n_users) == 1 && n_users == 1 && users[0] == 317125740);
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,log,log_header,event_id) VALUES(30,317125740,'2026-11-26T18:05:00.00Z',"
   "'{\"appCategoryType\":\"ps5_native_game\",\"appTitleId\":\"PPSA92006\",\"appSessionId\":\"63931312109000000\",\"totalFgTime\":991,\"sessionEndReason\":\"suspend\"}',"
   "'{\"localUserIds\":[\"12e6f46c\"]}','ApplicationSessionEnd')", NULL,NULL,NULL);
 r.from = r.last_row; r.n_found = 0;
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(gwm_jfg_playing(&r.jfg, "PPSA92006", users, &n_users) == 0);
 CHECK(r.n_found == 1 && r.found[0].uid == 317125740);

 /* The session ends with a row per profile signed in, each a session of
  * its own, whatever the controller said (Remote Play counts 0). */
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,log,log_header,event_id) VALUES(40,317125739,'2026-11-26T18:30:00.00Z',"
   "'{\"appCategoryType\":\"ps5_native_game\",\"appTitleId\":\"PPSA92006\",\"appSessionId\":\"63931314000000000\",\"totalFgTime\":324,\"userControllerActiveTime\":0}',"
   "'{\"localUserIds\":[\"12e6f46b\"]}','ApplicationSessionEnd')", NULL,NULL,NULL);
 sqlite3_exec(db, "INSERT INTO tbl_log(rowid,user_id0,created_date,log,log_header,event_id) VALUES(41,317125740,'2026-11-26T18:30:00.00Z',"
   "'{\"appCategoryType\":\"ps5_native_game\",\"appTitleId\":\"PPSA92006\",\"appSessionId\":\"63931314000000000\",\"totalFgTime\":324,\"userControllerActiveTime\":321}',"
   "'{\"localUserIds\":[\"12e6f46c\"]}','ApplicationSessionEnd')", NULL,NULL,NULL);
 r.from = r.last_row; r.n_found = 0;
 CHECK(gwm_journal_scan_ends(db, &r));
 CHECK(r.n_found == 2 && r.found[0].uid == 317125739 && r.found[1].uid == 317125740);

 /* A copy started later looks back once: left, so the loaded game is not
  * played; a game the journal never saw is unknown. */
 gwm_jread_t fresh; memset(&fresh, 0, sizeof(fresh)); fresh.last_row = 30;
 CHECK(gwm_journal_look_back(db, &fresh, "PPSA92006"));
 CHECK(gwm_jfg_playing(&fresh.jfg, "PPSA92006", users, &n_users) == 0);
 CHECK(gwm_journal_look_back(db, &fresh, "PPSA99306"));
 CHECK(gwm_jfg_playing(&fresh.jfg, "PPSA99306", users, &n_users) == -1);
 fresh.last_row = 25;
 memset(&fresh.jfg, 0, sizeof(fresh.jfg));
 CHECK(gwm_journal_look_back(db, &fresh, "PPSA92006"));
 CHECK(gwm_jfg_playing(&fresh.jfg, "PPSA92006", users, &n_users) == 1);

 /* Mira's journal session covers her poll session, not Fire's. */
 int64_t js, je;
 gwm_iso_to_unix("2026-11-26T17:49:00Z", &js); gwm_iso_to_unix("2026-11-26T18:04:00Z", &je);
 CHECK(gwm_journal_covers(db, "PPSA92006", 317125740, js, je) == 1);
 CHECK(gwm_journal_covers(db, "PPSA92006", 317125739, js, je) == 0);
 CHECK(gwm_journal_covers(db, "PPSA92006", -1, js, je) == 1);
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
