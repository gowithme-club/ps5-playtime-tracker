/* The page the tile opens: http://127.0.0.1:GWM_WEB_PORT/ (tile.h).
 *
 * For someone who has only the television in front of them: not linked yet,
 * it shows the code and a QR code for gowithme.club/ps5?code=..., so a phone
 * camera does the typing; linked, it says when the console last reported and
 * shows the profile of whoever is signed in on the console as a link and a QR
 * code (profileUrl, which the server names in the heartbeat answer). When
 * that profile is nobody's on the site yet, someone other than the owner is
 * at the console: the page asks them to claim it with a code ("claim"), in the
 * same way and at the same address as linking the console.
 *
 *   GET  /                 assets/page.html, which draws everything from /state
 *   GET  /icon.png         the tile icon, for the page header
 *   GET  /state?lang=en|ru the current state as JSON
 *   POST /pair             a new pairing window once the old one ran out (pair.h)
 *   POST /welcome          the welcome screen was read; it is not shown again
 *
 * Until the console is linked, the page opens on a welcome screen that says
 * what the service does, once: its button sends /welcome, and the main loop
 * keeps the answer in GWM_WELCOME_FILE across runs.
 *
 * The site picks its language from the address, and only / and /ps5 follow
 * the browser's. The page passes the console's language, so the profile link
 * and its QR code open the version the person reads.
 *
 * The server listens on 127.0.0.1 only: the page is for the console's own
 * browser and nothing is exposed to the network. /state carries the pairing
 * code, and a page elsewhere could reach 127.0.0.1 by a domain that resolves
 * there (DNS rebinding), so a request is served only when its Host is
 * 127.0.0.1 or localhost.
 *
 * It runs in its own thread with one connection at a time. The main loop
 * publishes a copy of what the page shows (gwm_web_publish) and reads the
 * page's two requests (gwm_web_pair_wanted, gwm_web_welcome_read) and whether
 * it is open (gwm_web_opened, gwm_web_open: the page asks for /state every
 * 3 s); the thread copies that view under a lock and builds the JSON from the
 * copy, so the loop stays the single owner of everything else.
 *
 * Request parsing, the Host check, the language and the state JSON are pure,
 * covered by a host test.
 */

#ifndef GWM_WEB_H
#define GWM_WEB_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "json.h"
#include "qrcodegen.h"
#include "version.h"

#define GWM_WEB_STATE_MAX 8192
#define GWM_WEB_REQ_MAX 2048
/* Up to version 10 at medium correction: 213 bytes, a profile address with
 * room to spare. */
#define GWM_WEB_QR_MAX_VERSION 10

/* What the QR code opens: the pairing page with the code filled in. The site
 * fills it in but never sends it by itself. */
#define GWM_WEB_QR_PREFIX "https://gowithme.club/ps5?code="

/* Present once the welcome screen was read. */
#define GWM_WELCOME_FILE GWM_DATA_DIR "/welcome.txt"

/* Method, path, query and Host from a request head. Returns 1 once the head
 * is complete (an empty line), 0 if more is needed or it is malformed. */
static int gwm_web_parse(const char *req, size_t len, char *method, size_t msize,
                         char *path, size_t psize, char *query, size_t qsize,
                         char *host, size_t hsize)
{
	method[0] = path[0] = query[0] = host[0] = 0;

	const char *head_end = NULL;
	for(size_t i = 0; i + 3 < len; i++)
		if(!memcmp(req + i, "\r\n\r\n", 4)) { head_end = req + i; break; }
	if(!head_end) return 0;

	const char *sp1 = memchr(req, ' ', (size_t)(head_end - req));
	if(!sp1) return 0;
	const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(head_end - sp1 - 1));
	if(!sp2) return 0;

	snprintf(method, msize, "%.*s", (int)(sp1 - req), req);
	snprintf(path, psize, "%.*s", (int)(sp2 - sp1 - 1), sp1 + 1);

	/* The query part does not choose the route. */
	char *q = strchr(path, '?');
	if(q)
	{
		snprintf(query, qsize, "%s", q + 1);
		*q = 0;
	}

	for(const char *line = strstr(req, "\r\n"); line && line < head_end; line = strstr(line + 2, "\r\n"))
	{
		const char *name = line + 2;
		if(!strncasecmp(name, "Host:", 5))
		{
			const char *v = name + 5;
			while(*v == ' ' || *v == '\t') v++;
			const char *e = strstr(v, "\r\n");
			if(e) snprintf(host, hsize, "%.*s", (int)(e - v), v);
			break;
		}
	}
	return method[0] && path[0] == '/';
}

/* The console's own browser asks for 127.0.0.1 or localhost, with or without
 * the port; anything else is a name someone else chose. */
static int gwm_web_host_ok(const char *host)
{
	static const char *names[] = { "127.0.0.1", "localhost" };

	for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		size_t n = strlen(names[i]);
		if(!strncasecmp(host, names[i], n) && (host[n] == 0 || host[n] == ':')) return 1;
	}
	return 0;
}

