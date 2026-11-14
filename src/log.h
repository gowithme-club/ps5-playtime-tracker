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

/* Classic pop-up notification. The layout follows the SDK's
 * samples/notify_debug: the system ignores everything before the text. */
typedef struct
{
	char useless[45];
	char message[3075];
} gwm_notify_request_t;

int sceKernelSendNotificationRequest(int, gwm_notify_request_t *, size_t, int);

static int gwm_popups = 1;

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
	if(gwm_popups) sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#endif /* GWM_LOG_H */
