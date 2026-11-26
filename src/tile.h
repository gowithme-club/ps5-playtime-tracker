/* The gowithme.club tile in the Media section of the home screen.
 *
 * A tile is a registered app folder with nothing to run: /user/app/<id>/sce_sys
 * holds param.json and icon0.png, and the param's deeplinkUri makes the system
 * open its browser on the page the payload serves (web.h). Payload Manager,
 * BFpilot and LiveShell put their tiles there the same way; no PKG is
 * involved. Registration is sceAppInstUtilAppInstallTitleDir on /user/app.
 *
 * pic0.dds is what the home screen shows behind the focused tile, as with a
 * game's art: 3840x2160 BC7 behind a DX10 DDS header. On 13.40 a pic1.dds or
 * a .png showed nothing; pic0.dds did once it was in sce_sys and in
 * /user/appmeta/<id>, the copy pkg installs keep and AppInstallAll does not
 * fill for a /user/app folder, so it is written to both. param.json has no
 * subtitle, so what the service is goes on the background, on the right: the
 * shell dims the art under its title on the left to about a third.
 *
 * The .dds is 8 MB of flat colour and repeated cells, so the payload carries
 * it packed (assets/pic0.dds.rle, about 50 KB) and unpacks it when it writes
 * it.
 *
 * The tile is written once and registered again only when its files change,
 * as with a new icon or port. "Delete" in the tile's menu removes the folder;
 * a payload that put the tile back on every start would overrule the person,
 * so tile.txt in the data folder remembers that the tile was registered, and a
 * folder missing after that means it was deleted on purpose.
 *
 * The system calls can take seconds, and libSceAppInstUtil, like the user
 * service, has not been seen from elfldr: everything runs in a detached thread
 * the main loop never waits for.
 *
 * The param text and the decision are pure, covered by a host test.
 */

#ifndef GWM_TILE_H
#define GWM_TILE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "version.h"

#define GWM_TILE_ID "GWMC00001"

/* Media section: the category Payload Manager's and BFpilot's tiles use. */
#define GWM_TILE_CATEGORY 65536

/* pic0.dds: the DX10 DDS header, then one byte a pixel. */
#define GWM_TILE_PIC_HEAD 148
#define GWM_TILE_PIC_SIZE (GWM_TILE_PIC_HEAD + 3840 * 2160)

static uint32_t gwm_tile_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Unpacks 16-byte blocks after a GWM_TILE_PIC_HEAD-byte header. Each record
 * starts with a little-endian u32 n; its top two bits say what the low 30
 * count: 10 repeats the one block after it, 01 copies blocks from d blocks
 * back (a u32 d follows; the copy may overlap what it writes), 00 is that
 * many blocks as they are. Returns the bytes written, 0 if the input is cut
 * short, points before the start or would not fit. */
static size_t gwm_tile_unpack(const unsigned char *src, size_t len, unsigned char *dst, size_t cap)
{
	if(len < GWM_TILE_PIC_HEAD || cap < GWM_TILE_PIC_HEAD) return 0;
	memcpy(dst, src, GWM_TILE_PIC_HEAD);
	size_t in = GWM_TILE_PIC_HEAD, out = GWM_TILE_PIC_HEAD;

	while(in < len)
	{
		if(len - in < 4) return 0;
		uint32_t n = gwm_tile_u32(src + in), kind = n >> 30, count = n & 0x3fffffffu;
		in += 4;
		if(count == 0 || kind == 3 || (cap - out) / 16 < count) return 0;

		if(kind == 1)
		{
			if(len - in < 4) return 0;
			size_t back = (size_t)gwm_tile_u32(src + in) * 16;
			in += 4;
			if(back == 0 || back > out - GWM_TILE_PIC_HEAD) return 0;
			for(uint32_t i = 0; i < count; i++, out += 16) memcpy(dst + out, dst + out - back, 16);
			continue;
		}

		size_t take = kind == 2 ? 16 : (size_t)count * 16;
		if(len - in < take) return 0;
		for(uint32_t i = 0; i < count; i++, out += 16)
			memcpy(dst + out, src + in + (kind == 2 ? 0 : (size_t)i * 16), 16);
		in += take;
	}
	return out;
}

