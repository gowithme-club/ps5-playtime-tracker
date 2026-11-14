/* Who is playing: the console user and the name used for account.
 *
 * account is the account key on the server and must match across heartbeat,
 * sessions and trophies, or one person becomes two. Order:
 *
 *   1. account from config.cfg, if set, for every user;
 *   2. the console profile name (sceUserServiceGetUserName);
 *   3. the user id in hex, the name of the user's folder in /user/home.
 *
 * The user service is unreliable from a payload. On 13.40
 * sceUserServiceInitialize never returns in a process started by elfldr,
 * and without Initialize every call returns 0x80960002. So Initialize and
 * the calls run in a separate thread with a timeout: if it hangs, the thread
 * is abandoned, the service is off for this run, and steps 1 and 3 apply.
 *
 * Without the service, the current user is the only folder in /user/home.
 * With several users there is no telling who plays; sessions then go to
 * the first one and the log gets a warning.
 */

#ifndef GWM_USER_H
#define GWM_USER_H

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "session.h"

#define GWM_USER_HOME   "/user/home"
#define GWM_USER_NONE   (-1)
#define GWM_USER_WAIT_S 3

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user_id);
int sceUserServiceGetUserName(int user_id, char *name, size_t size);

/* Whether the service responds; cleared after the first hang. */
static int gwm_usvc_alive = 1;

typedef struct
{
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int             done;
	int             user_id;   /* input for the name, output for foreground */
	int             rc;
	char            name[GWM_ACCOUNT_LEN];
	int             want_name;
} gwm_usvc_call_t;

static void *gwm_usvc_worker(void *arg)
{
	gwm_usvc_call_t *c = arg;
	int rc, uid = c->user_id;
	char name[GWM_ACCOUNT_LEN] = "";
	static int inited = 0;

	/* Without Initialize the service returns 0x80960002 (not initialized);
	 * measured on 13.40 under Payload Manager. Under elfldr Initialize hung;
	 * here it runs in a thread with a timeout, so a hang is harmless. */
	if(!inited)
	{
		int irc = sceUserServiceInitialize(NULL);
		inited = 1;
		if(irc) gwm_log("user service: Initialize -> 0x%08x", irc);
	}

	if(c->want_name) rc = sceUserServiceGetUserName(uid, name, sizeof(name));
	else             rc = sceUserServiceGetForegroundUser(&uid);

	pthread_mutex_lock(&c->lock);
	c->rc = rc;
	c->user_id = uid;
	snprintf(c->name, sizeof(c->name), "%s", name);
	c->done = 1;
	pthread_cond_signal(&c->cond);
	pthread_mutex_unlock(&c->lock);
	return NULL;
}

/* One service call with a timeout. The call struct lives on the heap and is
 * left to the abandoned thread on a hang, so it must not be freed then. */
static int gwm_usvc_call(int want_name, int *user_id, char *name, size_t size)
{
	if(!gwm_usvc_alive) return -1;

	gwm_usvc_call_t *c = calloc(1, sizeof(*c));
	if(!c) return -1;

	pthread_mutex_init(&c->lock, NULL);
	pthread_cond_init(&c->cond, NULL);
	c->want_name = want_name;
	c->user_id = *user_id;

	pthread_t th;
	if(pthread_create(&th, NULL, gwm_usvc_worker, c) != 0) { free(c); return -1; }
	pthread_detach(th);

	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += GWM_USER_WAIT_S;

	pthread_mutex_lock(&c->lock);
	while(!c->done)
		if(pthread_cond_timedwait(&c->cond, &c->lock, &until) != 0) break;
	int done = c->done, rc = c->rc;
	pthread_mutex_unlock(&c->lock);

	if(!done)
	{
		gwm_usvc_alive = 0;
		gwm_log("user service: no answer in %d s, continuing without it", GWM_USER_WAIT_S);
		return -1;
	}

	*user_id = c->user_id;
	if(name) snprintf(name, size, "%s", c->name);
	free(c);

	if(rc != 0) gwm_log("user service: %s -> 0x%08x", want_name ? "GetUserName" : "GetForegroundUser", rc);
	return rc;
}

/* Users from the /user/home folders (folder name is the id in hex). */
static int gwm_user_from_home(void)
{
	DIR *d = opendir(GWM_USER_HOME);
	struct dirent *e;
	int found = GWM_USER_NONE, count = 0;

	if(!d) return GWM_USER_NONE;

	while((e = readdir(d)) != NULL)
	{
		char *end = NULL;
		if(e->d_name[0] == '.') continue;
		unsigned long id = strtoul(e->d_name, &end, 16);
		if(!end || *end || !id) continue;
		if(found == GWM_USER_NONE) found = (int)id;
		count++;
	}
	closedir(d);

	if(count > 1)
	{
		static int warned = 0;
		if(!warned++) gwm_log("%d users in /user/home and no user service: using 0x%08x", count, found);
	}
	return found;
}

static int gwm_user_foreground(void)
{
	int uid = GWM_USER_NONE;

	if(gwm_usvc_call(0, &uid, NULL, 0) == 0 && uid != GWM_USER_NONE && uid != 0)
		return uid;

	return gwm_user_from_home();
}

/* Name cache: a profile name does not change mid-run, and there is no need
 * to ask the service on every poll. */
#define GWM_USER_CACHE 8

static struct { int uid; char name[GWM_ACCOUNT_LEN]; } gwm_user_cache[GWM_USER_CACHE];
static int gwm_user_cached = 0;

/* account for data of user uid. */
static void gwm_user_account(const char *cfg_account, int uid, char *out, size_t size)
{
	if(cfg_account && cfg_account[0]) { snprintf(out, size, "%s", cfg_account); return; }

	for(int i = 0; i < gwm_user_cached; i++)
		if(gwm_user_cache[i].uid == uid) { snprintf(out, size, "%s", gwm_user_cache[i].name); return; }

	char name[GWM_ACCOUNT_LEN] = "";
	int id = uid;

	if(uid == GWM_USER_NONE || gwm_usvc_call(1, &id, name, sizeof(name)) != 0 || !name[0])
		snprintf(name, sizeof(name), "%08x", (unsigned)uid);

	/* The fallback is cached too: otherwise every call would wait for the
	 * timeout again, and the name must not change within a run anyway. */
	if(gwm_user_cached < GWM_USER_CACHE)
	{
		gwm_user_cache[gwm_user_cached].uid = uid;
		snprintf(gwm_user_cache[gwm_user_cached].name, GWM_ACCOUNT_LEN, "%s", name);
		gwm_user_cached++;
		gwm_log("user 0x%08x -> account \"%s\"", (unsigned)uid, name);
	}

	snprintf(out, size, "%s", name);
}

#endif /* GWM_USER_H */
