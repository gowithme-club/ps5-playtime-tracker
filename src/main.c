/* gowithme.club playtime tracker for a jailbroken PS5.
 *
 * A single-threaded loop every 10 seconds:
 *   - what is running -> session state machine -> closed sessions wait for
 *     the journal;
 *   - new sessions from the telemetry journal -> queue (journal.h); poll
 *     sessions only when the journal never showed them;
 *   - new trophy notifications -> trophy queue;
 *   - changed PS5 trophy files -> full set state into the same queue
 *     (trophy2.h);
 *   - once a minute (and on every change) a heartbeat, then the queues.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "clock.h"
#include "config.h"
#include "game.h"
#include "http.h"
#include "instance.h"
#include "journal.h"
#include "log.h"
#include "notif.h"
#include "pair.h"
#include "queue.h"
#include "session.h"
#include "state.h"
#include "tile.h"
#include "trophy2.h"
#include "upload.h"
#include "user.h"
#include "version.h"
#include "web.h"

#define GWM_STATUS_FILE GWM_DATA_DIR "/status.txt"

#define GWM_POLL_SEC         10
#define GWM_HEARTBEAT_SEC    60
/* The config is reread when the file changes, at most this often. */
#define GWM_CONFIG_CHECK_SEC 30

static gwm_config_t  gwm_cfg;
static gwm_session_t gwm_sess;
/* The other players of the game in gwm_sess, each from when they came in. */
#define GWM_COOP_MAX (GWM_PLAYERS_MAX - 1)
static gwm_session_t gwm_coop[GWM_COOP_MAX];
static gwm_state_t   gwm_state;

static int  gwm_last_code = 0;
static int  gwm_connected_once = 0;
/* The start notification waits for the first exchange with the server, so
 * one notification replaces "started" followed by "connected". */
static int  gwm_announced = 0;
static time_t gwm_cfg_mtime = 0;
static gwm_pair_t gwm_pair;
/* Raw time of the server's last 200, for the console page. */
static int64_t gwm_last_ok = 0;
/* The welcome screen was read, or the console was linked once: the page no
 * longer opens on it (web.h). */
static int gwm_welcome_seen = 0;

/* The profile signed in on the console, which the heartbeat names (record.h),
 * and the one the last answer was about (gwm_profile, upload.h). */
static char gwm_foreground[GWM_ACCOUNT_LEN] = "";
static char gwm_profile_for[GWM_ACCOUNT_LEN] = "";
/* Profiles told once this run that they are nobody's on the site yet. */
#define GWM_REMINDED_MAX 8
static char gwm_reminded[GWM_REMINDED_MAX][GWM_ACCOUNT_LEN];
static int  gwm_reminded_n = 0;

static void gwm_welcome_mark(void)
{
	FILE *f = fopen(GWM_WELCOME_FILE, "w");
	if(f) fclose(f);
	gwm_welcome_seen = 1;
}

/* On the PS5 the main thread's name is also the process name (ki_comm),
 * which sets us apart from all the other payload.elf. The .elf suffix is
 * required: Payload Manager treats only processes named *.elf as payloads
 * and hides the rest as system ones. MAXCOMLEN is 19 characters. */
static void gwm_name_self(void)
{
#ifdef SYS_thr_set_name
	syscall(SYS_thr_set_name, -1, GWM_PRODUCT ".elf");
#endif
}

static void gwm_config_refresh(int announce)
{
	struct stat st;
	time_t mtime = (stat(GWM_CONFIG_FILE, &st) == 0) ? st.st_mtime : 0;

	if(!announce && mtime == gwm_cfg_mtime) return;
	gwm_cfg_mtime = mtime;

	gwm_config_load(&gwm_cfg, GWM_CONFIG_FILE);
	gwm_popups = gwm_cfg.popups;
	if(gwm_cfg.valid && !gwm_welcome_seen) gwm_welcome_mark();

	gwm_log("config: %s, host=%s:%u, account=%s", gwm_cfg.valid ? "token set" : "no token, pairing by code",
	        gwm_cfg.host, (unsigned)gwm_cfg.port, gwm_cfg.account[0] ? gwm_cfg.account : "(profile)");
}

/* No token: one pairing step (pair.h). The profile reported to the site is
 * whoever is at the console now. */