static size_t gwm_tile_param(char *out, size_t size, unsigned port)
{
	int n = snprintf(out, size,
		"{\"titleId\":\"" GWM_TILE_ID "\","
		"\"applicationCategoryType\":%d,"
		"\"deeplinkUri\":\"http://127.0.0.1:%u/\","
		"\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
		"\"en-US\":{\"titleName\":\"" GWM_PRODUCT "\"}}}\n",
		GWM_TILE_CATEGORY, port);
	return (n < 0 || (size_t)n >= size) ? 0 : (size_t)n;
}

enum { GWM_TILE_KEEP, GWM_TILE_REGISTER, GWM_TILE_DELETED };

/* What to do given what is on disk: the folder, whether its files are the
 * ones this build writes, and whether the tile was registered before. Files
 * without a registration are a register that failed: try again. */
static int gwm_tile_plan(int folder, int files_match, int registered_before)
{
	if(!folder) return registered_before ? GWM_TILE_DELETED : GWM_TILE_REGISTER;
	return files_match && registered_before ? GWM_TILE_KEEP : GWM_TILE_REGISTER;
}

#ifndef GWM_HOST_TEST

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "asset.h"
#include "config.h"
#include "log.h"

#define GWM_TILE_DIR    "/user/app/" GWM_TILE_ID
#define GWM_TILE_SYS    GWM_TILE_DIR "/sce_sys"
#define GWM_TILE_PARAM  GWM_TILE_SYS "/param.json"
#define GWM_TILE_ICON   GWM_TILE_SYS "/icon0.png"
#define GWM_TILE_PIC    GWM_TILE_SYS "/pic0.dds"
#define GWM_TILE_META   "/user/appmeta/" GWM_TILE_ID
#define GWM_TILE_META_PIC GWM_TILE_META "/pic0.dds"
#define GWM_TILE_MARK   GWM_DATA_DIR "/tile.txt"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *reserved);

/* 13.40 no longer exports sceAppInstUtilAppInstallTitleDir: an ELF importing
 * it by name is refused at load ("Unable to resolve") and never reaches main,
 * and the NID Payload Manager tries (Wudg3Xe3heE) is not there either. It is
 * looked up at run time for older firmware; without it, AppInstallAll
 * installs every folder in /user/app, ours among them. That is how Payload
 * Manager, PS5Upload and ShadowMount+ register on 13.x, and it runs only when
 * the tile needs registering. */
#define GWM_NID_INSTALL_TITLE_DIR "Wudg3Xe3heE"

/* Whether the tile is on the home screen, for the pairing notifications
 * (pair.h): 0 once it is known to be deleted or failed to register. Written
 * by the tile thread, read by the main loop. */
static volatile int gwm_tile_shown = 1;

static int gwm_tile_install(void)
{
	int (*install_dir)(const char *, const char *, void *) = NULL;
	uint32_t handle = 0;
	pid_t pid = getpid();

	if(!kernel_dynlib_handle(pid, "libSceAppInstUtil.sprx", &handle))
		install_dir = (void *)kernel_dynlib_resolve(pid, handle, GWM_NID_INSTALL_TITLE_DIR);

	if(install_dir) return install_dir(GWM_TILE_ID, "/user/app/", NULL);
	return sceAppInstUtilAppInstallAll(NULL);
}

/* 1 when the file holds exactly these bytes. Read in pieces: the
 * background is 8 MB. */
static int gwm_tile_same(const char *path, const void *data, size_t size)
{
	struct stat st;
	if(stat(path, &st) != 0 || (size_t)st.st_size != size) return 0;

	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	char buf[16384];
	size_t at = 0;
	int same = 1;
	while(same && at < size)
	{
		size_t want = size - at < sizeof(buf) ? size - at : sizeof(buf);
		same = fread(buf, 1, want, f) == want && !memcmp(buf, (const char *)data + at, want);
		at += want;
	}
	fclose(f);
	return same;
}

static int gwm_tile_write(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if(!f) return -1;
	int failed = fwrite(data, 1, size, f) != size;
	if(fclose(f) != 0) failed = 1;
	return failed ? -1 : 0;
}

