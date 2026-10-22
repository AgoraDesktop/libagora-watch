/*
 * EventStream-kqueue.c — the BSD backend: `kqueue` with `EVFILT_VNODE`, one descriptor per watched path.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * WRITTEN, AND NOT YET MEASURED ON A BSD — this tree is developed on Linux, where the suite exercises the
 * `inotify` backend and this file is not compiled. It is kept beside that one and selected by the build on
 * FreeBSD, NetBSD and OpenBSD, and it is said plainly here rather than implied that a BSD run is the thing
 * that would turn "written" into "measured". `kqueue` is the kernel's own interface and, like `inotify`,
 * needs no systemd and nothing linked.
 *
 * WHAT A DIRECTORY GIVES, AND WHY IT IS COARSER. `NOTE_WRITE` on a directory says its ENTRIES changed but
 * not WHICH — so a directory event is reported with `MustScanSubDirs`, which is exactly the 10.6 FSEvents
 * model: the consumer rescans. A FILE's descriptor, by contrast, distinguishes modified, deleted, renamed
 * and attribute changes, so a watched FILE gets the granular flags. This is the platform's honesty, stated
 * rather than papered over.
 */
#if !(defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#error "EventStream-kqueue.c is the BSD backend; Linux builds EventStream-inotify.c."
#endif

#include "EventStream_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { int fd; char *path; bool isDir; } AGKqWatch;
typedef struct { int kq; AGKqWatch *watches; size_t count, cap; } AGKqueue;

static void addWatch(AGKqueue *st, const char *path, bool isDir)
{
	int fd = open(path, O_RDONLY | (isDir ? O_DIRECTORY : 0));
	if (fd < 0) return;
	struct kevent change;
	EV_SET(&change, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
	       NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_REVOKE,
	       0, NULL);
	if (kevent(st->kq, &change, 1, NULL, 0, NULL) < 0) { close(fd); return; }
	if (st->count == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 16;
		st->watches = realloc(st->watches, st->cap * sizeof(*st->watches));
	}
	st->watches[st->count].fd = fd;
	st->watches[st->count].path = strdup(path);
	st->watches[st->count].isDir = isDir;
	st->count++;
}

static void addRecursive(AGKqueue *st, const char *path)
{
	addWatch(st, path, true);
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

static AGKqWatch *watchForFd(AGKqueue *st, int fd)
{
	for (size_t i = 0; i < st->count; i++) if (st->watches[i].fd == fd) return &st->watches[i];
	return NULL;
}

int _AGWatchBackendOpen(AGEventStream *stream)
{
	AGKqueue *st = calloc(1, sizeof(*st));
	if (st == NULL) return -1;
	st->kq = kqueue();
	if (st->kq < 0) { free(st); return -1; }

	CFIndex n = CFArrayGetCount(stream->paths);
	for (CFIndex i = 0; i < n; i++) {
		CFStringRef path = CFArrayGetValueAtIndex(stream->paths, i);
		if (path == NULL || CFGetTypeID(path) != CFStringGetTypeID()) continue;
		char buf[4096];
		if (!CFStringGetFileSystemRepresentation(path, buf, sizeof buf)) continue;
		struct stat sb;
		if (stat(buf, &sb) == 0 && S_ISDIR(sb.st_mode)) addRecursive(st, buf);
		else addWatch(st, buf, false);
	}
	stream->backend = st;
	return st->kq;
}

void _AGWatchBackendDrain(AGEventStream *stream)
{
	AGKqueue *st = stream->backend;
	if (st == NULL) return;

	struct timespec zero = { 0, 0 };
	struct kevent events[64];
	int n;
	while ((n = kevent(st->kq, NULL, 0, events, 64, &zero)) > 0) {
		for (int i = 0; i < n; i++) {
			struct kevent *ev = &events[i];
			if (ev->filter != EVFILT_VNODE) continue;
			AGKqWatch *w = watchForFd(st, (int)ev->ident);
			if (w == NULL) continue;

			FSEventStreamEventFlags flags = 0;
			if (ev->fflags & NOTE_WRITE)  flags |= w->isDir ? kFSEventStreamEventFlagMustScanSubDirs
			                                                : kFSEventStreamEventFlagItemModified;
			if (ev->fflags & NOTE_EXTEND) flags |= kFSEventStreamEventFlagItemModified;
			if (ev->fflags & NOTE_DELETE) flags |= kFSEventStreamEventFlagItemRemoved;
			if (ev->fflags & NOTE_RENAME) flags |= kFSEventStreamEventFlagItemRenamed;
			if (ev->fflags & NOTE_ATTRIB) flags |= kFSEventStreamEventFlagItemInodeMetaMod;
			if (flags == 0) continue;
			flags |= w->isDir ? kFSEventStreamEventFlagItemIsDir : kFSEventStreamEventFlagItemIsFile;
			_AGWatchRecord(stream, w->path, flags);
		}
	}
}

void _AGWatchBackendClose(AGEventStream *stream)
{
	AGKqueue *st = stream->backend;
	if (st == NULL) return;
	for (size_t i = 0; i < st->count; i++) { close(st->watches[i].fd); free(st->watches[i].path); }
	free(st->watches);
	if (st->kq >= 0) close(st->kq);
	free(st);
	stream->backend = NULL;
}