static void gwm_pair_step(void)
{
	static char profile[GWM_ACCOUNT_LEN] = "";

	if(!profile[0]) gwm_user_account(gwm_cfg.account, gwm_user_foreground(), profile, sizeof(profile));

	if(gwm_pair_tick(&gwm_pair, &gwm_cfg, profile, gwm_tile_shown))
	{
		gwm_config_refresh(1);
		gwm_connected_once = 1;
		if(profile[0]) gwm_toast(GWM_TR("Console linked\n%s", "Консоль привязана\n%s"), profile);
		else gwm_toast("%s", GWM_TR("Console linked", "Консоль привязана"));
		return;
	}

	/* Without a key, the code notification announces the start. No code
	 * means the server is unreachable, which is worth saying right away. */
	if(gwm_announced) return;
	if(gwm_pair.active) gwm_announced = 1;
	else if(gwm_pair.next_start > gwm_now_raw())
	{
		gwm_announced = 1;
		gwm_toast("%s", GWM_TR("Can't get a code\nNo connection to " GWM_PRODUCT,
		                       "Код не получен\nНет связи с " GWM_PRODUCT));
	}
}

/* What the console page shows (web.h). */
static void gwm_web_refresh(void)
{
	gwm_web_view_t v;
	int64_t now = gwm_now_raw();

	memset(&v, 0, sizeof(v));
	const char *state;

	if(gwm_cfg.valid)                  state = gwm_profile.unclaimed ? "claim" : "linked";
	else if(gwm_pair.done)             state = "stopped";
	else if(gwm_pair.active)           state = "code";
	else if(gwm_pair.next_start > now) state = "offline";
	else                               state = "connecting";

	snprintf(v.state, sizeof(v.state), "%s", state);
	if(!gwm_cfg.valid && gwm_pair.active) snprintf(v.code, sizeof(v.code), "%s", gwm_pair.code);
	if(gwm_cfg.valid && gwm_profile.unclaimed)
	{
		snprintf(v.code, sizeof(v.code), "%s", gwm_profile.code);
		snprintf(v.profile, sizeof(v.profile), "%s", gwm_profile_for);
	}
	v.sync_ago = gwm_last_ok ? now - gwm_last_ok : -1;
	if(v.sync_ago < -1) v.sync_ago = 0;
	v.queued = gwm_file_size(GWM_SESSION_QUEUE) + gwm_file_size(GWM_TROPHY_QUEUE);
	if(v.queued < 0) v.queued = 0;
	/* A profile shows only while the console is linked: after an unlink the
	 * address names someone the console no longer reports to. */
	if(gwm_cfg.valid) snprintf(v.profile_url, sizeof(v.profile_url), "%s", gwm_profile.url);
	v.rec_seconds = v.rec_games = v.rec_trophies = -1;
	if(gwm_cfg.valid)
	{
		v.rec_seconds = gwm_recorded.seconds;
		v.rec_games = gwm_recorded.games;
		v.rec_trophies = gwm_recorded.trophies;
	}
	v.welcome = !gwm_cfg.valid && !gwm_welcome_seen;
	if(gwm_version_newer(gwm_latest, GWM_VERSION)) snprintf(v.latest, sizeof(v.latest), "%s", gwm_latest);
	gwm_web_publish(&v);
}

