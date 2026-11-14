/* PS5 game trophies from the trophy files, including those earned while the
 * payload was not running.
 *
 * Trophy notifications (notif.h) vanish within hours, so a trophy earned
 * without the payload rarely arrives that way. PS5 games have a better
 * source, the trophy system's unencrypted files:
 *
 *   /user/home/<uid_hex>/trophy2/nobackup/data/<NPWR>/TRPTITLE.DAT
 *       what the user earned and when;
 *   /user/trophy2/nobackup/conf/<NPWR>/TROPHY.UCP
 *       the set's shared config: grades, names, descriptions.
 *
 * PS4 games have nothing like it: their trophies sit in an encrypted
 * trophy.img, so notifications remain their only source.
 *
 * TRPTITLE.DAT is big-endian. A 0x40-byte header (magic T2PD), then data in
 * 0x400-byte chunks, each followed by a 0x20-byte SHA-256 of the chunk. A
 * "logical" address is an offset into the data without the hashes. Logical
 * 0 holds magic T2TD and the block count (+0x08); from 0x30 a table of
 * 0x20-byte block entries: type, record size, ?, record count, offset
 * (8 bytes). Block 0x800 has one record per trophy: 0x10 header (type,
 * size), id (+0x10), flags (+0x14, low bit set when earned), earned time
 * (+0x20, microseconds since 0001-01-01 on the journal clock, journal.h).
 *
 * TROPHY.UCP is an archive: file count at 0x10, from 0x60 a table of
 * 0x40-byte entries (name 0x20, offset 8, size 8). The payload needs
 * tropconf.json (grades, hidden, default language) and
 * tropmeta_<lang>.json (names).
 *
 * A set goes to the server as its full state, every earned trophy, as the
 * API asks. The server diffs it itself, so a repeat is safe, and only
 * trophies newer than the account's first sync are announced.
 *
 * Parsing and body building are pure, covered by a host test
 * (tests/fixtures/trptitle_ps5.dat, trophy_ps5.ucp).
 */

#ifndef GWM_TROPHY2_H
#define GWM_TROPHY2_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isotime.h"
#include "journal.h"
#include "json.h"
#include "record.h"
#include "trophy.h"

#define GWM_T2_HEAD   0x40
#define GWM_T2_CHUNK  0x400
#define GWM_T2_HASH   0x20
#define GWM_T2_TROPHY 0x800

/* No set has more trophies; the server has the same limit. */
#define GWM_T2_MAX 1000

typedef struct
{
	int     id;
	int64_t earned;   /* system clock, like everything in the queues */
} gwm_t2_item_t;

static int gwm_t2_byte(const uint8_t *d, size_t n, uint64_t l, uint8_t *out)
{
	uint64_t p = GWM_T2_HEAD + (l / GWM_T2_CHUNK) * (GWM_T2_CHUNK + GWM_T2_HASH) + l % GWM_T2_CHUNK;
	if(p >= n) return 0;
	*out = d[p];
	return 1;
}

/* A field at a logical address. Byte by byte: a record may span two chunks. */
static int gwm_t2_be(const uint8_t *d, size_t n, uint64_t l, int bytes, uint64_t *out)
{
	uint64_t v = 0;
	for(int i = 0; i < bytes; i++)
	{
		uint8_t b;
		if(!gwm_t2_byte(d, n, l + (uint64_t)i, &b)) return 0;
		v = (v << 8) | b;
	}
	*out = v;
	return 1;
}

/* Earned trophies into items. offset is journal clock minus system clock.
 * Returns their count, or -1 if the file does not look like TRPTITLE.DAT
 * (or is only partly written). */
