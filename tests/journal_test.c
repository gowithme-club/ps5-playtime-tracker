#include "check.h"
#include "journal.h"

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if(!f) return NULL;
	static char buf[65536];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

int main(void)
{
	gwm_jsess_t s;
	int64_t t;
	char iso[GWM_ISO_LEN];

	CHECK(gwm_journal_time("2026-10-07T21:42:01.00Z", &t));
	gwm_iso_from_unix(iso, t);
	CHECK_STR(iso, "2026-10-07T21:42:01Z");
	CHECK(!gwm_journal_time("2026-10-07", &t));

	/* appSessionId is the start time: this session's Start row was written at
	 * 21:38:53, the id is a fraction of a second behind created_date. */
	CHECK(gwm_journal_session_start("63927005932000000", &t));
	gwm_iso_from_unix(iso, t);
	CHECK_STR(iso, "2026-10-07T21:38:52Z");
	CHECK(!gwm_journal_session_start("0", &t));
	CHECK(!gwm_journal_session_start("6392700593200000x", &t));

	/* A real ApplicationSessionEnd row from the console: P.T., 184 s. */
	char *log = slurp("tests/fixtures/journal_end_ps4.json");
	CHECK(log != NULL);
	CHECK(gwm_journal_parse_end(log, "2026-10-07T21:42:01.00Z", &s));
	CHECK_STR(s.title_id, "CUSA01127");
	CHECK_STR(s.app_session_id, "63927005932000000");
	CHECK(s.seconds == 184);
	CHECK(s.end - s.start == 189);

	/* fgTime next to totalFgTime does not confuse the parser. */
	CHECK(gwm_journal_parse_end("{\"appCategoryType\":\"ps5_game\",\"appTitleId\":\"PPSA01234\","
	                            "\"appSessionId\":\"63927005932000000\",\"fgTime\":5,\"totalFgTime\":60}",
	                            "2026-10-07T21:42:01.00Z", &s));
	CHECK(s.seconds == 60);

	/* Without appSessionId the start is the end minus foreground time. */
	CHECK(gwm_journal_parse_end("{\"appCategoryType\":\"ps4_game\",\"appTitleId\":\"CUSA03980\",\"totalFgTime\":65}",
	                            "2026-10-07T17:05:02.00Z", &s));
	CHECK(s.end - s.start == 65);

	/* System apps, dialogs, garbage: not games. */
	CHECK(!gwm_journal_parse_end("{\"appCategoryType\":\"ps5_common_dialog\",\"appTitleId\":\"NPXS40123\",\"totalFgTime\":3}",
	                             "2026-10-07T17:05:02.00Z", &s));
	CHECK(!gwm_journal_parse_end("{\"appCategoryType\":\"ps4_game\",\"appTitleId\":\"NPXS40123\",\"totalFgTime\":3}",
	                             "2026-10-07T17:05:02.00Z", &s));
	CHECK(!gwm_journal_parse_end("{\"appCategoryType\":\"ps4_game\",\"appTitleId\":\"CUSA0398\",\"totalFgTime\":3}",
	                             "2026-10-07T17:05:02.00Z", &s));
	CHECK(!gwm_journal_parse_end("{\"appCategoryType\":\"ps4_game\",\"appTitleId\":\"CUSA03980\"}",
	                             "2026-10-07T17:05:02.00Z", &s));

	/* Overlap allows two minutes of slack: the poll sees the exit late, and
	 * the journal clock offset is off by seconds. */
	CHECK(gwm_spans_overlap(100, 200, 150, 300));
	CHECK(gwm_spans_overlap(100, 200, 310, 400));
	CHECK(!gwm_spans_overlap(100, 200, 330, 400));

	gwm_spans_t l;
	memset(&l, 0, sizeof(l));
	gwm_spans_add(&l, "CUSA01127", "Fire", 1000, 1200);
	CHECK(gwm_spans_hit(&l, "CUSA01127", "Fire", 1039, 1189));
	CHECK(gwm_spans_hit(&l, "CUSA01127", "", 1039, 1189));        /* a line from before profiles */
	CHECK(!gwm_spans_hit(&l, "CUSA01127", "Mira", 1039, 1189));   /* the other player of a co-op game */
	CHECK(!gwm_spans_hit(&l, "CUSA03980", "Fire", 1039, 1189));
	CHECK(!gwm_spans_hit(&l, "CUSA01127", "Fire", 5000, 6000));
	gwm_spans_add(&l, "CUSA01127", "", 7000, 7100);
	CHECK(gwm_spans_hit(&l, "CUSA01127", "Mira", 7010, 7090));
	for(int i = 0; i < GWM_SPANS; i++) gwm_spans_add(&l, "CUSA03980", "Fire", 10000 + i, 10001 + i);
	CHECK(!gwm_spans_hit(&l, "CUSA01127", "Fire", 1039, 1189));
	CHECK(l.n == GWM_SPANS);

	/* The profiles a row names: user columns, then the header's hex ids,
	 * each once; -1 and the system's small ids are nobody. Rows from the
	 * console (Mira 0x12e6f46c started Burnout; Fire signed in later). */
	int users[GWM_PLAYERS_MAX];
	int64_t cols[4] = { 317125740, -1, -1, -1 };
	CHECK(gwm_journal_users(cols, "{\"localUserIds\":[\"12e6f46c\"],\"numberOfAccounts\":1}", users) == 1);
	CHECK(users[0] == 317125740);
	int64_t pair[4] = { 317125740, -1, -1, -1 };
	CHECK(gwm_journal_users(pair, "{\"localUserIds\":[\"12e6f46c\",\"12e6f46b\"]}", users) == 2);
	CHECK(users[1] == 317125739);
	int64_t none[4] = { -1, 254, 0, -1 };
	CHECK(gwm_journal_users(none, NULL, users) == 0);

	/* Played or left: Wind Waker started, suspended when Fire switched to
	 * Mira, never resumed; a stale End does not close a newer session. */
	gwm_jfg_t jf;
	memset(&jf, 0, sizeof(jf));
	int n_users;
	CHECK(gwm_jfg_playing(&jf, "PPSA99306", users, &n_users) == -1);
	int fire[1] = { 317125739 };
	gwm_jfg_start(&jf, "PPSA99306", "63931310000000000", fire, 1);
	CHECK(gwm_jfg_playing(&jf, "PPSA99306", users, &n_users) == 1 && n_users == 1 && users[0] == 317125739);
	gwm_jfg_end(&jf, "PPSA99306", "63931309999000000");
	CHECK(gwm_jfg_playing(&jf, "PPSA99306", users, &n_users) == 1);
	gwm_jfg_end(&jf, "PPSA99306", "63931310000000000");
	CHECK(gwm_jfg_playing(&jf, "PPSA99306", users, &n_users) == 0 && n_users == 0);
	gwm_jfg_start(&jf, "PPSA99306", "63931311000000000", fire, 1);
	CHECK(gwm_jfg_playing(&jf, "PPSA99306", users, &n_users) == 1);
	for(int i = 0; i < GWM_JFG_TITLES + 2; i++)
	{
		char id[16];
		snprintf(id, sizeof(id), "PPSA%05d", i);
		gwm_jfg_end(&jf, id, "");
	}
	CHECK(jf.n == GWM_JFG_TITLES);

	/* The offset is the largest recent reading: only a file change without a
	 * new timestamp makes it low. Readings from the console. */
	gwm_offsets_t o;
	memset(&o, 0, sizeof(o));
	CHECK(gwm_offsets_add(&o, 254) == 254);
	CHECK(gwm_offsets_add(&o, 203) == 254);
	CHECK(gwm_offsets_add(&o, 255) == 255);
	for(int i = 0; i < GWM_JOURNAL_OFFSETS; i++) gwm_offsets_add(&o, 100);
	CHECK(gwm_offsets_add(&o, 100) == 100);

	/* A deleted tail does not mean a recreated database; only a new
	 * generation resets the mark. */
	CHECK(gwm_journal_cursor(395658, "2026-10-10", "2026-10-10") == 395658);
	CHECK(gwm_journal_cursor(395658, "", "2026-10-10") == 395658);
	CHECK(gwm_journal_cursor(395658, "2026-10-10", "2026-10-28") == 0);

	memset(&o, 0, sizeof(o));
	CHECK(gwm_offsets_sample(&o, 1000, 742) == 258);
	for(int i = 1; i < 30; i++) CHECK(gwm_offsets_sample(&o, 1000, 742 + i * 10) == 258);
	CHECK(o.n == 1);
	CHECK(gwm_offsets_sample(&o, 1300, 1043) == 258);

	/* A queue line back to its game and bounds. */
	char line[GWM_RECORD_MAX], title_id[GWM_TITLE_ID_LEN], account[GWM_ACCOUNT_LEN];
	int64_t a, b;
	gwm_build_session(line, sizeof(line), "Player", "CUSA01127", "P.T.", "2026-10-07T21:35:18Z", "2026-10-07T21:37:48Z", 150);
	CHECK(gwm_session_line_span(line, title_id, sizeof(title_id), account, sizeof(account), &a, &b));
	CHECK_STR(title_id, "CUSA01127");
	CHECK_STR(account, "Player");
	CHECK(b - a == 150);
	CHECK(!gwm_session_line_span("{\"titleId\":\"CUSA01127\"}", title_id, sizeof(title_id), account, sizeof(account), &a, &b));

	/* The same P.T. seen by a payload launched mid-game and by the journal,
	 * whose clock is 253 s off: one session. */
	CHECK(gwm_journal_parse_end(log, "2026-10-07T21:42:01.00Z", &s));
	CHECK(gwm_spans_overlap(s.start - 253, s.end - 253, a, b));

	/* One raw appSessionId gives one sourceSessionId whatever the offset. */
	CHECK(gwm_journal_parse_end(log, "2026-10-07T21:42:01.00Z", &s));
	char record[GWM_RECORD_MAX], source[128];
	gwm_journal_record(record, sizeof(record), "Player", &s, 123, "P.T.",
	                   "2026-10-07T21:35:18Z", "2026-10-07T21:37:48Z");
	CHECK(gwm_json_get_str(record, "sourceSessionId", source, sizeof(source)));
	CHECK_STR(source, "ps5-journal:0000007b:CUSA01127:63927005932000000");

	/* An invalid raw id must not break a regular fallback session. */
	snprintf(s.app_session_id, sizeof(s.app_session_id), "%s", "invalid");
	gwm_journal_record(record, sizeof(record), "Player", &s, 123, "P.T.",
	                   "2026-10-07T21:35:18Z", "2026-10-07T21:37:48Z");
	CHECK(!gwm_json_get_str(record, "sourceSessionId", source, sizeof(source)));

	return gwm_report("journal");
}
