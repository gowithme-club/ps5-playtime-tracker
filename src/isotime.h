/* ISO 8601 timestamps, as the server expects them.
 *
 * The PS5 clock gives plain Unix time and libc is available, so gmtime_r and
 * timegm do the calendar work. The timestamp has a fixed width, so it can be shifted in place
 * inside queue lines once the clock offset is known (see upload.h).
 */

#ifndef GWM_ISOTIME_H
#define GWM_ISOTIME_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* "2026-07-05T18:30:00Z" plus the terminator */
#define GWM_ISO_LEN 21

static void gwm_iso_from_unix(char out[GWM_ISO_LEN], int64_t t)
{
	struct tm tm;
	time_t tt = (time_t)t;

	gmtime_r(&tt, &tm);
	snprintf(out, GWM_ISO_LEN, "%04d-%02d-%02dT%02d:%02d:%02dZ",
	         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	         tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Parses a timestamp in our own format. Strict: fields are read at fixed
 * offsets and separators are checked, so garbage in a queue is never
 * shifted. */
static int gwm_iso_to_unix(const char *s, int64_t *out)
{
	static const int pos[6] = { 0, 5, 8, 11, 14, 17 };
	static const int wid[6] = { 4, 2, 2, 2, 2, 2 };
	int f[6];

	if(s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
	   s[13] != ':' || s[16] != ':' || s[19] != 'Z') return 0;

	for(int k = 0; k < 6; k++)
	{
		int v = 0;
		for(int i = 0; i < wid[k]; i++)
		{
			char c = s[pos[k] + i];
			if(c < '0' || c > '9') return 0;
			v = v * 10 + (c - '0');
		}
		f[k] = v;
	}

	if(f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 ||
	   f[3] > 23 || f[4] > 59 || f[5] > 60) return 0;

	struct tm tm = { 0 };
	tm.tm_year = f[0] - 1900;
	tm.tm_mon  = f[1] - 1;
	tm.tm_mday = f[2];
	tm.tm_hour = f[3];
	tm.tm_min  = f[4];
	tm.tm_sec  = f[5];

	*out = (int64_t)timegm(&tm);
	return 1;
}

/* Shifts a timestamp in place by sec seconds. The timestamp usually sits in
 * the middle of a JSON line (a quote follows it), and the formatter writes
 * the terminator as the 21st byte, so that byte is saved and restored.
 * Returns 0 if the timestamp does not parse; the buffer is then untouched. */
static int gwm_iso_shift(char *iso, int64_t sec)
{
	int64_t t;

	if(!gwm_iso_to_unix(iso, &t)) return 0;
	if(t + sec <= 0) return 0;

	char after = iso[GWM_ISO_LEN - 1];
	gwm_iso_from_unix(iso, t + sec);
	iso[GWM_ISO_LEN - 1] = after;
	return 1;
}

#endif /* GWM_ISOTIME_H */