static void gwm_write_status(void)
{
	char iso[GWM_ISO_LEN], started[GWM_ISO_LEN];
	FILE *f = fopen(GWM_STATUS_FILE ".tmp", "w");

	if(!f) return;

	gwm_iso_from_unix(iso, gwm_now_raw());
	gwm_iso_from_unix(started, gwm_sess.start);

	fprintf(f, "version = %s\n", GWM_VERSION);
	fprintf(f, "updated = %s\n", iso);
	fprintf(f, "token = %s\n", gwm_cfg.valid ? "yes" : "no");
	fprintf(f, "playing = %s\n", gwm_sess.active ? gwm_sess.title_id : "");
	fprintf(f, "title = %s\n", gwm_sess.active ? gwm_sess.title : "");
	fprintf(f, "account = %s", gwm_sess.active ? gwm_sess.account : "");
	for(int i = 0; i < GWM_COOP_MAX; i++) if(gwm_coop[i].active) fprintf(f, ", %s", gwm_coop[i].account);
	fprintf(f, "\n");
	fprintf(f, "signed_in = %s%s\n", gwm_foreground, gwm_profile.unclaimed ? " (not linked)" : "");
	fprintf(f, "started = %s\n", gwm_sess.active ? started : "");
	fprintf(f, "last_http = %d\n", gwm_last_code);
	fprintf(f, "clock_offset = %lld\n", (long long)gwm_clock_offset);
	fprintf(f, "clock_synced = %d\n", gwm_clock_synced);
	fprintf(f, "queue_sessions = %ld\n", gwm_file_size(GWM_SESSION_QUEUE));
	fprintf(f, "queue_trophies = %ld\n", gwm_file_size(GWM_TROPHY_QUEUE));
	fprintf(f, "notif_last_id = %lld\n", (long long)gwm_state.notif_last_id);
	fprintf(f, "journal_last_row = %lld\n", (long long)gwm_state.journal_last_row);
	fprintf(f, "journal_offset = %lld\n", gwm_journal_offset_known ? (long long)gwm_journal_offset : 0LL);
	fprintf(f, "queue_pending = %ld\n", gwm_file_size(GWM_PENDING_FILE));
	fprintf(f, "trophy2_sets = %d\n", gwm_t2_seen_n < 0 ? 0 : gwm_t2_seen_n);
	fclose(f);

	rename(GWM_STATUS_FILE ".tmp", GWM_STATUS_FILE);
	gwm_web_refresh();
}

#define GWM_SYSTEM_PARAM_LANG 1

int sceSystemServiceParamGetInt(int id, int *value);

/* The language of the notifications (lang.h); the last one stays when the
 * system does not answer. */
static void gwm_lang_detect(void)
{
	static int last = -1;
	int code = -1;

	if(sceSystemServiceParamGetInt(GWM_SYSTEM_PARAM_LANG, &code) != 0) return;
	gwm_lang = gwm_lang_of(code);
	if(code != last) gwm_log("language: %d (%s)", code, gwm_lang == GWM_LANG_RU ? "ru" : "en");
	last = code;
}

/* The profile signed in on the console: the one config.cfg names for every
 * user, or the service's answer. Nothing when the service does not answer:
 * the page must not name the wrong person. */
static void gwm_foreground_detect(void)
{
	char name[GWM_ACCOUNT_LEN] = "";

	if(gwm_cfg.account[0]) snprintf(name, sizeof(name), "%s", gwm_cfg.account);
	else
	{
		int uid = gwm_user_signed_in();
		if(uid != GWM_USER_NONE) gwm_user_account(NULL, uid, name, sizeof(name));
	}
	if(strcmp(name, gwm_foreground)) gwm_log("signed in: %s", name[0] ? name : "(unknown)");
	snprintf(gwm_foreground, sizeof(gwm_foreground), "%s", name);
}

static int gwm_reminded_once(const char *profile)
{
	for(int i = 0; i < gwm_reminded_n; i++)
		if(!strcmp(gwm_reminded[i], profile)) return 0;
	if(gwm_reminded_n < GWM_REMINDED_MAX)
		snprintf(gwm_reminded[gwm_reminded_n++], GWM_ACCOUNT_LEN, "%s", profile);
	return 1;
}

/* What the answer changed for the profile signed in: claimed on the site
 * just now (by the code on the page, or by the owner), or found to be
 * nobody's, which its person hears once a run unless the page is in front
 * of them already. */
static void gwm_profile_news(const gwm_profile_t *before, int same_profile)
{
	if(same_profile && before->unclaimed && !gwm_profile.unclaimed && gwm_profile_for[0])
		gwm_toast(GWM_TR("Profile linked\n%s", "Профиль привязан\n%s"), gwm_profile_for);
	if(gwm_profile.unclaimed && gwm_profile_for[0] && gwm_reminded_once(gwm_profile_for) && !gwm_web_open())
		gwm_toast("%s", GWM_TR("Link your profile\nOpen " GWM_PRODUCT, "Привяжите свой профиль\nОткройте " GWM_PRODUCT));
}

/* A newer release, said once per version: the one said last is kept in
 * GWM_UPDATE_FILE across runs. The tile's footer keeps showing it. */
