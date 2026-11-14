/* Minimal host test helpers: a check that reports where it failed, and a summary. */

#ifndef GWM_CHECK_H
#define GWM_CHECK_H

#include <stdio.h>
#include <string.h>

static int gwm_failures = 0;

#define CHECK(cond) do { \
	if(!(cond)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); gwm_failures++; } \
} while(0)

#define CHECK_STR(got, want) do { \
	const char *g_ = (got), *w_ = (want); \
	if(strcmp(g_, w_) != 0) { \
		fprintf(stderr, "%s:%d: got  \"%s\"\n%*s want \"%s\"\n", __FILE__, __LINE__, g_, \
		        (int)strlen(__FILE__) + 6, "", w_); \
		gwm_failures++; \
	} \
} while(0)

static int gwm_report(const char *name)
{
	if(gwm_failures) { fprintf(stderr, "%s: %d FAILED\n", name, gwm_failures); return 1; }
	printf("%s: OK\n", name);
	return 0;
}

#endif /* GWM_CHECK_H */