static int gwm_t2_parse(const uint8_t *d, size_t n, int64_t offset, gwm_t2_item_t *items, int max)
{
	uint64_t blocks, type, size, count, base, v;

	if(n < GWM_T2_HEAD + 0x30 || memcmp(d, "T2PD", 4) || memcmp(d + GWM_T2_HEAD, "T2TD", 4)) return -1;
	if(!gwm_t2_be(d, n, 0x08, 4, &blocks) || blocks > 64) return -1;

	for(uint64_t b = 0; b < blocks; b++)
	{
		uint64_t e = 0x30 + b * 0x20;
		if(!gwm_t2_be(d, n, e, 4, &type) || !gwm_t2_be(d, n, e + 4, 4, &size) ||
		   !gwm_t2_be(d, n, e + 12, 4, &count) || !gwm_t2_be(d, n, e + 16, 8, &base))
			return -1;
		if(type != GWM_T2_TROPHY) continue;
		if(size < 0x20 || count > GWM_T2_MAX) return -1;

		int got = 0;
		for(uint64_t i = 0; i < count; i++)
		{
			uint64_t r = base + i * (0x10 + size), id, flags, ticks;

			if(!gwm_t2_be(d, n, r, 4, &v) || v != GWM_T2_TROPHY) return -1;
			if(!gwm_t2_be(d, n, r + 0x10, 4, &id) || !gwm_t2_be(d, n, r + 0x14, 4, &flags) ||
			   !gwm_t2_be(d, n, r + 0x20, 8, &ticks))
				return -1;

			if(!(flags & 1) || ticks / 1000000 <= GWM_JOURNAL_EPOCH || got >= max) continue;

			items[got].id = (int)id;
			items[got].earned = (int64_t)(ticks / 1000000) - GWM_JOURNAL_EPOCH - offset;
			got++;
		}
		return got;
	}
	return -1;
}

/* A file in TROPHY.UCP by its table entry (hdr is the start of the archive). */
static int gwm_ucp_find(const uint8_t *hdr, size_t n, const char *name, uint64_t *off, uint64_t *size)
{
	uint64_t count = 0;

	if(n < 0x60) return 0;
	for(int i = 0; i < 4; i++) count = (count << 8) | hdr[0x10 + i];

	for(uint64_t i = 0; i < count && 0x60 + (i + 1) * 0x40 <= n; i++)
	{
		const uint8_t *e = hdr + 0x60 + i * 0x40;
		if(strncmp((const char *)e, name, 0x20)) continue;

		uint64_t o = 0, s = 0;
		for(int k = 0; k < 8; k++) { o = (o << 8) | e[0x20 + k]; s = (s << 8) | e[0x28 + k]; }
		*off = o;
		*size = s;
		return 1;
	}
	return 0;
}

/* The object of trophy id in tropconf.json / tropmeta: from "id":"0006" to
 * the next "id":". Nested condition objects sit inside, but the fields
 * needed (grade, hidden, name, detail) come before them. */
static int gwm_ucp_object(const char *json, int id, char *out, size_t size)
{
	char key[24];
	const char *p, *q;

	if(!json) return 0;
	snprintf(key, sizeof(key), "\"id\":\"%04d\"", id);
	if(!(p = strstr(json, key))) return 0;

	q = strstr(p + strlen(key), "\"id\":\"");
	size_t len = q ? (size_t)(q - p) : strlen(p);
	if(len >= size) len = size - 1;
	memcpy(out, p, len);
	out[len] = 0;
	return 1;
}

/* Grade, hidden, name and description of a trophy, whatever is there. */
static void gwm_ucp_trophy(const char *conf, const char *meta, int id, gwm_trophy_t *t)
{
	char obj[1024], g[4] = "";

	if(gwm_ucp_object(conf, id, obj, sizeof(obj)))
	{
		gwm_json_get_str(obj, "grade", g, sizeof(g));
		gwm_json_get_bool(obj, "hidden", &t->hidden);
	}
	snprintf(t->grade, sizeof(t->grade), "%s",
	         !strcmp(g, "P") ? "platinum" : !strcmp(g, "G") ? "gold" :
	         !strcmp(g, "S") ? "silver" : !strcmp(g, "B") ? "bronze" : "");

	if(gwm_ucp_object(meta, id, obj, sizeof(obj)))
	{
		gwm_json_get_str(obj, "name", t->name, sizeof(t->name));
		gwm_json_get_str(obj, "detail", t->detail, sizeof(t->detail));
	}
}

/* The game's name from tropmeta. */
static int gwm_ucp_title(const char *meta, char *out, size_t size)
{
	const char *p = meta ? gwm_json_find(meta, "titleMetadata") : NULL;
	return p && gwm_json_get_str(p, "name", out, size) && out[0];
}

/* One queue line: a POST /trophies body with trophies items[start...], as
 * many as fit in out_size. A long set goes as several lines, which the
 * server merges. *next is where the following line starts. Returns the
 * length, or 0 if not even one trophy fits. */