#define GWM_UPDATE_FILE GWM_DATA_DIR "/update.txt"

static void gwm_update_news(void)
{
	char said[16] = "";
	FILE *f;

	if(!gwm_version_newer(gwm_latest, GWM_VERSION)) return;
	if((f = fopen(GWM_UPDATE_FILE, "r")))
	{
		if(!fgets(said, sizeof(said), f)) said[0] = 0;
		said[strcspn(said, "\n")] = 0;
		fclose(f);
	}
	if(!strcmp(said, gwm_latest)) return;

	gwm_log("update: %s is out, this is %s", gwm_latest, GWM_VERSION);
	gwm_toast(GWM_TR("Version %s is out", "Вышла версия %s"), gwm_latest);
	if((f = fopen(GWM_UPDATE_FILE, "w")))
	{
		fprintf(f, "%s\n", gwm_latest);
		fclose(f);
	}
}

/* One network pass: a heartbeat and, if the server answered, the queues. */
static void gwm_net_pass(void)
{
	int was_synced = gwm_clock_synced;
	gwm_lang_detect();
	gwm_foreground_detect();

	gwm_profile_t before = gwm_profile;
	int same_profile = !strcmp(gwm_profile_for, gwm_foreground);
	int code = gwm_send_heartbeat(&gwm_cfg, &gwm_sess, gwm_coop, GWM_COOP_MAX, gwm_foreground, gwm_web_open());

	/* The first response brought the clock offset, but this heartbeat went
	 * out without it, with startedAt off by the clock error. If the error is
	 * noticeable, resend right away. */
	if(code == 200 && !was_synced && gwm_clock_synced &&
	   (gwm_clock_offset > 30 || gwm_clock_offset < -30))
		code = gwm_send_heartbeat(&gwm_cfg, &gwm_sess, gwm_coop, GWM_COOP_MAX, gwm_foreground, gwm_web_open());
	if(code == 200) snprintf(gwm_profile_for, sizeof(gwm_profile_for), "%s", gwm_foreground);

	if(code != gwm_last_code) gwm_log("heartbeat -> %d (clock offset %lld s)", code, (long long)gwm_clock_offset);
	gwm_last_code = code;

	/* The server does not know the key: the connection was removed on the
	 * site or the console was linked again. The key is erased; the queues are
	 * left alone. No code follows: removing the console on the site is how
	 * people unlink it, and a code repeating until the next reboot would only
	 * be noise. Loading the payload again pairs it anew. */
	if(code == 401)
	{
		gwm_log("server rejected the key, pairing on the next start");
		gwm_config_save_token(GWM_CONFIG_FILE, "");
		gwm_config_refresh(1);
		gwm_connected_once = 0;
		gwm_announced = 1;
		gwm_pair.done = 1;
		gwm_toast("%s", GWM_TR("Console unlinked", "Консоль отвязана"));
		return;
	}

	if(!gwm_announced)
	{
		gwm_announced = 1;
		gwm_connected_once = (code == 200);
		gwm_toast("%s", code == 200 ? GWM_TR("Tracker started", "Трекер запущен")
		                            : GWM_TR("Tracker started\nNo connection to " GWM_PRODUCT,
		                                     "Трекер запущен\nНет связи с " GWM_PRODUCT));
	}

	if(code != 200) return;
	gwm_last_ok = gwm_now_raw();

	if(!gwm_connected_once)
	{
		gwm_connected_once = 1;
		gwm_toast("%s", GWM_TR("Connected", "Связь восстановлена"));
	}

	/* After "started" or "connected", which say the tracker is there at all. */
	gwm_profile_news(&before, same_profile);
	gwm_update_news();

	/* Queues go out only once the clock offset is known: otherwise
	 * timestamps would leave raw, and a resend with different ones is a
	 * duplicate. */
	if(!gwm_clock_synced) return;

	gwm_upload_sessions(&gwm_cfg);
	gwm_upload_trophies(&gwm_cfg);
}

/* Everyone else signed in while a game is played: co-op players, each with
 * a session of their own. The journal names only who started the game, and
 * switching users signs the last one out, so another profile signed in is
 * another player (user.h). One profile for all (config.cfg) has no others. */
