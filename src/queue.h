/* On-disk queues: closed sessions and earned trophies.
 *
 * Consoles get switched off at the button and kept offline, so unsent data
 * must survive both. A line is removed only after the server answers.
 *
 * JSON Lines, append only: a power cut loses at most an unfinished last
 * line, which reading drops.
 *
 * Timestamps in the lines are raw console time, uncorrected: offline there
 * is no offset to apply. They are shifted at send time (upload.h).
 *
 * Paths are parameters so the host test can run a queue in a temp folder.
 */

#ifndef GWM_QUEUE_H
#define GWM_QUEUE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A guard against unbounded growth if the server is down for weeks. A
 * megabyte is thousands of sessions, years of play. At the cap the queue
 * stops accepting new lines rather than dropping old ones: what is already
 * queued has survived power cycles and deserves delivery more. */
#define GWM_QUEUE_MAX_BYTES (1024 * 1024)

static long gwm_file_size(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

static int gwm_queue_append(const char *path, const char *line, size_t len)
{
	FILE *f = fopen(path, "a+b");
	if(!f) return -1;

	/* After a power cut the last line may be unfinished. Its tail is cut
	 * before appending, or two JSON objects would merge into one broken. */
	if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long total = ftell(f), complete = total;
	char chunk[4096];
	while(complete > 0)
	{
		long from = complete > (long)sizeof(chunk) ? complete - (long)sizeof(chunk) : 0;
		size_t want = (size_t)(complete - from);
		if(fseek(f, from, SEEK_SET) != 0 || fread(chunk, 1, want, f) != want)
		{ fclose(f); return -1; }
		while(want && chunk[want - 1] != '\n') want--;
		if(want) { complete = from + (long)want; break; }
		complete = from;
	}
	if(complete < 0 || (complete != total && ftruncate(fileno(f), complete) != 0) ||
	   complete + (long)len > GWM_QUEUE_MAX_BYTES || fseek(f, 0, SEEK_END) != 0)
	{ fclose(f); return -1; }

	size_t put = fwrite(line, 1, len, f);
	int failed = (put != len) || ferror(f);
	if(fflush(f) != 0) failed = 1;
	if(fsync(fileno(f)) != 0) failed = 1;
	if(fclose(f) != 0) failed = 1;

	return failed ? -1 : 0;
}

/* Reads the head of the queue up to the last complete newline, so an
 * unfinished line never goes into a batch. Returns the byte count. */
static size_t gwm_queue_read(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	if(!f) { buf[0] = 0; return 0; }

	size_t got = fread(buf, 1, size - 1, f);
	fclose(f);

	while(got && buf[got - 1] != '\n') got--;
	buf[got] = 0;
	return got;
}

/* Drops the first consumed bytes and keeps everything appended since. The
 * rest moves through a temp file and rename, so a power cut mid-way cannot
 * damage what is still unsent. */
static int gwm_queue_consume(const char *path, size_t consumed)
{
	char tmp[256];
	char chunk[4096];

	if(!consumed) return 0;

	if((long)consumed >= gwm_file_size(path))
		return unlink(path);

	FILE *src = fopen(path, "r");
	if(!src) return -1;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *dst = fopen(tmp, "w");
	if(!dst) { fclose(src); return -1; }

	int failed = fseek(src, (long)consumed, SEEK_SET) != 0;
	size_t n;
	while(!failed && (n = fread(chunk, 1, sizeof(chunk), src)) > 0)
		if(fwrite(chunk, 1, n, dst) != n) failed = 1;

	if(fflush(dst) != 0 || ferror(dst)) failed = 1;
	fsync(fileno(dst));
	fclose(src);
	fclose(dst);

	if(failed) { unlink(tmp); return -1; }
	return rename(tmp, path);
}

#endif /* GWM_QUEUE_H */
