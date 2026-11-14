/* Clock correction from the server's time.
 *
 * A console kept away from Sony's servers drifts, often by tens of minutes.
 * The server rejects timestamps from the future, and the client would never
 * know.
 *
 * The time comes from the Date header, present in every HTTP response. The
 * payload records every timestamp in raw system time (time()) and adds the
 * offset at send time. System time rather than the
 * telemetry clock: app.db and trophy notifications use it too, so one offset
 * fits everything.
 *
 * Date parsing and hysteresis are pure logic, covered by a host test.
 */

#ifndef GWM_CLOCK_H
#define GWM_CLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

typedef struct
{
	int year, month, day, hour, minute, second;
} gwm_date_t;

static int gwm_month_by_name(const char *m)
{
	static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

	for(int i = 0; i < 12; i++)
		if(m[0] == names[i * 3] && m[1] == names[i * 3 + 1] && m[2] == names[i * 3 + 2])
			return i + 1;

	return 0;
}

static int gwm_num(const char *s, int len)
{
	int v = 0;
	for(int i = 0; i < len; i++)
	{
		if(s[i] < '0' || s[i] > '9') return -1;
		v = v * 10 + (s[i] - '0');
	}
	return v;
}

/* Finds the Date header in a response: "Date: Sat, 16 Aug 2026 21:45:00 GMT"
 * (RFC 7231). Matches only at the start of a header line. */
static int gwm_parse_http_date(const char *resp, size_t len, gwm_date_t *out)
{
	const char *p = NULL;

	for(size_t i = 0; i + 6 < len; i++)
	{
		if(i && resp[i - 1] != '\n') continue;

		if((resp[i] == 'D' || resp[i] == 'd') &&
		   (resp[i + 1] == 'a' || resp[i + 1] == 'A') &&
		   (resp[i + 2] == 't' || resp[i + 2] == 'T') &&
		   (resp[i + 3] == 'e' || resp[i + 3] == 'E') &&
		   resp[i + 4] == ':')
		{
			p = resp + i + 5;
			break;
		}
	}

	if(!p) return 0;

	const char *stop = resp + len;
	while(p < stop && *p == ' ') p++;
	while(p < stop && *p != ',' && *p != ' ') p++;   /* weekday */
	if(p < stop && *p == ',') p++;
	while(p < stop && *p == ' ') p++;

	if(p + 20 > stop) return 0;                      /* DD Mon YYYY HH:MM:SS */

	out->day = gwm_num(p, 2);
	out->month = gwm_month_by_name(p + 3);
	out->year = gwm_num(p + 7, 4);
	out->hour = gwm_num(p + 12, 2);
	out->minute = gwm_num(p + 15, 2);
	out->second = gwm_num(p + 18, 2);

	if(!out->month || out->day < 1 || out->day > 31 || out->year < 2000 ||
	   out->hour < 0 || out->hour > 23 || out->minute < 0 || out->minute > 59 ||
	   out->second < 0 || out->second > 60)
		return 0;

	return 1;
}

static int64_t gwm_unix_from_date(const gwm_date_t *d)
{
	struct tm tm = { 0 };
	tm.tm_year = d->year - 1900;
	tm.tm_mon  = d->month - 1;
	tm.tm_mday = d->day;
	tm.tm_hour = d->hour;
	tm.tm_min  = d->minute;
	tm.tm_sec  = d->second;
	return (int64_t)timegm(&tm);
}

/* Offset "server minus console", in seconds. */
static int64_t gwm_clock_offset = 0;
static int gwm_clock_synced = 0;

/* Both Date and the console clock have one-second resolution and are read
 * a moment apart, so even perfect clocks give an offset that flips between
 * neighbouring integers. That jitter must not be adopted: the start and end
 * of a short session could drift apart until the end comes before the start.
 * Real drift is still caught, because the comparison is against the adopted
 * value, not the previous reading. */
#define GWM_CLOCK_HYSTERESIS_SEC 1

static void gwm_clock_adopt(int64_t new_offset)
{
	if(gwm_clock_synced)
	{
		int64_t delta = new_offset - gwm_clock_offset;
		if(delta < 0) delta = -delta;
		if(delta <= GWM_CLOCK_HYSTERESIS_SEC) return;
	}

	gwm_clock_offset = new_offset;
	gwm_clock_synced = 1;
}

/* Called with the start of a server response and the raw clock at that moment. */
static void gwm_clock_sync(const char *resp, size_t len, int64_t local_now)
{
	gwm_date_t d;

	if(!gwm_parse_http_date(resp, len, &d)) return;
	gwm_clock_adopt(gwm_unix_from_date(&d) - local_now);
}

/* The console's raw clock. */
static int64_t gwm_now_raw(void)
{
	return (int64_t)time(NULL);
}

/* Corrected time, for what goes to the server right now. */
static int64_t gwm_now(void)
{
	return gwm_now_raw() + gwm_clock_offset;
}

#endif /* GWM_CLOCK_H */