static size_t gwm_t2_build_line(char *out, size_t out_size, const char *account, const char *set_id,
                                const char *title, const char *conf, const char *meta,
                                const gwm_t2_item_t *items, int count, int start, int *next)
{
	char one[GWM_RECORD_MAX], earned[GWM_ISO_LEN];
	char *p = out;
	const char *end = out + out_size;
	int put = 0;

	p = gwm_lit(p, end, "{");
	p = gwm_json_kv(p, end, "account", account);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "platform", GWM_PLATFORM);
	p = gwm_lit(p, end, ",");
	p = gwm_json_kv(p, end, "setId", set_id);
	if(title && title[0])
	{
		p = gwm_lit(p, end, ",");
		p = gwm_json_kv(p, end, "title", title);
	}
	p = gwm_lit(p, end, ",\"trophies\":[");

	int i;
	for(i = start; i < count; i++)
	{
		gwm_trophy_t t;
		char *q = one;
		const char *qe = one + sizeof(one);

		memset(&t, 0, sizeof(t));
		gwm_ucp_trophy(conf, meta, items[i].id, &t);
		gwm_iso_from_unix(earned, items[i].earned);

		q = gwm_lit(q, qe, put ? ",{\"id\":" : "{\"id\":");
		q = gwm_i64(q, qe, items[i].id);
		q = gwm_lit(q, qe, ",");
		q = gwm_json_kv(q, qe, "earnedAt", earned);
		if(t.grade[0])  { q = gwm_lit(q, qe, ","); q = gwm_json_kv(q, qe, "grade", t.grade); }
		if(t.hidden)      q = gwm_lit(q, qe, ",\"hidden\":true");
		if(t.name[0])   { q = gwm_lit(q, qe, ","); q = gwm_json_kv(q, qe, "name", t.name); }
		if(t.detail[0]) { q = gwm_lit(q, qe, ","); q = gwm_json_kv(q, qe, "detail", t.detail); }
		q = gwm_lit(q, qe, "}");

		/* Room for the trophy and the closing "]}". */
		if((size_t)(end - p) <= (size_t)(q - one) + 3) break;
		p = gwm_lit(p, end, one);
		put++;
	}

	p = gwm_lit(p, end, "]}");
	*next = i;
	return put ? (size_t)(p - out) : 0;
}

#ifndef GWM_HOST_TEST

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "notif.h"
#include "queue.h"
#include "user.h"

#define GWM_T2_CONF_DIR "/user/trophy2/nobackup/conf"
#define GWM_T2_SEEN_FILE GWM_DATA_DIR "/trophy2.txt"

/* How often the folders are scanned: a stat per set, and trophies are less
 * urgent than sessions. */
#define GWM_T2_SCAN_SEC 30
/* A file changed just now may still be written; wait for it to settle. */
#define GWM_T2_SETTLE_SEC 5
/* Sets queued per scan: the first run finds the whole history. */
#define GWM_T2_PER_SCAN 10
#define GWM_T2_SETS 2048
#define GWM_T2_FILE_MAX (1024 * 1024)
#define GWM_T2_JSON_MAX (512 * 1024)
#define GWM_T2_LINE_MAX (24 * 1024)   /* < GWM_BATCH_READ: a line must fit in one queue read */

/* Files already processed: uid, set, mtime and size, kept in trophy2.txt.
 * Without it (first run, upgrade) the whole history goes out; the server is
 * idempotent and does not announce old trophies. */
typedef struct { unsigned uid; char set_id[16]; int64_t mtime; int64_t size; } gwm_t2_seen_t;

static gwm_t2_seen_t gwm_t2_seen[GWM_T2_SETS];
static int gwm_t2_seen_n = -1;
static int64_t gwm_t2_next_scan = 0;
static gwm_t2_item_t gwm_t2_items[GWM_T2_MAX];
static char gwm_t2_line[GWM_T2_LINE_MAX];

static void gwm_t2_seen_load(void)
{
	char line[128];
	FILE *f = fopen(GWM_T2_SEEN_FILE, "r");

	gwm_t2_seen_n = 0;
	if(!f) return;

	while(gwm_t2_seen_n < GWM_T2_SETS && fgets(line, sizeof(line), f))
	{
		gwm_t2_seen_t *s = &gwm_t2_seen[gwm_t2_seen_n];
		long long mtime, size;
		if(sscanf(line, "%x %15s %lld %lld", &s->uid, s->set_id, &mtime, &size) != 4) continue;
		s->mtime = mtime;
		s->size = size;
		gwm_t2_seen_n++;
	}
	fclose(f);
}

