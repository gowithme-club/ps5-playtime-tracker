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
#include "trophy2.h"
#include "upload.h"
#include "user.h"
#include "version.h"

#define GWM_STATUS_FILE GWM_DATA_DIR "/status.txt"

#define GWM_POLL_SEC         10
#define GWM_HEARTBEAT_SEC    60
/* The config is reread when the file changes, at most this often. */
#define GWM_CONFIG_CHECK_SEC 30

static gwm_config_t  gwm_cfg;
static gwm_session_t gwm_sess;
static gwm_state_t   gwm_state;

static int  gwm_last_code = 0;
static int  gwm_connected_once = 0;
/* The start notification waits for the first exchange with the server, so
 * one notification replaces "started" followed by "connected". */
static int  gwm_announced = 0;
static time_t gwm_cfg_mtime = 0;
static gwm_pair_t gwm_pair;

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

	gwm_log("config: %s, host=%s:%u, account=%s", gwm_cfg.valid ? "token set" : "no token, pairing by code",
	        gwm_cfg.host, (unsigned)gwm_cfg.port, gwm_cfg.account[0] ? gwm_cfg.account : "(profile)");
}

/* No token: one pairing step (pair.h). The profile reported to the site is
 * whoever is at the console now. */
static void gwm_pair_step(void)
{
	static char profile[GWM_ACCOUNT_LEN] = "";

	if(!profile[0]) gwm_user_account(gwm_cfg.account, gwm_user_foreground(), profile, sizeof(profile));

	if(gwm_pair_tick(&gwm_pair, &gwm_cfg, profile))
	{
		gwm_config_refresh(1);
		gwm_connected_once = 1;
		gwm_toast(GWM_PRODUCT ": %s linked", profile);
		return;
	}

	/* Without a key, the code notification announces the start. No code
	 * means the server is unreachable, which is worth saying right away. */
	if(gwm_announced) return;
	if(gwm_pair.active) gwm_announced = 1;
	else if(gwm_pair.next_start > gwm_now_raw())
	{
		gwm_announced = 1;
		gwm_toast(GWM_PRODUCT ": tracker started, server unreachable");
	}
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
	fprintf(f, "account = %s\n", gwm_sess.active ? gwm_sess.account : "");
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
}

/* One network pass: a heartbeat and, if the server answered, the queues. */
static void gwm_net_pass(void)
{
	int was_synced = gwm_clock_synced;
	int code = gwm_send_heartbeat(&gwm_cfg, &gwm_sess);

	/* The first response brought the clock offset, but this heartbeat went
	 * out without it, with startedAt off by the clock error. If the error is
	 * noticeable, resend right away. */
	if(code == 200 && !was_synced && gwm_clock_synced &&
	   (gwm_clock_offset > 30 || gwm_clock_offset < -30))
		code = gwm_send_heartbeat(&gwm_cfg, &gwm_sess);

	if(code != gwm_last_code) gwm_log("heartbeat -> %d (clock offset %lld s)", code, (long long)gwm_clock_offset);
	gwm_last_code = code;

	/* The server does not know the key: the connection was removed on the
	 * site or the console was linked again. The key is erased and pairing
	 * starts over; the queues are left alone. */
	if(code == 401)
	{
		gwm_log("server rejected the key, pairing again");
		gwm_config_save_token(GWM_CONFIG_FILE, "");
		gwm_config_refresh(1);
		gwm_connected_once = 0;
		gwm_announced = 1;
		gwm_toast(GWM_PRODUCT ": console unlinked, showing a new code");
		return;
	}

	if(!gwm_announced)
	{
		gwm_announced = 1;
		gwm_connected_once = (code == 200);
		gwm_toast(code == 200 ? GWM_PRODUCT ": tracker started" : GWM_PRODUCT ": tracker started, server unreachable");
	}

	if(code != 200) return;

	if(!gwm_connected_once)
	{
		gwm_connected_once = 1;
		gwm_toast(GWM_PRODUCT ": connected");
	}

	/* Queues go out only once the clock offset is known: otherwise
	 * timestamps would leave raw, and a resend with different ones is a
	 * duplicate. */
	if(!gwm_clock_synced) return;

	gwm_upload_sessions(&gwm_cfg);
	gwm_upload_trophies(&gwm_cfg);
}

static void gwm_poll(int *heartbeat_due)
{
	char title_id[GWM_TITLE_ID_LEN], title[GWM_TITLE_LEN] = "", account[GWM_ACCOUNT_LEN] = "";
	char line[GWM_RECORD_MAX];
	int changed = 0;
	int64_t now = gwm_now_raw();

	int playing = gwm_game_detect(title_id);

	/* Title and owner are looked up only when a session starts: that is an
	 * app.db read and a user service call, not something for every poll. */
	if(playing && !(gwm_sess.active && !strcmp(gwm_sess.title_id, title_id)))
	{
		if(!gwm_game_title(title_id, title, sizeof(title)))
			gwm_log("no title for %s in app.db", title_id);
		gwm_user_account(gwm_cfg.account, gwm_user_foreground(), account, sizeof(account));
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

	if(gwm_notif_poll(&gwm_state, gwm_cfg.account) > 0) *heartbeat_due = 1;
	if(gwm_journal_poll(&gwm_state, gwm_cfg.account, now) > 0) *heartbeat_due = 1;
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

		if(gwm_cfg.valid && (heartbeat_due || now >= next_heartbeat || now < next_heartbeat - GWM_HEARTBEAT_SEC))
		{
			gwm_net_pass();
			heartbeat_due = 0;
			next_heartbeat = gwm_now_raw() + GWM_HEARTBEAT_SEC;
		}

		gwm_write_status();

		for(int i = 0; i < GWM_POLL_SEC; i++)
		{
			if(!gwm_cfg.valid) gwm_pair_step();

			if(gwm_instance_stop_requested())
			{
				/* Close the running session now; the new copy opens its own
				 * on its first poll. Both halves wait for the journal, and if
				 * it is being written, one whole session goes from there. */
				char line[GWM_RECORD_MAX];
				size_t len = gwm_session_close(&gwm_sess, gwm_now_raw(), line, sizeof(line));
				if(len) gwm_pending_add(line, len);

				gwm_log("a new copy asked to take over, exiting");
				return 0;
			}
			sleep(1);
		}
	}
}