/* "en" when the page asked for English, "ru" otherwise: the site's default. */
static const char *gwm_web_lang(const char *query)
{
	for(const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
		if(!strncmp(p, "lang=en", 7) && (p[7] == 0 || p[7] == '&')) return "en";
	return "ru";
}

/* https://gowithme.club/player/x in English is https://gowithme.club/en/player/x. */
static void gwm_web_localize(const char *url, const char *lang, char *out, size_t size)
{
	const char *path = !strncmp(url, "https://", 8) ? strchr(url + 8, '/') : NULL;

	if(strcmp(lang, "en") || !path) snprintf(out, size, "%s", url);
	else snprintf(out, size, "%.*s/en%s", (int)(path - url), url, path);
}

typedef struct
{
	char    state[16];      /* linked, claim, code, connecting, offline, stopped */
	char    code[8];        /* the pairing code, or for claim the profile code */
	char    profile[64];    /* claim: the profile signed in on the console */
	int64_t sync_ago;       /* seconds since the last answer, -1 never */
	long    queued;         /* bytes waiting to be sent */
	char    profile_url[256];
	/* What the console recorded (reply.h); -1 until the server says. */
	int64_t rec_seconds, rec_games, rec_trophies;
	int     welcome;        /* open on the welcome screen */
} gwm_web_view_t;

/* The QR code for text as rows of 0 and 1, the way the page draws it. */
static char *gwm_web_qr(char *p, const char *end, const char *text)
{
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(GWM_WEB_QR_MAX_VERSION)];
	uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(GWM_WEB_QR_MAX_VERSION)];

	if(!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, GWM_WEB_QR_MAX_VERSION, qrcodegen_Mask_AUTO, 1))
		return gwm_lit(p, end, "[]");

	int size = qrcodegen_getSize(qr);
	p = gwm_lit(p, end, "[");
	for(int y = 0; y < size; y++)
	{
		p = gwm_lit(p, end, y ? ",\"" : "\"");
		for(int x = 0; x < size && p < end - 1; x++) *p++ = qrcodegen_getModule(qr, x, y) ? '1' : '0';
		*p = 0;
		p = gwm_lit(p, end, "\"");
	}
	return gwm_lit(p, end, "]");
}

static size_t gwm_web_state_json(char *out, size_t size, const gwm_web_view_t *v, const char *lang)
{
	char *p = out, *end = out + size, text[320];

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "version", GWM_VERSION);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "state", v->state);
	if(v->profile[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "profile", v->profile);
	}
	if(v->code[0])
	{
		snprintf(text, sizeof(text), GWM_WEB_QR_PREFIX "%s", v->code);
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "code", v->code);
		p = gwm_lit(p, end, ",\"qr\":");
		p = gwm_web_qr(p, end, text);
	}
	p = gwm_lit(p, end, ",\"syncAgo\":");
	p = gwm_i64(p, end, v->sync_ago);
	p = gwm_lit(p, end, ",\"queued\":");
	p = gwm_i64(p, end, v->queued);
	if(v->profile_url[0])
	{
		gwm_web_localize(v->profile_url, lang, text, sizeof(text));
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "profileUrl", text);
		p = gwm_lit(p, end, ",\"profileQr\":");
		p = gwm_web_qr(p, end, text);
	}
	if(v->rec_seconds >= 0)
	{
		p = gwm_lit(p, end, ",\"recorded\":{\"seconds\":");
		p = gwm_i64(p, end, v->rec_seconds);
		p = gwm_lit(p, end, ",\"games\":");
		p = gwm_i64(p, end, v->rec_games);
		p = gwm_lit(p, end, ",\"trophies\":");
		p = gwm_i64(p, end, v->rec_trophies);
		p = gwm_lit(p, end, "}");
	}
	if(v->welcome) p = gwm_lit(p, end, ",\"welcome\":true");
	p = gwm_lit(p, end, "}");
	return (size_t)(p - out) < size - 1 ? (size_t)(p - out) : 0;
}

#ifndef GWM_HOST_TEST

#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "asset.h"
#include "clock.h"
#include "log.h"

static pthread_mutex_t gwm_web_lock = PTHREAD_MUTEX_INITIALIZER;
static gwm_web_view_t gwm_web_view = { "connecting", "", "", -1, 0, "", -1, -1, -1, 0 };
static int gwm_web_pair_flag = 0;
static int gwm_web_welcome_flag = 0;
static int gwm_web_opened_flag = 0;
/* Raw time of the page's last /state request. */
static int64_t gwm_web_seen_at = 0;

/* 1 once each time the page is opened. */
static int gwm_web_opened(void)
{
	return __atomic_exchange_n(&gwm_web_opened_flag, 0, __ATOMIC_ACQ_REL);
}

/* The page is on screen: it asked for /state within the last few seconds. */
static int gwm_web_open(void)
{
	return gwm_now_raw() - __atomic_load_n(&gwm_web_seen_at, __ATOMIC_ACQUIRE) < 10;
}

