/* The console name from its settings, e.g. "PS5-123".
 *
 * The site shows it instead of the device id, so it goes with the pairing
 * request and with every heartbeat: the console can be renamed. It is not a
 * key; the device id is.
 *
 * System parameter 6 (SCE_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME on PS4). The
 * buffer must be at least 65 bytes (the PS4 name limit): with 64 bytes the
 * service returns 0x80a10003 on firmware 13.40, with 128 it returns the name.
 * A failure is not cached: the name is asked again on the next heartbeat, and
 * once read it is kept for the rest of the run.
 */

#ifndef GWM_CONSOLE_H
#define GWM_CONSOLE_H

#include <stdio.h>
#include <string.h>

#include "log.h"

#define GWM_SYSTEM_PARAM_NAME 6

int sceSystemServiceParamGetString(int id, char *buf, size_t size);

#define GWM_CONSOLE_NAME_MAX 128

static char gwm_console_name_cache[GWM_CONSOLE_NAME_MAX];

static const char *gwm_console_name(void)
{
	static int attempts = 0;

	if(gwm_console_name_cache[0] || attempts >= 30) return gwm_console_name_cache;

	char name[sizeof(gwm_console_name_cache)] = "";
	int rc = sceSystemServiceParamGetString(GWM_SYSTEM_PARAM_NAME, name, sizeof(name));
	attempts++;

	if(rc == 0 && name[0])
	{
		snprintf(gwm_console_name_cache, sizeof(gwm_console_name_cache), "%s", name);
		gwm_log("console name: %s (attempt %d)", gwm_console_name_cache, attempts);
	}
	else if(attempts == 1 || attempts == 30)
	{
		gwm_log("console name unavailable (0x%08x, attempt %d)", rc, attempts);
	}
	return gwm_console_name_cache;
}

#endif /* GWM_CONSOLE_H */