static void gwm_coop_update(int64_t now, const char *title_id, const char *title, int *heartbeat_due)
{
	char names[GWM_COOP_MAX][GWM_ACCOUNT_LEN], line[GWM_RECORD_MAX];
	int n = 0, changed;

	if(title_id[0] && gwm_sess.active && !gwm_cfg.account[0])
	{
		int ids[GWM_PLAYERS_MAX], k = gwm_user_signed_in_all(ids);
		for(int i = 0; i < k && n < GWM_COOP_MAX; i++)
		{
			char name[GWM_ACCOUNT_LEN];
			gwm_user_account(NULL, ids[i], name, sizeof(name));
			if(strcmp(name, gwm_sess.account)) snprintf(names[n++], GWM_ACCOUNT_LEN, "%s", name);
		}
	}

	/* Who left, or a game that changed under them. */
	for(int i = 0; i < GWM_COOP_MAX; i++)
	{
		gwm_session_t *c = &gwm_coop[i];
		char account[GWM_ACCOUNT_LEN];
		int stays = 0;

		if(!c->active) continue;
		for(int j = 0; j < n; j++) if(!strcmp(names[j], c->account)) stays = 1;
		snprintf(account, sizeof(account), "%s", c->account);
		size_t len = gwm_session_update(c, now, stays ? title_id : "", title, account, line, sizeof(line), &changed);
		if(len) gwm_pending_add(line, len);
		if(changed)
		{
			if(!c->active) gwm_log("co-op: %s left", account);
			*heartbeat_due = 1;
		}
	}

	/* Who came in. */
	for(int j = 0; j < n; j++)
	{
		int have = 0, slot = -1;
		for(int i = 0; i < GWM_COOP_MAX; i++)
		{
			if(gwm_coop[i].active && !strcmp(gwm_coop[i].account, names[j])) have = 1;
			else if(!gwm_coop[i].active && slot < 0) slot = i;
		}
		if(have || slot < 0) continue;
		gwm_session_update(&gwm_coop[slot], now, title_id, title, names[j], line, sizeof(line), &changed);
		gwm_log("co-op: %s plays %s too", names[j], title_id);
		*heartbeat_due = 1;
	}
}

