/*
 * eventstream.c — F3's measurement.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS HOLDS UP, in WORKSPACE-PLAN.md's words for F3: the stream reports create, remove and rename;
 * and a burst is COALESCED into one event. It also shows the two things the plan singled out as the reason
 * the component exists — a startup that watches the whole tree, and a NEW subdirectory being watched the
 * moment it appears, so a file made inside it is not a miss.
 *
 * THE MUTATIONS ARE MADE BY A SHELL. A change made by another process is what a file manager actually
 * watches for; the suite waits out the latency window between a mutation and its assertion.
 */
#define _DEFAULT_SOURCE

#include <Watch/EventStream.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_EVENTS 8192

typedef struct { char path[4096]; FSEventStreamEventFlags flags; } AGEvent;
static AGEvent g_events[MAX_EVENTS];
static size_t g_eventCount = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_failures = 0;

static void check(bool condition, const char *what)
{
	if (!condition) g_failures++;
	printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
}

static void callback(ConstFSEventStreamRef stream, void *info, size_t numEvents,
                     void *eventPaths, const FSEventStreamEventFlags eventFlags[],
                     const FSEventStreamEventId eventIds[])
{
	(void)stream; (void)info; (void)eventIds;
	char **paths = eventPaths;
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < numEvents && g_eventCount < MAX_EVENTS; i++) {
		snprintf(g_events[g_eventCount].path, sizeof g_events[0].path, "%s", paths[i]);
		g_events[g_eventCount].flags = eventFlags[i];
		g_eventCount++;
	}
	pthread_mutex_unlock(&g_mutex);
}

static bool hasEvent(const char *suffix, FSEventStreamEventFlags flag)
{
	size_t sl = strlen(suffix);
	pthread_mutex_lock(&g_mutex);
	bool found = false;
	for (size_t i = 0; i < g_eventCount && !found; i++) {
		size_t l = strlen(g_events[i].path);
		if (l >= sl && strcmp(g_events[i].path + l - sl, suffix) == 0 && (g_events[i].flags & flag)) found = true;
	}
	pthread_mutex_unlock(&g_mutex);
	return found;
}

static size_t countSuffix(const char *suffix)
{
	size_t sl = strlen(suffix), count = 0;
	pthread_mutex_lock(&g_mutex);
	for (size_t i = 0; i < g_eventCount; i++) {
		size_t l = strlen(g_events[i].path);
		if (l >= sl && strcmp(g_events[i].path + l - sl, suffix) == 0) count++;
	}
	pthread_mutex_unlock(&g_mutex);
	return count;
}

static void resetEvents(void)
{
	pthread_mutex_lock(&g_mutex);
	g_eventCount = 0;
	pthread_mutex_unlock(&g_mutex);
}

static void mutate(const char *format, ...)
{
	char command[4096];
	va_list ap;
	va_start(ap, format);
	vsnprintf(command, sizeof command, format, ap);
	va_end(ap);
	(void)system(command);
	usleep(300000);			/* let the reader drain and the latency window close */
}

int main(void)
{
	char tmpl[] = "/tmp/agora-watch-XXXXXX";
	char *root = mkdtemp(tmpl);
	if (root == NULL) { perror("mkdtemp"); return 1; }

	CFStringRef rootPath = CFStringCreateWithCString(NULL, root, kCFStringEncodingUTF8);
	const void *values[1] = { rootPath };
	CFArrayRef paths = CFArrayCreate(NULL, values, 1, &kCFTypeArrayCallBacks);
	FSEventStreamContext context = { 0 };

	printf("eventstream: %s\n", root);

	FSEventStreamRef stream = FSEventStreamCreate(NULL, callback, &context, paths,
	                                              kFSEventStreamEventIdSinceNow, 0.05,
	                                              kFSEventStreamCreateFlagFileEvents);
	check(stream != NULL, "a stream is created for a directory");
	check(FSEventStreamStart(stream), "and it starts");

	/* The one door Apple has that this version does not honour, and it says so. */
	check(!FSEventStreamSetDispatchQueue(stream, NULL),
	      "SetDispatchQueue reports that it is not honoured (delivery is on the reader thread)");

	mutate("touch '%s/a'", root);
	check(hasEvent("/a", kFSEventStreamEventFlagItemCreated), "creating a file is reported (ItemCreated)");

	mutate("echo hello > '%s/b'", root);
	check(hasEvent("/b", kFSEventStreamEventFlagItemCreated), "creating b is reported");
	check(hasEvent("/b", kFSEventStreamEventFlagItemModified), "and writing it is reported (ItemModified)");

	mutate("mv '%s/b' '%s/c'", root, root);
	check(hasEvent("/c", kFSEventStreamEventFlagItemRenamed),
	      "renaming b to c is reported at the destination (ItemRenamed)");

	mutate("rm '%s/a'", root);
	check(hasEvent("/a", kFSEventStreamEventFlagItemRemoved), "removing a is reported (ItemRemoved)");

	/* ---- coalescing: forty writes to one file are not forty events ------------------------ */
	resetEvents();
	mutate("for i in $(seq 1 40); do echo $i >> '%s/f'; done", root);
	size_t fcount = countSuffix("/f");
	printf("  (note: forty writes to one file produced %zu event(s))\n", fcount);
	check(fcount >= 1, "writing one file forty times is reported at least once");
	check(fcount < 40, "and is COALESCED: fewer events than writes");

	/* ---- a new subdirectory is watched the moment it appears ------------------------------ */
	mutate("mkdir '%s/sub'", root);
	mutate("touch '%s/sub/z'", root);
	check(hasEvent("/sub/z", kFSEventStreamEventFlagItemCreated),
	      "a file in a NEW subdirectory is reported, so the tree stays live");

	FSEventStreamFlushSync(stream);

	FSEventStreamStop(stream);
	FSEventStreamRelease(stream);
	CFRelease(paths);
	CFRelease(rootPath);

	char cleanup[4096];
	snprintf(cleanup, sizeof cleanup, "rm -rf '%s'", root);
	(void)system(cleanup);

	printf("eventstream: %d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
