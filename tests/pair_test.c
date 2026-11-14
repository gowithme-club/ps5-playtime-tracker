#include "check.h"
#include "config.h"
#include "pair.h"

int main(void)
{
	gwm_pair_t p;
	char token[GWM_TOKEN_MAX], out[1024];

	memset(&p, 0, sizeof(p));
	CHECK(gwm_pair_parse_start("{\"code\":\"042917\",\"secret\":\"s3cr3t-_x\",\"expiresAt\":\"2026-10-07T18:10:00.000Z\",\"interval\":5}", &p));
	CHECK_STR(p.code, "042917");
	CHECK_STR(p.secret, "s3cr3t-_x");
	CHECK(p.interval == 5);

	/* No interval, or an absurd one: five seconds. */
	CHECK(gwm_pair_parse_start("{\"code\":\"123456\",\"secret\":\"a\",\"interval\":9999}", &p) && p.interval == 5);
	CHECK(!gwm_pair_parse_start("{\"code\":\"12345\",\"secret\":\"a\"}", &p));
	CHECK(!gwm_pair_parse_start("{\"error\":\"This platform does not pair by code\"}", &p));

	CHECK(gwm_pair_parse_poll("{\"status\":\"pending\",\"code\":\"123456\"}", "sec", token, sizeof(token)) == 0);
	/* A server without tokens: the key is the console's own secret. */
	CHECK(gwm_pair_parse_poll("{\"status\":\"paired\",\"deviceId\":\"ps5-cfw-x\"}", "s3cr3t-_x", token, sizeof(token)) == 1);
	CHECK_STR(token, "s3cr3t-_x");
	/* An early server version sent a token: use it. */
	CHECK(gwm_pair_parse_poll("{\"status\":\"paired\",\"token\":\"tok_ABC-123\",\"deviceId\":\"ps5-cfw-x\"}", "sec", token, sizeof(token)) == 1);
	CHECK_STR(token, "tok_ABC-123");
	CHECK(gwm_pair_parse_poll("{\"error\":\"Unknown pairing\"}", "sec", token, sizeof(token)) == -1);

	size_t n = gwm_pair_start_body(out, sizeof(out), "Player", "PS5-123");
	CHECK(n == strlen(out));
	CHECK_STR(out, "{\"platform\":\"ps5-cfw\",\"device\":\"PS5-123\",\"profile\":\"Player\"}");
	gwm_pair_start_body(out, sizeof(out), "", "");
	CHECK_STR(out, "{\"platform\":\"ps5-cfw\"}");

	/* The token becomes the first line, the old one is dropped, the rest stays. */
	n = gwm_cfg_with_token("# my config\ntoken = old\naccount = Player\n  token=older\nhost = 10.0.0.1", "new", out, sizeof(out));
	CHECK(n == strlen(out));
	CHECK_STR(out, "token = new\n# my config\naccount = Player\nhost = 10.0.0.1\n");

	/* An empty config gives one line. And tokens = ... is not a token. */
	gwm_cfg_with_token("", "t", out, sizeof(out));
	CHECK_STR(out, "token = t\n");
	gwm_cfg_with_token("tokens = 1\n", "t", out, sizeof(out));
	CHECK_STR(out, "token = t\ntokens = 1\n");

	/* The result parses back. */
	gwm_config_t c;

	/* An empty key (rejected by the server): config without a key, the rest intact. */
	gwm_cfg_with_token("token = old\naccount = Player\n", "", out, sizeof(out));
	gwm_cfg_parse(&c, out);
	CHECK(!c.valid);
	CHECK_STR(c.account, "Player");

	gwm_cfg_with_token("account = Player\n", "abc", out, sizeof(out));
	gwm_cfg_parse(&c, out);
	CHECK(c.valid);
	CHECK_STR(c.token, "abc");
	CHECK_STR(c.account, "Player");

	/* The console name goes into the heartbeat body; an empty one is not sent. */
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "PS5-123", 0, "", "", "", "");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"deviceName\":\"PS5-123\",\"current\":[]}");
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "", 0, "", "", "", "");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"current\":[]}");

	return gwm_report("pair");
}