static void gwm_poll(int *heartbeat_due)
{
	char title_id[GWM_TITLE_ID_LEN], title[GWM_TITLE_LEN] = "", account[GWM_ACCOUNT_LEN] = "";
	char line[GWM_RECORD_MAX];
	int changed = 0;
	int64_t now = gwm_now_raw();
	static char left[GWM_TITLE_ID_LEN] = "";

	int running = gwm_game_detect(title_id);

	/* The journal first: it says whether the loaded game is played, and
	 * who started it (journal.h). */
	if(gwm_journal_poll(&gwm_state, gwm_cfg.account, now, running ? title_id : NULL) > 0) *heartbeat_due = 1;

	int users[GWM_PLAYERS_MAX], n_users = 0;
	int played = running ? gwm_journal_playing(title_id, users, &n_users) : -1;
	int playing = running && played != 0;

	/* Left for the home screen, another game or another user: still loaded,
	 * nobody playing it. */
	if(running && !playing && strcmp(left, title_id))
		gwm_log("%s loaded, not played: the journal ended its session", title_id);
	snprintf(left, sizeof(left), "%s", running && !playing ? title_id : "");

	/* Title and owner are looked up only when a session starts: that is an
	 * app.db read and a user service call, not something for every poll. The
	 * owner is who started the game by the journal, else the profile open. */
	if(playing && !(gwm_sess.active && !strcmp(gwm_sess.title_id, title_id)))
	{
		if(!gwm_game_title(title_id, title, sizeof(title)))
			gwm_log("no title for %s in app.db", title_id);
		gwm_user_account(gwm_cfg.account, played == 1 && n_users ? users[0] : gwm_user_foreground(),
		                 account, sizeof(account));
	}
	else if(playing)
	{
		snprintf(title, sizeof(title), "%s", gwm_sess.title);
		snprintf(account, sizeof(account), "%s", gwm_sess.account);
	}

	size_t len = gwm_session_update(&gwm_sess, now, playing ? title_id : "", title, account,
	                                line, sizeof(line), &changed);
	if(len) gwm_pending_add(line, len);

	if(changed)
	{
		gwm_log(gwm_sess.active ? "playing %s \"%s\" (%s)" : "no game%s%s%s",
		        gwm_sess.active ? gwm_sess.title_id : "",
		        gwm_sess.active ? gwm_sess.title : "",
		        gwm_sess.active ? gwm_sess.account : "");
		*heartbeat_due = 1;
	}

	gwm_coop_update(now, gwm_sess.active ? gwm_sess.title_id : "", gwm_sess.title, heartbeat_due);

	if(gwm_notif_poll(&gwm_state, gwm_cfg.account) > 0) *heartbeat_due = 1;
	if(gwm_trophy2_poll(gwm_cfg.account, now) > 0) *heartbeat_due = 1;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IONBF, 0);

	mkdir(GWM_DATA_DIR, 0777);
	gwm_name_self();

	gwm_log(GWM_PRODUCT_VERSION ", pid %d", (int)getpid());

	if(!gwm_instance_acquire())
	{
		gwm_log("old copy did not exit in %d s, quitting", GWM_TAKEOVER_WAIT_S);
		return 1;
	}

	gwm_config_refresh(1);
	gwm_state_load(&gwm_state);
	gwm_session_init(&gwm_sess);
	for(int i = 0; i < GWM_COOP_MAX; i++) gwm_session_init(&gwm_coop[i]);

	gwm_lang_detect();
	struct stat welcome;
	if(stat(GWM_WELCOME_FILE, &welcome) == 0) gwm_welcome_seen = 1;

	gwm_icon_write();
	/* The page's first answer already knows whether to welcome. */
	gwm_web_refresh();
	gwm_web_start();
	gwm_tile_start(GWM_WEB_PORT);

	int64_t next_heartbeat = 0, next_config = 0;
	int heartbeat_due = 1;

	for(;;)
	{
		gwm_poll(&heartbeat_due);

		int64_t now = gwm_now_raw();

		if(now >= next_config)
		{
			gwm_config_refresh(0);
			next_config = now + GWM_CONFIG_CHECK_SEC;
		}

		/* A code on the page: every pass, so a claim on the phone shows
		 * within seconds. */
		if(gwm_cfg.valid && gwm_profile.unclaimed && gwm_web_open()) heartbeat_due = 1;

		if(gwm_cfg.valid && (heartbeat_due || now >= next_heartbeat || now < next_heartbeat - GWM_HEARTBEAT_SEC))
		{
			gwm_net_pass();
			heartbeat_due = 0;
			next_heartbeat = gwm_now_raw() + GWM_HEARTBEAT_SEC;
		}

		gwm_write_status();

		for(int i = 0; i < GWM_POLL_SEC; i++)
		{
			/* "Show a code" on the console page: a new pairing window after
			 * the last one ran out or the console was unlinked. */
			if(gwm_web_pair_wanted() && !gwm_cfg.valid && gwm_pair.done)
			{
				gwm_log("pairing: new code asked for on the console page");
				memset(&gwm_pair, 0, sizeof(gwm_pair));
			}

			/* The page was just opened: ask the server now who is signed
			 * in, rather than show the last answer for up to a minute. */
			if(gwm_web_opened() && gwm_cfg.valid)
			{
				heartbeat_due = 1;
				break;
			}

			if(gwm_web_welcome_read() && !gwm_welcome_seen)
			{
				gwm_log("page: welcome read");
				gwm_welcome_mark();
				gwm_web_refresh();
			}

			if(!gwm_cfg.valid)
			{
				gwm_pair_step();
				gwm_web_refresh();
			}

			if(gwm_instance_stop_requested())
			{
				/* Close the running session now; the new copy opens its own
				 * on its first poll. Both halves wait for the journal, and if
				 * it is being written, one whole session goes from there. */
				char line[GWM_RECORD_MAX];
				size_t len = gwm_session_close(&gwm_sess, gwm_now_raw(), line, sizeof(line));
				if(len) gwm_pending_add(line, len);
				for(int c = 0; c < GWM_COOP_MAX; c++)
				{
					len = gwm_session_close(&gwm_coop[c], gwm_now_raw(), line, sizeof(line));
					if(len) gwm_pending_add(line, len);
				}

				gwm_log("a new copy asked to take over, exiting");
				return 0;
			}
			sleep(1);
		}
	}
}