static void gwm_t2_seen_save(void)
{
	const char *tmp = GWM_T2_SEEN_FILE ".tmp";
	FILE *f = fopen(tmp, "w");

	if(!f) return;
	for(int i = 0; i < gwm_t2_seen_n; i++)
		fprintf(f, "%08x %s %lld %lld\n", gwm_t2_seen[i].uid, gwm_t2_seen[i].set_id,
		        (long long)gwm_t2_seen[i].mtime, (long long)gwm_t2_seen[i].size);
	int failed = fflush(f) != 0 || ferror(f);
	fsync(fileno(f));
	fclose(f);

	if(failed) unlink(tmp);
	else rename(tmp, GWM_T2_SEEN_FILE);
}

static gwm_t2_seen_t *gwm_t2_seen_get(unsigned uid, const char *set_id)
{
	for(int i = 0; i < gwm_t2_seen_n; i++)
		if(gwm_t2_seen[i].uid == uid && !strcmp(gwm_t2_seen[i].set_id, set_id)) return &gwm_t2_seen[i];
	return NULL;
}

/* Reads a whole file (up to max bytes). The caller frees the buffer. */
static uint8_t *gwm_t2_read(const char *path, size_t max, size_t *len)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	uint8_t *buf = NULL;

	if(fd < 0) return NULL;
	if(fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= max && (buf = malloc((size_t)st.st_size)))
	{
		ssize_t got = read(fd, buf, (size_t)st.st_size);
		if(got != st.st_size) { free(buf); buf = NULL; }
		else *len = (size_t)got;
	}
	close(fd);
	return buf;
}

/* File bytes [off, off+size) as a NUL-terminated string. */
static char *gwm_t2_read_at(int fd, uint64_t off, uint64_t size)
{
	char *buf;

	if(!size || size > GWM_T2_JSON_MAX || !(buf = malloc((size_t)size + 1))) return NULL;
	if(pread(fd, buf, (size_t)size, (off_t)off) != (ssize_t)size) { free(buf); return NULL; }
	buf[size] = 0;
	return buf;
}

/* tropconf.json and tropmeta in the set's default language, NULL if
 * missing: the set still goes out, and the server takes names from PSN. */
static void gwm_t2_conf(const char *set_id, char **conf, char **meta)
{
	char path[160], name[64], lang[16] = "";
	uint8_t hdr[0x60];
	uint64_t off, size;

	*conf = *meta = NULL;
	snprintf(path, sizeof(path), GWM_T2_CONF_DIR "/%s/TROPHY.UCP", set_id);

	int fd = open(path, O_RDONLY);
	if(fd < 0) return;

	if(pread(fd, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr))
	{
		uint32_t count = ((uint32_t)hdr[0x10] << 24) | ((uint32_t)hdr[0x11] << 16) | ((uint32_t)hdr[0x12] << 8) | hdr[0x13];
		size_t table = 0x60 + (size_t)(count > 4096 ? 4096 : count) * 0x40;
		uint8_t *toc = malloc(table);

		if(toc && pread(fd, toc, table, 0) == (ssize_t)table)
		{
			if(gwm_ucp_find(toc, table, "tropconf.json", &off, &size))
				*conf = gwm_t2_read_at(fd, off, size);
			if(*conf) gwm_json_get_str(*conf, "defaultLanguage", lang, sizeof(lang));

			snprintf(name, sizeof(name), "tropmeta_%s.json", lang[0] ? lang : "en-US");
			if(gwm_ucp_find(toc, table, name, &off, &size) ||
			   gwm_ucp_find(toc, table, "tropmeta_en-US.json", &off, &size))
				*meta = gwm_t2_read_at(fd, off, size);
		}
		free(toc);
	}
	close(fd);
}

/* One set: parse and queue. 1 when done (queued, or nothing earned), 0 to
 * retry later. */
