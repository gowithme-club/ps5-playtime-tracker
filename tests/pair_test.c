#include "check.h"
#include "config.h"
#include "console.h"
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

	/* Codes are taken for a window that opens with the first one. */
	memset(&p, 0, sizeof(p));
	p.next_start = 100;
	CHECK(gwm_pair_may_start(&p, 50) == 0);
	CHECK(gwm_pair_may_start(&p, 100) == 1);
	/* No code yet (server unreachable): retries never run out. */
	CHECK(gwm_pair_may_start(&p, 100 + 10 * GWM_PAIR_WINDOW_SEC) == 1);
	p.window_end = 1000 + GWM_PAIR_WINDOW_SEC;
	CHECK(gwm_pair_may_start(&p, 1000 + GWM_PAIR_WINDOW_SEC - 1) == 1);
	CHECK(gwm_pair_may_start(&p, 1000 + GWM_PAIR_WINDOW_SEC) == -1);
	/* Unlinked on the site: no codes until the next start. */
	memset(&p, 0, sizeof(p));
	p.done = 1;
	CHECK(gwm_pair_may_start(&p, 0) == -1);

	size_t n = gwm_pair_start_body(out, sizeof(out), "Player", "PS5-123", "00112233445566778899aabbccddeeff");
	CHECK(n == strlen(out));
	CHECK_STR(out, "{\"platform\":\"ps5-cfw\",\"device\":\"PS5-123\","
	               "\"consoleId\":\"00112233445566778899aabbccddeeff\",\"profile\":\"Player\"}");
	gwm_pair_start_body(out, sizeof(out), "", "", "");
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

	/* The console name and identifier go into the heartbeat body; empty ones are not sent. */
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "PS5-123", "00112233445566778899aabbccddeeff",
	                    "", 0, 0, "", "", "", "");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"deviceName\":\"PS5-123\","
	               "\"consoleId\":\"00112233445566778899aabbccddeeff\",\"current\":[]}");
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "", "", "", 1, 0, "", "", "", "");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"current\":[]}");
	/* The open profile, and whether the tile is on screen to show its code. */
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "", "", "Mira", 0, 0, "", "", "", "");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"foregroundAccount\":\"Mira\",\"current\":[]}");
	gwm_build_heartbeat(out, sizeof(out), "2026-10-07T21:35:18Z", "", "", "Mira", 1, 1, "Mira", "PPSA99031", "ICO",
	                    "2026-10-07T21:30:00Z");
	CHECK_STR(out, "{\"sentAt\":\"2026-10-07T21:35:18Z\",\"foregroundAccount\":\"Mira\",\"profileCodeWanted\":true,"
	               "\"current\":[{\"account\":\"Mira\",\"titleId\":\"PPSA99031\",\"titleName\":\"ICO\","
	               "\"platform\":\"ps5-cfw\",\"startedAt\":\"2026-10-07T21:30:00Z\"}]}");

	/* SHA-256 against the standard vectors: one block, the empty input, two blocks of padding. */
	uint8_t digest[GWM_SHA256_LEN];
	char hex[65];
	const char *vectors[][2] = {
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
	};
	for(size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
	{
		gwm_sha256((const uint8_t *)vectors[i][0], strlen(vectors[i][0]), digest);
		for(int k = 0; k < GWM_SHA256_LEN; k++) snprintf(hex + k * 2, 3, "%02x", digest[k]);
		CHECK_STR(hex, vectors[i][1]);
	}

	/* The console identifier: 32 hex characters of a prefixed hash; no identifier from zeros. */
	uint8_t psid[GWM_OPENPSID_LEN], zeros[GWM_OPENPSID_LEN] = { 0 };
	char id[GWM_CONSOLE_ID_LEN];
	for(int i = 0; i < GWM_OPENPSID_LEN; i++) psid[i] = (uint8_t)i;
	CHECK(gwm_console_id_hash(psid, id) == 1);
	CHECK_STR(id, "e547e82517de890aa1894d27afe2ed54");
	CHECK(gwm_console_id_hash(zeros, id) == 0);
	CHECK_STR(id, "");

	return gwm_report("pair");
}
