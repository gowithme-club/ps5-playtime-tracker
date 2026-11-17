/* The gowithme.club tile in the Media section of the home screen.
 *
 * A tile is a registered app folder with nothing to run: /user/app/<id>/sce_sys
 * holds param.json and icon0.png, and the param's deeplinkUri makes the system
 * open its browser on the page the payload serves (web.h). Payload Manager,
 * BFpilot and LiveShell put their tiles there the same way; no PKG is
 * involved. Registration is sceAppInstUtilAppInstallTitleDir on /user/app.
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

/* 1 when the file holds exactly these bytes. */
static int gwm_tile_same(const char *path, const void *data, size_t size)
{
	struct stat st;
	if(stat(path, &st) != 0 || (size_t)st.st_size != size) return 0;

	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	char *buf = malloc(size ? size : 1);
	int same = buf && fread(buf, 1, size, f) == size && !memcmp(buf, data, size);
	free(buf);
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

	int folder = stat(GWM_TILE_DIR, &st) == 0;
	int match = folder && gwm_tile_same(GWM_TILE_PARAM, param, plen) && gwm_tile_same(GWM_TILE_ICON, gwm_icon_png, ilen);
	int plan = gwm_tile_plan(folder, match, stat(GWM_TILE_MARK, &st) == 0);

	if(plan == GWM_TILE_KEEP) return NULL;
	if(plan == GWM_TILE_DELETED)
	{
		gwm_log("tile: deleted from the home screen, not adding it again");
		return NULL;
	}

	mkdir(GWM_TILE_DIR, 0755);
	mkdir(GWM_TILE_SYS, 0755);
	if(gwm_tile_write(GWM_TILE_PARAM, param, plen) || gwm_tile_write(GWM_TILE_ICON, gwm_icon_png, ilen))
	{
		gwm_log("tile: cannot write " GWM_TILE_SYS " (errno %d)", errno);
		return NULL;
	}

	int rc = sceAppInstUtilInitialize();
	if(rc)
	{
		gwm_log("tile: sceAppInstUtilInitialize -> 0x%08x", rc);
		return NULL;
	}
	rc = gwm_tile_install();
	sceAppInstUtilTerminate();

	if(rc)
	{
		gwm_log("tile: install " GWM_TILE_ID " -> 0x%08x", rc);
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
