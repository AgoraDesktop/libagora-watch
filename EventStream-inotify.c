/*
 * EventStream-inotify.c — the Linux backend: `inotify`, with a watch added for every directory under each
 * watched path so the tree is live.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THERE IS NO SYSTEMD HERE, AND NOTHING TO LINK: `inotify` is the kernel's own interface, reached through
 * `inotify_init1` and `read`. The event vocabulary does not line up with FSEvents' one for one, so each
 * `IN_*` bit is translated to the flag with the nearest meaning, and a change to a DIRECTORY that arrives
 * without a name (the watched directory itself moved or went away) is reported against the directory.
 *
 * RECURSION IS THE BACKEND'S JOB. `inotify` watches one directory at a time, so a directory that appears
 * inside a watched tree is itself watched the moment it is seen to exist — which is what makes a file
 * created in a NEW subdirectory arrive as a live event and not as a miss.
 */
#ifndef __linux__
#error "EventStream-inotify.c is the Linux backend; the BSDs build EventStream-kqueue.c."
#endif

#include "EventStream_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define AG_WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | \
                       IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF)

typedef struct { int wd; char *path; } AGWatchDir;
typedef struct { int fd; AGWatchDir *dirs; size_t count, cap; } AGInotify;

static const char *lookupWd(AGInotify *st, int wd)
{
	for (size_t i = 0; i < st->count; i++) if (st->dirs[i].wd == wd) return st->dirs[i].path;
	return NULL;
}

static void addWatch(AGInotify *st, const char *path)
{
	int wd = inotify_add_watch(st->fd, path, AG_WATCH_MASK);
	if (wd < 0) return;
	for (size_t i = 0; i < st->count; i++) {
		if (st->dirs[i].wd == wd) { free(st->dirs[i].path); st->dirs[i].path = strdup(path); return; }
	}
	if (st->count == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 16;
		st->dirs = realloc(st->dirs, st->cap * sizeof(*st->dirs));
	}
	st->dirs[st->count].wd = wd;
	st->dirs[st->count].path = strdup(path);
	st->count++;
}

static void removeWatch(AGInotify *st, int wd)
{
	for (size_t i = 0; i < st->count; i++) {
		if (st->dirs[i].wd != wd) continue;
		free(st->dirs[i].path);
		memmove(&st->dirs[i], &st->dirs[i + 1], (st->count - i - 1) * sizeof(*st->dirs));
		st->count--;
		return;
	}
}

static void addRecursive(AGInotify *st, const char *path)
{
	addWatch(st, path);
	DIR *d = opendir(path);
	if (d == NULL) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char child[4096];
		snprintf(child, sizeof child, "%s/%s", path, e->d_name);
		struct stat sb;
		if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) addRecursive(st, child);
	}
	closedir(d);
}

int _AGWatchBackendOpen(AGEventStream *stream)
{
	AGInotify *st = calloc(1, sizeof(*st));
	if (st == NULL) return -1;
	st->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (st->fd < 0) { free(st); return -1; }

	CFIndex n = CFArrayGetCount(stream->paths);
	for (CFIndex i = 0; i < n; i++) {
		CFStringRef path = CFArrayGetValueAtIndex(stream->paths, i);
		if (path == NULL || CFGetTypeID(path) != CFStringGetTypeID()) continue;
		char buf[4096];
		if (!CFStringGetFileSystemRepresentation(path, buf, sizeof buf)) continue;
		struct stat sb;
		if (stat(buf, &sb) == 0 && S_ISDIR(sb.st_mode)) addRecursive(st, buf);
		else addWatch(st, buf);
	}
	stream->backend = st;
	return st->fd;
}

void _AGWatchBackendDrain(AGEventStream *stream)
{
	AGInotify *st = stream->backend;
	if (st == NULL) return;

	char buf[65536];
	for (;;) {
		ssize_t n = read(st->fd, buf, sizeof buf);
		if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
		for (char *p = buf; p < buf + n; ) {
			struct inotify_event *e = (struct inotify_event *)p;
			size_t elen = sizeof(struct inotify_event) + e->len;
			if (e->mask & IN_IGNORED) { removeWatch(st, e->wd); p += elen; continue; }

			const char *dir = lookupWd(st, e->wd);
			if (dir != NULL) {
				char path[4096];
				if (e->len > 0 && e->name[0] != '\0') snprintf(path, sizeof path, "%s/%s", dir, e->name);
				else snprintf(path, sizeof path, "%s", dir);

				FSEventStreamEventFlags flags = 0;
				if (e->mask & IN_CREATE)     flags |= kFSEventStreamEventFlagItemCreated;
				if (e->mask & IN_DELETE)     flags |= kFSEventStreamEventFlagItemRemoved;
				if (e->mask & IN_MOVED_FROM) flags |= kFSEventStreamEventFlagItemRenamed;
				if (e->mask & IN_MOVED_TO)   flags |= kFSEventStreamEventFlagItemRenamed;
				if (e->mask & (IN_MODIFY | IN_CLOSE_WRITE)) flags |= kFSEventStreamEventFlagItemModified;
				if (e->mask & IN_ATTRIB)     flags |= kFSEventStreamEventFlagItemInodeMetaMod;
				if (e->mask & IN_DELETE_SELF) flags |= kFSEventStreamEventFlagItemRemoved;
				if (e->mask & IN_MOVE_SELF)   flags |= kFSEventStreamEventFlagItemRenamed;
				flags |= (e->mask & IN_ISDIR) ? kFSEventStreamEventFlagItemIsDir
				                              : kFSEventStreamEventFlagItemIsFile;
				if (flags != 0) _AGWatchRecord(stream, path, flags);

				if ((e->mask & IN_CREATE) && (e->mask & IN_ISDIR)) addRecursive(st, path);
			}
			p += elen;
		}
	}
}

void _AGWatchBackendClose(AGEventStream *stream)
{
	AGInotify *st = stream->backend;
	if (st == NULL) return;
	if (st->fd >= 0) close(st->fd);
	for (size_t i = 0; i < st->count; i++) free(st->dirs[i].path);
	free(st->dirs);
	free(st);
	stream->backend = NULL;
}
