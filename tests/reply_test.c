#include "check.h"
#include "reply.h"

#define NOW 1791398400 /* 2026-10-07T18:40:00Z */

int main(void)
{
	char span[24], texts[3][256];

	/* What the console recorded, from the heartbeat answer. */
	gwm_recorded_t rec = { -1, -1, -1 };
	CHECK(gwm_reply_recorded("{\"sessionsWatermark\":null,\"recorded\":{\"seconds\":261240,\"games\":42,"
	                         "\"trophies\":318},\"current\":[]}", &rec));
	CHECK(rec.seconds == 261240 && rec.games == 42 && rec.trophies == 318);
	rec.seconds = -1;
	CHECK(!gwm_reply_recorded("{\"sessionsWatermark\":null,\"current\":[]}", &rec));   /* an older server */
	CHECK(!gwm_reply_recorded("{\"recorded\":{\"seconds\":5,\"games\":1}}", &rec));     /* incomplete */
	CHECK(!gwm_reply_recorded("{\"recorded\":null}", &rec));
	CHECK(rec.seconds == -1);                                                       /* left alone */

	/* Whose profile the tile shows. */
	gwm_profile_t prof;
	gwm_reply_profile("{\"profileUrl\":\"https://gowithme.club/player/mira\",\"recorded\":{}}", &prof);
	CHECK_STR(prof.url, "https://gowithme.club/player/mira");
	CHECK(!prof.unclaimed && !prof.code[0]);
	gwm_reply_profile("{\"profileUrl\":\"https://evil.example/player/x\"}", &prof);
	CHECK(!prof.url[0]);
	gwm_reply_profile("{\"profileUnclaimed\":true,\"recorded\":{\"seconds\":0}}", &prof);
	CHECK(prof.unclaimed && !prof.url[0] && !prof.code[0]);       /* no code until the tile asks */
	gwm_reply_profile("{\"profileUnclaimed\":true,\"profileCode\":\"042137\"}", &prof);
	CHECK(prof.unclaimed);
	CHECK_STR(prof.code, "042137");
	gwm_reply_profile("{\"profileUnclaimed\":true,\"profileCode\":\"04213x\"}", &prof);
	CHECK(prof.unclaimed && !prof.code[0]);
	gwm_reply_profile("{\"sessionsWatermark\":null,\"current\":[]}", &prof);   /* an older server, or nobody's page */
	CHECK(!prof.url[0] && !prof.unclaimed && !prof.code[0]);

	gwm_format_span(30, span, sizeof(span));    CHECK_STR(span, "< 1 min");
	gwm_format_span(7 * 60 + 59, span, sizeof(span)); CHECK_STR(span, "7 min");
	gwm_format_span(2 * 3600 + 5 * 60, span, sizeof(span)); CHECK_STR(span, "2 h 5 min");

	/* One fresh session, in the shape of a real server response. */
	const char *one =
		"{\"accepted\":1,\"duplicates\":0,\"rejected\":[],"
		"\"sessionsWatermark\":\"2026-10-07T18:35:00.000Z\","
		"\"sessions\":[{\"titleId\":\"ps5-cfw:CUSA07202\",\"titleName\":\"Ace Combat 7: Skies Unknown\","
		"\"startedAt\":\"2026-10-07T18:28:00.000Z\",\"endedAt\":\"2026-10-07T18:35:00.000Z\","
		"\"seconds\":420,\"totalSeconds\":2460}]}";
	CHECK(gwm_reply_messages(one, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "Ace Combat 7: Skies Unknown\n7 min recorded · 41 min total");

	/* The server could not compute the total: only what was recorded. */
	const char *no_total =
		"{\"accepted\":1,\"sessions\":[{\"titleName\":\"Game {\\\"X\\\"}\",\"endedAt\":\"2026-10-07T18:35:00Z\","
		"\"seconds\":30,\"totalSeconds\":null}]}";
	CHECK(gwm_reply_messages(no_total, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "Game {\"X\"}\n< 1 min recorded");

	/* An old session from the queue stays silent, a fresh one next to it does not. */
	const char *mixed =
		"{\"accepted\":2,\"sessions\":["
		"{\"titleName\":\"Old\",\"endedAt\":\"2026-10-06T10:00:00.000Z\",\"seconds\":600,\"totalSeconds\":600},"
		"{\"titleName\":\"New\",\"endedAt\":\"2026-10-07T18:39:00.000Z\",\"seconds\":120,\"totalSeconds\":720}]}";
	CHECK(gwm_reply_messages(mixed, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "New\n2 min recorded · 12 min total");

	/* All old: no notifications; duplicates (an empty list) neither. */
	const char *old =
		"{\"accepted\":1,\"sessions\":[{\"titleName\":\"Old\",\"endedAt\":\"2026-10-06T10:00:00Z\",\"seconds\":600}]}";
	CHECK(gwm_reply_messages(old, NOW, texts, 3) == 0);
	CHECK(gwm_reply_messages("{\"accepted\":0,\"duplicates\":1,\"sessions\":[]}", NOW, texts, 3) == 0);

	/* Four fresh at once: one line with the sum. */
	const char *many =
		"{\"accepted\":4,\"sessions\":["
		"{\"titleName\":\"A\",\"endedAt\":\"2026-10-07T18:30:00Z\",\"seconds\":600},"
		"{\"titleName\":\"B\",\"endedAt\":\"2026-10-07T18:31:00Z\",\"seconds\":600},"
		"{\"titleName\":\"C\",\"endedAt\":\"2026-10-07T18:32:00Z\",\"seconds\":600},"
		"{\"titleName\":\"D\",\"endedAt\":\"2026-10-07T18:33:00Z\",\"seconds\":600}]}";
	CHECK(gwm_reply_messages(many, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "4 sessions recorded\n40 min played");

	/* A console set to Russian; any other language stays English. */
	CHECK(gwm_lang_of(GWM_SYSTEM_LANG_RU) == GWM_LANG_RU);
	CHECK(gwm_lang_of(1) == GWM_LANG_EN && gwm_lang_of(-1) == GWM_LANG_EN);
	gwm_lang = GWM_LANG_RU;
	gwm_format_span(30, span, sizeof(span));    CHECK_STR(span, "< 1 мин");
	gwm_format_span(2 * 3600 + 5 * 60, span, sizeof(span)); CHECK_STR(span, "2 ч 5 мин");
	CHECK(gwm_reply_messages(one, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "Ace Combat 7: Skies Unknown\nЗаписано 7 мин · всего 41 мин");
	CHECK(gwm_reply_messages(no_total, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "Game {\"X\"}\nЗаписано < 1 мин");
	CHECK(gwm_reply_messages(many, NOW, texts, 3) == 1);
	CHECK_STR(texts[0], "Записано сессий: 4\nСыграно 40 мин");
	gwm_lang = GWM_LANG_EN;

	return gwm_report("reply");
}