static void *gwm_tile_worker(void *arg)
{
	unsigned port = (unsigned)(uintptr_t)arg;
	char param[512];
	size_t plen = gwm_tile_param(param, sizeof(param), port);
	size_t ilen = (size_t)(gwm_icon_png_end - gwm_icon_png);
	struct stat st;
	int folder = stat(GWM_TILE_DIR, &st) == 0, registered_before = stat(GWM_TILE_MARK, &st) == 0;
	if(!folder && registered_before) gwm_tile_shown = 0;

	unsigned char *pic = malloc(GWM_TILE_PIC_SIZE);
	size_t piclen = pic ? gwm_tile_unpack(gwm_pic0, (size_t)(gwm_pic0_end - gwm_pic0), pic, GWM_TILE_PIC_SIZE) : 0;
	if(piclen != GWM_TILE_PIC_SIZE)
	{
		gwm_log("tile: no background (%zu bytes)", piclen);
		free(pic);
		return NULL;
	}

	int match = folder && gwm_tile_same(GWM_TILE_PARAM, param, plen) && gwm_tile_same(GWM_TILE_ICON, gwm_icon_png, ilen)
		&& gwm_tile_same(GWM_TILE_PIC, pic, piclen) && gwm_tile_same(GWM_TILE_META_PIC, pic, piclen);
	int plan = gwm_tile_plan(folder, match, registered_before);

	if(plan != GWM_TILE_REGISTER)
	{
		if(plan == GWM_TILE_DELETED)
		{
			/* The system's delete clears appmeta as well; this covers a
			 * folder removed some other way. */
			unlink(GWM_TILE_META_PIC);
			rmdir(GWM_TILE_META);
			gwm_log("tile: deleted from the home screen, not adding it again");
		}
		free(pic);
		return NULL;
	}

	mkdir(GWM_TILE_DIR, 0755);
	mkdir(GWM_TILE_SYS, 0755);
	mkdir(GWM_TILE_META, 0755);
	int failed = gwm_tile_write(GWM_TILE_PARAM, param, plen) || gwm_tile_write(GWM_TILE_ICON, gwm_icon_png, ilen)
		|| gwm_tile_write(GWM_TILE_PIC, pic, piclen) || gwm_tile_write(GWM_TILE_META_PIC, pic, piclen);
	free(pic);
	if(failed)
	{
		gwm_log("tile: cannot write " GWM_TILE_SYS " (errno %d)", errno);
		if(!folder) gwm_tile_shown = 0;
		return NULL;
	}

	int rc = sceAppInstUtilInitialize();
	if(rc)
	{
		gwm_log("tile: sceAppInstUtilInitialize -> 0x%08x", rc);
		if(!folder) gwm_tile_shown = 0;
		return NULL;
	}
	rc = gwm_tile_install();
	sceAppInstUtilTerminate();

	if(rc)
	{
		gwm_log("tile: install " GWM_TILE_ID " -> 0x%08x", rc);
		if(!folder) gwm_tile_shown = 0;
		return NULL;
	}

	FILE *f = fopen(GWM_TILE_MARK, "w");
	if(f)
	{
		fprintf(f, "%s registered by %s\n", GWM_TILE_ID, GWM_PRODUCT_VERSION);
		fclose(f);
	}
	gwm_log("tile: " GWM_TILE_ID " %s, page on port %u", folder ? "updated" : "added", port);
	return NULL;
}

/* The icon the notifications show (log.h), kept apart from the tile so a
 * deleted tile does not take it along. */
static void gwm_icon_write(void)
{
	size_t len = (size_t)(gwm_icon_png_end - gwm_icon_png);
	if(!gwm_tile_same(GWM_NOTIFY_ICON, gwm_icon_png, len)) gwm_tile_write(GWM_NOTIFY_ICON, gwm_icon_png, len);
}

static void gwm_tile_start(unsigned port)
{
	pthread_t th;
	if(pthread_create(&th, NULL, gwm_tile_worker, (void *)(uintptr_t)port) == 0) pthread_detach(th);
	else gwm_log("tile: no thread");
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_TILE_H */