static void gwm_web_publish(const gwm_web_view_t *v)
{
	pthread_mutex_lock(&gwm_web_lock);
	gwm_web_view = *v;
	pthread_mutex_unlock(&gwm_web_lock);
}

/* 1 once per press of the page's button. */
static int gwm_web_pair_wanted(void)
{
	return __atomic_exchange_n(&gwm_web_pair_flag, 0, __ATOMIC_ACQ_REL);
}

/* 1 once the welcome screen's button was pressed. */
static int gwm_web_welcome_read(void)
{
	return __atomic_exchange_n(&gwm_web_welcome_flag, 0, __ATOMIC_ACQ_REL);
}

static void gwm_web_send(int s, const char *status, const char *type, const void *body, size_t len)
{
	char head[256];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);

	if(send(s, head, (size_t)n, 0) != n) return;
	const char *b = body;
	while(len)
	{
		ssize_t w = send(s, b, len, 0);
		if(w <= 0) return;
		b += w;
		len -= (size_t)w;
	}
}

static void gwm_web_serve(int s)
{
	char req[GWM_WEB_REQ_MAX], method[8], path[64], query[64], host[64];
	size_t got = 0;

	while(got < sizeof(req) - 1)
	{
		ssize_t r = recv(s, req + got, sizeof(req) - 1 - got, 0);
		if(r <= 0) return;
		got += (size_t)r;
		req[got] = 0;
		if(gwm_web_parse(req, got, method, sizeof(method), path, sizeof(path), query, sizeof(query), host, sizeof(host))) break;
	}
	if(!path[0]) return;

	if(!gwm_web_host_ok(host))
	{
		gwm_log("page: refused a request for host \"%s\"", host);
		gwm_web_send(s, "403 Forbidden", "text/plain", "", 0);
		return;
	}

	if(!strcmp(method, "GET") && !strcmp(path, "/"))
	{
		gwm_web_send(s, "200 OK", "text/html; charset=utf-8", gwm_page_html, (size_t)(gwm_page_html_end - gwm_page_html));
	}
	else if(!strcmp(method, "GET") && !strcmp(path, "/icon.png"))
	{
		gwm_web_send(s, "200 OK", "image/png", gwm_icon_png, (size_t)(gwm_icon_png_end - gwm_icon_png));
	}
	else if(!strcmp(method, "GET") && !strcmp(path, "/state"))
	{
		static char json[GWM_WEB_STATE_MAX];
		gwm_web_view_t v;

		/* The page's first request names its language and window size. */
		if(strstr(query, "first=1"))
		{
			gwm_log("page: shown (%s)", query);
			__atomic_store_n(&gwm_web_opened_flag, 1, __ATOMIC_RELEASE);
		}
		__atomic_store_n(&gwm_web_seen_at, gwm_now_raw(), __ATOMIC_RELEASE);

		pthread_mutex_lock(&gwm_web_lock);
		v = gwm_web_view;
		pthread_mutex_unlock(&gwm_web_lock);

		size_t n = gwm_web_state_json(json, sizeof(json), &v, gwm_web_lang(query));
		if(n) gwm_web_send(s, "200 OK", "application/json", json, n);
		else  gwm_web_send(s, "500 Internal Server Error", "text/plain", "", 0);
	}
	else if(!strcmp(method, "POST") && !strcmp(path, "/pair"))
	{
		__atomic_store_n(&gwm_web_pair_flag, 1, __ATOMIC_RELEASE);
		gwm_web_send(s, "202 Accepted", "application/json", "{}", 2);
	}
	else if(!strcmp(method, "POST") && !strcmp(path, "/welcome"))
	{
		__atomic_store_n(&gwm_web_welcome_flag, 1, __ATOMIC_RELEASE);
		gwm_web_send(s, "202 Accepted", "application/json", "{}", 2);
	}
	else gwm_web_send(s, "404 Not Found", "text/plain", "", 0);
}

static void *gwm_web_worker(void *arg)
{
	(void)arg;
	int logged = 0;

	for(;;)
	{
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		struct sockaddr_in a;

		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(GWM_WEB_PORT);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if(ls < 0) { sleep(5); continue; }
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		/* A copy being replaced may hold the port for a moment. */
		if(bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0)
		{
			if(!logged++) gwm_log("page: port %d busy, retrying", GWM_WEB_PORT);
			close(ls);
			sleep(5);
			continue;
		}
		gwm_log("page: http://127.0.0.1:%d/", GWM_WEB_PORT);

		for(;;)
		{
			int s = accept(ls, NULL, NULL);
			if(s < 0) continue;

			struct timeval tv = { 3, 0 };
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			gwm_web_serve(s);
			close(s);
		}
	}
	return NULL;
}

static void gwm_web_start(void)
{
	pthread_t th;
	if(pthread_create(&th, NULL, gwm_web_worker, NULL) == 0) pthread_detach(th);
	else gwm_log("page: no thread");
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_WEB_H */
