/* A minimal HTTP client.
 *
 * Exactly what the ingest API needs: POST with a JSON body, Authorization,
 * a mandatory Content-Length, HTTP/1.1, Connection: close. Plain HTTP: the
 * PS5 has TLS (libSceSsl), but the server accepts HTTP and the device key is
 * the only secret on the wire.
 *
 * Request building and status parsing are pure, covered by a host test.
 */

#ifndef GWM_HTTP_H
#define GWM_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "version.h"

/* The server shows this value next to the device. It knows the platform
 * from the token, so this is the bare version. */
#define GWM_CLIENT_ID  GWM_VERSION
#define GWM_USER_AGENT GWM_PRODUCT "-ps5/" GWM_VERSION

#define GWM_HTTP_HEAD_MAX 768
#define GWM_TOKEN_HEADER_MAX 192

/* The poll loop is single-threaded: while it waits on the network, nothing
 * watches the game. Ten seconds is enough even for slow Wi-Fi. */
#define GWM_HTTP_TIMEOUT_SEC 10

/* Negative codes are ours, not the server's. */
#define GWM_HTTP_ERR_SOCKET  (-1)
#define GWM_HTTP_ERR_RESOLVE (-2)
#define GWM_HTTP_ERR_CONNECT (-3)
#define GWM_HTTP_ERR_SEND    (-4)
#define GWM_HTTP_ERR_RECV    (-5)
#define GWM_HTTP_ERR_PARSE   (-6)

static size_t gwm_build_request(char *out, size_t out_size,
                                const char *path, const char *host,
                                uint16_t port, const char *token,
                                size_t body_len)
{
	char hostport[128];

	/* A non-default port belongs in Host. */
	if(port != 80) snprintf(hostport, sizeof(hostport), "%s:%u", host, (unsigned)port);
	else           snprintf(hostport, sizeof(hostport), "%s", host);

	/* No token means a pairing request (pair.h): the server takes those
	 * without Authorization and would treat an empty Bearer as a bad token. */
	char auth[GWM_TOKEN_HEADER_MAX] = "";
	if(token && token[0]) snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);

	int n = snprintf(out, out_size,
	                 "POST %s HTTP/1.1\r\n"
	                 "Host: %s\r\n"
	                 "%s"
	                 "User-Agent: " GWM_USER_AGENT "\r\n"
	                 "X-Client-Version: " GWM_CLIENT_ID "\r\n"
	                 "Content-Type: application/json\r\n"
	                 "Content-Length: %zu\r\n"
	                 "Connection: close\r\n"
	                 "\r\n",
	                 path, hostport, auth, body_len);

	if(n < 0 || (size_t)n >= out_size) return 0;
	return (size_t)n;
}

/* "HTTP/1.1 200 OK" -> 200. */
static int gwm_parse_status(const char *resp, size_t len)
{
	if(len < 12 || strncmp(resp, "HTTP/", 5) != 0) return GWM_HTTP_ERR_PARSE;

	const char *p = resp, *stop = resp + len;

	while(p < stop && *p != ' ') p++;
	while(p < stop && *p == ' ') p++;

	int code = 0, digits = 0;
	while(p < stop && *p >= '0' && *p <= '9' && digits < 3)
	{
		code = code * 10 + (*p++ - '0');
		digits++;
	}

	return (digits == 3) ? code : GWM_HTTP_ERR_PARSE;
}

#ifndef GWM_HOST_TEST

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#include "clock.h"
#include "config.h"

static int gwm_send_all(int s, const char *buf, size_t len)
{
	while(len)
	{
		ssize_t n = send(s, buf, len, 0);
		if(n <= 0) return -1;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Sends the body and returns the HTTP status or GWM_HTTP_ERR_*. If out is
 * given, the response body goes there (cut to the buffer size). Headers are
 * always read in full, since Date comes from them. */
static int gwm_http_request(const gwm_config_t *cfg, const char *path,
                            const char *body, size_t body_len,
                            const char *token, char *out, size_t out_size)
{
	struct addrinfo hints, *res = NULL;
	char port[8];
	char head[GWM_HTTP_HEAD_MAX];
	char resp[8192];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%u", (unsigned)cfg->port);

	if(getaddrinfo(cfg->host, port, &hints, &res) != 0 || !res)
		return GWM_HTTP_ERR_RESOLVE;

	int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(s < 0) { freeaddrinfo(res); return GWM_HTTP_ERR_SOCKET; }

	struct timeval tv = { GWM_HTTP_TIMEOUT_SEC, 0 };
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int rc = connect(s, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if(rc < 0) { close(s); return GWM_HTTP_ERR_CONNECT; }

	size_t head_len = gwm_build_request(head, sizeof(head), path, cfg->host,
	                                    cfg->port, token, body_len);

	if(!head_len || gwm_send_all(s, head, head_len) < 0 ||
	   (body_len && gwm_send_all(s, body, body_len) < 0))
	{
		close(s);
		return GWM_HTTP_ERR_SEND;
	}

	/* With Connection: close the server closes the connection itself, so the
	 * body is read to the end of the stream. Without out, only the headers. */
	size_t got = 0;
	while(got < sizeof(resp) - 1)
	{
		ssize_t n = recv(s, resp + got, sizeof(resp) - 1 - got, 0);
		if(n <= 0) break;
		got += (size_t)n;
		resp[got] = 0;
		if(!out && strstr(resp, "\r\n\r\n")) break;
	}
	close(s);

	if(!got) return GWM_HTTP_ERR_RECV;
	resp[got] = 0;

	if(out && out_size)
	{
		const char *b = strstr(resp, "\r\n\r\n");
		snprintf(out, out_size, "%s", b ? b + 4 : "");
	}

	gwm_clock_sync(resp, got, gwm_now_raw());
	return gwm_parse_status(resp, got);
}

static int gwm_http_post(const gwm_config_t *cfg, const char *path,
                         const char *body, size_t body_len)
{
	return gwm_http_request(cfg, path, body, body_len, cfg->token, NULL, 0);
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_HTTP_H */