static int gwm_t2_queue_set(unsigned uid, const char *set_id, const char *path, const char *cfg_account, int *queued)
{
	size_t len = 0;
	uint8_t *d = gwm_t2_read(path, GWM_T2_FILE_MAX, &len);

	if(!d) return 0;

	int n = gwm_t2_parse(d, len, gwm_journal_offset, gwm_t2_items, GWM_T2_MAX);
	free(d);

	/* Unparsable: remember it as is rather than retry every 30 s; the next
	 * change to the file gives another try. */
	if(n < 0) { gwm_log("PS5 trophies %s: file not parsed", set_id); return 1; }
	if(n == 0) return 1;

	char *conf, *meta, title[GWM_TITLE_LEN] = "", account[GWM_ACCOUNT_LEN];
	int ok = 1, lines = 0;

	gwm_t2_conf(set_id, &conf, &meta);
	gwm_ucp_title(meta, title, sizeof(title));
	gwm_user_account(cfg_account, (int)uid, account, sizeof(account));

	for(int start = 0, next = 0; start < n; start = next)
	{
		size_t l = gwm_t2_build_line(gwm_t2_line, sizeof(gwm_t2_line) - 1, account, set_id, title,
		                             conf, meta, gwm_t2_items, n, start, &next);
		if(!l) { gwm_log("PS5 trophies %s: trophy #%d does not fit in a line", set_id, gwm_t2_items[start].id); next = start + 1; continue; }

		gwm_t2_line[l++] = '\n';
		if(gwm_queue_append(GWM_TROPHY_QUEUE, gwm_t2_line, l) != 0)
		{
			gwm_log("PS5 trophies %s: queue refused", set_id);
			ok = 0;
			break;
		}
		lines++;
	}

	free(conf);
	free(meta);

	if(ok)
	{
		gwm_log("PS5 trophies %s \"%s\": %d earned, %d lines queued", set_id, title, n, lines);
		(*queued)++;
	}
	return ok;
}

/* Called on every poll, works once per GWM_T2_SCAN_SEC. Returns the number
 * of sets queued. */
static int gwm_trophy2_poll(const char *cfg_account, int64_t now)
{
	char path[256];
	struct stat st;
	int queued = 0, dirty = 0, more = 0;

	if(now < gwm_t2_next_scan && now > gwm_t2_next_scan - 2 * GWM_T2_SCAN_SEC) return 0;
	gwm_t2_next_scan = now + GWM_T2_SCAN_SEC;

	/* Trophy times are on the journal clock: wait for its offset if there is a journal. */
	if(!gwm_journal_offset_known && stat(GWM_JOURNAL_DB, &st) == 0) return 0;

	if(gwm_t2_seen_n < 0) gwm_t2_seen_load();

	DIR *home = opendir(GWM_USER_HOME);
	if(!home) return 0;

	struct dirent *u;
	while(!more && (u = readdir(home)) != NULL)
	{
		char *e = NULL;
		unsigned long uid = strtoul(u->d_name, &e, 16);
		if(u->d_name[0] == '.' || !e || *e || !uid) continue;

		snprintf(path, sizeof(path), GWM_USER_HOME "/%s/trophy2/nobackup/data", u->d_name);
		DIR *sets = opendir(path);
		if(!sets) continue;

		struct dirent *s;
		while((s = readdir(sets)) != NULL)
		{
			if(!gwm_set_id_valid(s->d_name)) continue;

			snprintf(path, sizeof(path), GWM_USER_HOME "/%s/trophy2/nobackup/data/%s/TRPTITLE.DAT",
			         u->d_name, s->d_name);
			if(stat(path, &st) != 0) continue;

			gwm_t2_seen_t *seen = gwm_t2_seen_get((unsigned)uid, s->d_name);
			if(seen && seen->mtime == (int64_t)st.st_mtime && seen->size == (int64_t)st.st_size) continue;
			if((int64_t)st.st_mtime > now - GWM_T2_SETTLE_SEC && (int64_t)st.st_mtime < now + GWM_T2_SCAN_SEC)
			{
				gwm_t2_next_scan = now + GWM_T2_SETTLE_SEC;
				continue;
			}

			if(queued >= GWM_T2_PER_SCAN) { more = 1; break; }

			if(!seen)
			{
				if(gwm_t2_seen_n >= GWM_T2_SETS)
				{
					static int warned = 0;
					if(!warned++) gwm_log("PS5 trophies: more than %d sets, ignoring the rest", GWM_T2_SETS);
					continue;
				}
				seen = &gwm_t2_seen[gwm_t2_seen_n++];
				memset(seen, 0, sizeof(*seen));
				seen->uid = (unsigned)uid;
				snprintf(seen->set_id, sizeof(seen->set_id), "%s", s->d_name);
				seen->mtime = -1;
			}

			if(gwm_t2_queue_set((unsigned)uid, s->d_name, path, cfg_account, &queued))
			{
				seen->mtime = (int64_t)st.st_mtime;
				seen->size = (int64_t)st.st_size;
				dirty = 1;
			}
		}
		closedir(sets);
	}
	closedir(home);

	if(dirty) gwm_t2_seen_save();
	/* Not everything fit in this scan: run the next one on the next poll. */
	if(more) gwm_t2_next_scan = now;

	return queued;
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_TROPHY2_H */
