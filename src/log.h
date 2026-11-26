/* Log and on-screen notifications.
 *
 * The log is /data/gowithme.club/log.txt, readable over FTP. The payload's
 * stdout is the elfldr socket, which closes together with the socat that sent
 * the payload, so it cannot be relied on (main ignores SIGPIPE). When the log
 * reaches its cap it becomes log.old.txt and a new one starts, so rotation
 * never loses all the context.
 */

#ifndef GWM_LOG_H
#define GWM_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "isotime.h"
#include "json.h"
#include "lang.h"

#define GWM_LOG_FILE     GWM_DATA_DIR "/log.txt"
#define GWM_LOG_OLD_FILE GWM_DATA_DIR "/log.old.txt"
#define GWM_LOG_MAX      (256 * 1024)

static void gwm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void gwm_log(const char *fmt, ...)
{
	char line[1024], iso[GWM_ISO_LEN];
	va_list ap;

	gwm_iso_from_unix(iso, (int64_t)time(NULL));

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	struct stat st;
	if(stat(GWM_LOG_FILE, &st) == 0 && st.st_size > GWM_LOG_MAX)
		rename(GWM_LOG_FILE, GWM_LOG_OLD_FILE);

	FILE *f = fopen(GWM_LOG_FILE, "a");
	if(f)
	{
		fprintf(f, "%s %s\n", iso, line);
		fclose(f);
	}

	printf("%s %s\n", iso, line);
}

/* On-screen notifications.
 *
 * sceKernelSendNotificationRequest (the SDK's samples/notify_debug) titles
 * every message "Debug:", which reads as something broken to whoever sees
 * it. sceNotificationSend (samples/notify) takes a JSON toast with an icon
 * and two lines: the brand mark beside the text instead. The old call stays
 * as the fallback when the system refuses the toast.
 *
 * The toast has a large line and a small one, which the shell draws above
 * it. A message's first line is the large one and its second line, as in
 * "Game\n7 min recorded", the small one; without a second line the small one
 * says "gowithme.club". The large line holds about 20 characters before it
 * wraps, so it carries the news and the small line the rest.
 *
 * The words are the console's language (lang.h). Every toast carries an
 * "Open" action to the tile's page, as a trophy
 * toast carries "View trophies": pressing PS while it shows opens it. */

typedef struct
{
	char useless[45];
	char message[3075];
} gwm_notify_request_t;

int sceKernelSendNotificationRequest(int, gwm_notify_request_t *, size_t, int);
int sceNotificationSend(int user_id, _Bool is_logged, const char *payload);

#define GWM_NOTIFY_SYSTEM_USER 0xFE
/* Written at start (main.c) from the tile icon, so a toast has it even
 * when the tile was deleted. */
#define GWM_NOTIFY_ICON GWM_DATA_DIR "/icon.png"
/* The same file as the shell sees it: /data is /user/data, and the shell
 * draws a placeholder for the short path. */
#define GWM_NOTIFY_ICON_URL "/user" GWM_NOTIFY_ICON

static int gwm_popups = 1;

static int gwm_toast_rich(const char *message)
{
	static unsigned seq = 0;
	char head[256], sub[256], json[2048], iso[GWM_ISO_LEN];
	char *p = json, *end = json + sizeof(json);
	const char *text = message, *nl;

	nl = strchr(text, '\n');
	snprintf(head, sizeof(head), "%.*s", nl ? (int)(nl - text) : (int)strlen(text), text);
	snprintf(sub, sizeof(sub), "%s", nl ? nl + 1 : GWM_PRODUCT);
	gwm_iso_from_unix(iso, (int64_t)time(NULL));

	p = gwm_lit(p, end, "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
	                    "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\","
	                    "\"isImmediate\":true,\"priority\":100,\"viewData\":{"
	                    "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"" GWM_NOTIFY_ICON_URL "\"}},"
	                    "\"message\":{\"body\":\"");
	p = gwm_json_str(p, end, head);
	p = gwm_lit(p, end, "\"}");
	if(sub[0])
	{
		p = gwm_lit(p, end, ",\"subMessage\":{\"body\":\"");
		p = gwm_json_str(p, end, sub);
		p = gwm_lit(p, end, "\"}");
	}
	p = gwm_lit(p, end, ",\"actions\":[{\"actionName\":\"");
	p = gwm_json_str(p, end, GWM_TR("Open", "Открыть"));
	p = gwm_lit(p, end, "\",\"actionType\":\"DeepLink\",\"defaultFocus\":true,"
	                    "\"parameters\":{\"actionUrl\":\"" GWM_WEB_URL "\"}}]");
	p = gwm_lit(p, end, "}},\"createdDateTime\":\"");
	p = gwm_lit(p, end, iso);
	p = gwm_lit(p, end, "\",\"localNotificationId\":\"");
	p = gwm_i64(p, end, (int64_t)time(NULL) % 100000000 * 10 + (seq++ % 10));
	p = gwm_lit(p, end, "\"}");
	if(p >= end - 1) return -1;

	return sceNotificationSend(GWM_NOTIFY_SYSTEM_USER, 1, json);
}

static void gwm_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void gwm_toast(const char *fmt, ...)
{
	gwm_notify_request_t req;
	va_list ap;

	memset(&req, 0, sizeof(req));
	va_start(ap, fmt);
	vsnprintf(req.message, sizeof(req.message), fmt, ap);
	va_end(ap);

	gwm_log("toast: %s", req.message);
	if(!gwm_popups) return;

	int rc = gwm_toast_rich(req.message);
	if(rc != 0)
	{
		static int logged = 0;
		if(!logged++) gwm_log("toast: sceNotificationSend -> 0x%08x, using the plain one", rc);
		sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	}
}

#endif /* GWM_LOG_H */
