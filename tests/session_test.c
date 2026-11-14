#include "check.h"
#include "session.h"

#define T0 1791392632 /* 2026-10-07T17:03:52Z */

int main(void)
{
	gwm_session_t s;
	char out[GWM_RECORD_MAX];
	int changed;

	gwm_session_init(&s);

	/* menu: nothing */
	CHECK(gwm_session_update(&s, T0, "", NULL, NULL, out, sizeof(out), &changed) == 0);
	CHECK(!changed && !s.active);

	/* game starts */
	CHECK(gwm_session_update(&s, T0, "CUSA03980", "Bioshock Infinite", "Player",
	                         out, sizeof(out), &changed) == 0);
	CHECK(changed && s.active && s.start == T0);

	/* same game */
	CHECK(gwm_session_update(&s, T0 + 10, "CUSA03980", "Bioshock Infinite", "Player",
	                         out, sizeof(out), &changed) == 0);
	CHECK(!changed);

	/* back to the menu: closed with a duration */
	size_t n = gwm_session_update(&s, T0 + 70, "", NULL, NULL, out, sizeof(out), &changed);
	CHECK(changed && n == strlen(out) && !s.active);
	CHECK_STR(out, "{\"account\":\"Player\",\"titleId\":\"CUSA03980\","
	               "\"title\":\"Bioshock Infinite\",\"platform\":\"ps5-cfw\","
	               "\"startedAt\":\"2026-10-07T17:03:52Z\","
	               "\"endedAt\":\"2026-10-07T17:05:02Z\",\"seconds\":70}\n");

	/* another game: the old one closes and the new one opens in one poll */
	gwm_session_update(&s, T0, "CUSA03980", "A", "Player", out, sizeof(out), &changed);
	n = gwm_session_update(&s, T0 + 30, "PPSA01234", "B", "Player", out, sizeof(out), &changed);
	CHECK(n > 0 && strstr(out, "\"seconds\":30") && strstr(out, "CUSA03980"));
	CHECK(s.active && !strcmp(s.title_id, "PPSA01234") && s.start == T0 + 30);

	/* sleep: the poll vanished for an hour; close at the last moment seen
	 * and open a new session, since the game is still running */
	gwm_session_update(&s, T0 + 40, "PPSA01234", "B", "Player", out, sizeof(out), &changed);
	n = gwm_session_update(&s, T0 + 3640, "PPSA01234", "B", "Player", out, sizeof(out), &changed);
	CHECK(n > 0 && strstr(out, "\"seconds\":10") && strstr(out, "\"platform\":\"ps5-cfw\""));
	CHECK(changed && s.active && s.start == T0 + 3640);

	/* a short launch is not recorded */
	n = gwm_session_update(&s, T0 + 3645, "", NULL, NULL, out, sizeof(out), &changed);
	CHECK(n == 0 && changed && !s.active);

	/* clock goes back: close at the last poll */
	gwm_session_update(&s, T0, "CUSA1", "C", "Player", out, sizeof(out), &changed);
	gwm_session_update(&s, T0 + 50, "CUSA1", "C", "Player", out, sizeof(out), &changed);
	n = gwm_session_update(&s, T0 - 1000, "CUSA1", "C", "Player", out, sizeof(out), &changed);
	CHECK(n > 0 && strstr(out, "\"seconds\":50"));
	CHECK(s.active && s.start == T0 - 1000);

	/* without a title the title id is sent: title is required */
	gwm_session_init(&s);
	gwm_session_update(&s, T0, "CUSA2", "", "Player", out, sizeof(out), &changed);
	n = gwm_session_update(&s, T0 + 20, "", NULL, NULL, out, sizeof(out), &changed);
	CHECK(n > 0 && strstr(out, "\"title\":\"CUSA2\""));

	return gwm_report("session");
}
