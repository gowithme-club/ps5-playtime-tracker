#include "check.h"
#include "isotime.h"
#include "clock.h"

int main(void)
{
	char iso[GWM_ISO_LEN];
	int64_t t = 0;

	/* 2026-10-07T17:05:02Z, the end of a BioShock session from a dump */
	gwm_iso_from_unix(iso, 1791392702);
	CHECK_STR(iso, "2026-10-07T17:05:02Z");
	CHECK(gwm_iso_to_unix(iso, &t) && t == 1791392702);
	CHECK(!gwm_iso_to_unix("2026-10-07 17:05:02Z", &t));
	CHECK(!gwm_iso_to_unix("2026-13-07T17:05:02Z", &t));

	/* an in-place shift in the middle of JSON keeps the quote after the timestamp */
	char line[] = "{\"endedAt\":\"2026-10-07T17:31:08Z\"}";
	CHECK(gwm_iso_shift(line + 12, -29 * 60));
	CHECK_STR(line, "{\"endedAt\":\"2026-10-07T17:02:08Z\"}");

	/* across midnight and a month boundary */
	char m[] = "2026-10-31T23:59:30Z\"";
	CHECK(gwm_iso_shift(m, 45));
	CHECK_STR(m, "2026-11-01T00:00:15Z\"");

	/* the Date header */
	const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
	                   "date: Wed, 07 Oct 2026 17:02:09 GMT\r\n\r\n";
	gwm_date_t d;
	CHECK(gwm_parse_http_date(resp, strlen(resp), &d));
	CHECK(d.year == 2026 && d.month == 10 && d.day == 7 &&
	      d.hour == 17 && d.minute == 2 && d.second == 9);
	CHECK(gwm_unix_from_date(&d) == 1791392529);

	/* "Date" not at the start of a line is not a header */
	const char *fake = "HTTP/1.1 200 OK\r\nX-Note: Date: Mon, 01 Jan 2001 00:00:00 GMT\r\n\r\n";
	CHECK(!gwm_parse_http_date(fake, strlen(fake), &d));

	/* offset: the console runs 29 minutes fast */
	gwm_clock_sync(resp, strlen(resp), 1791392529 + 29 * 60);
	CHECK(gwm_clock_synced && gwm_clock_offset == -29 * 60);

	/* one-second jitter is not adopted, drift is */
	gwm_clock_adopt(-29 * 60 + 1);
	CHECK(gwm_clock_offset == -29 * 60);
	gwm_clock_adopt(-29 * 60 + 5);
	CHECK(gwm_clock_offset == -29 * 60 + 5);

	return gwm_report("time");
}
