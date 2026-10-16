/*
 * EventStream.c — the engine: the reader thread, the coalescing, and the delivery.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE ENGINE KNOWS NOTHING ABOUT INOTIFY OR KQUEUE. A backend opens a descriptor and calls `_AGWatchRecord`
 * for each change it reads; this file merges those changes into a PENDING batch keyed by path, and delivers
 * the batch to the callback when the latency window closes — or at once, when the stream was created with
 * `kFSEventStreamCreateFlagNoDefer` or the latency is zero.
 *
 * COALESCING IS PER PATH, WHICH IS FSEVENTS' OWN CONTRACT: fifty writes to one file in the window are one
 * event for that file, not fifty. That is what makes a view live without making it busy.
 */
#include "EventStream_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The granular flags that mark a record as belonging to an ITEM rather than a directory. Used to decide
 * whether the 10.6 coarsening applies. */
static const FSEventStreamEventFlags kItemFlags =
	kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved |
	kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemModified |
	kFSEventStreamEventFlagItemInodeMetaMod;

void _AGWatchRecord(AGEventStream *stream, const char *path, FSEventStreamEventFlags flags)
{
	if (stream == NULL || path == NULL || flags == 0) return;

	char coarsened[4096];
	const char *use = path;
	FSEventStreamEventFlags effective = flags;

	/* WITHOUT `FileEvents`, AN ITEM CHANGE IS REPORTED AT ITS DIRECTORY — the 10.6 model: the consumer is
	 * told the directory changed and rescans it. */
	if (!(stream->createFlags & kFSEventStreamCreateFlagFileEvents) && (flags & kItemFlags)) {
		snprintf(coarsened, sizeof coarsened, "%s", path);
		char *slash = strrchr(coarsened, '/');
		if (slash != NULL) { if (slash == coarsened) coarsened[1] = '\0'; else *slash = '\0'; }
		use = coarsened;
		effective = kFSEventStreamEventFlagMustScanSubDirs |
		            ((flags & kFSEventStreamEventFlagItemIsDir) ? kFSEventStreamEventFlagItemIsDir
		                                                        : kFSEventStreamEventFlagItemIsFile);
	}

	pthread_mutex_lock(&stream->mutex);
	for (size_t i = 0; i < stream->pendingCount; i++) {
		if (strcmp(stream->pending[i].path, use) == 0) {
			stream->pending[i].flags |= effective;
			pthread_mutex_unlock(&stream->mutex);
			return;
		}
	}
	if (stream->pendingCount == stream->pendingCap) {
		stream->pendingCap = stream->pendingCap ? stream->pendingCap * 2 : 16;
		stream->pending = realloc(stream->pending, stream->pendingCap * sizeof(*stream->pending));
	}
	stream->pending[stream->pendingCount].path = strdup(use);
	stream->pending[stream->pendingCount].flags = effective;
	stream->pendingCount++;
	pthread_mutex_unlock(&stream->mutex);
}

static void deliverPending(AGEventStream *stream)
{
	pthread_mutex_lock(&stream->mutex);
	size_t n = stream->pendingCount;
	char **paths = NULL;
	FSEventStreamEventFlags *flags = NULL;
	FSEventStreamEventId *ids = NULL;
	if (n > 0) {
		paths = calloc(n, sizeof(*paths));
		flags = calloc(n, sizeof(*flags));
		ids = calloc(n, sizeof(*ids));
		for (size_t i = 0; i < n; i++) {
			paths[i] = stream->pending[i].path;
			flags[i] = stream->pending[i].flags;
			ids[i] = stream->nextId++;
			stream->pending[i].path = NULL;
		}
	}
	stream->pendingCount = 0;
	bool flush = stream->flushRequested;
	pthread_mutex_unlock(&stream->mutex);

	if (n > 0 && stream->callback != NULL)
		stream->callback(stream, stream->context.info, n, paths, flags, ids);

	if (paths != NULL) for (size_t i = 0; i < n; i++) free(paths[i]);
	free(paths);
	free(flags);
	free(ids);

	if (flush) {
		pthread_mutex_lock(&stream->mutex);
		stream->flushRequested = false;
		pthread_cond_broadcast(&stream->cond);
		pthread_mutex_unlock(&stream->mutex);
	}
}

static void *readerMain(void *argument)
{
	AGEventStream *stream = argument;
	for (;;) {
		pthread_mutex_lock(&stream->mutex);
		bool stop = stream->stopRequested;
		bool hasPending = stream->pendingCount > 0;
		bool flush = stream->flushRequested;
		pthread_mutex_unlock(&stream->mutex);
		if (stop) break;

		if (flush || (hasPending && stream->latency <= 0.0)) { deliverPending(stream); continue; }

		int timeout = hasPending ? (int)(stream->latency * 1000.0) : -1;
		struct pollfd fds[2];
		fds[0].fd = stream->fd;      fds[0].events = POLLIN; fds[0].revents = 0;
		fds[1].fd = stream->wakePipe[0]; fds[1].events = POLLIN; fds[1].revents = 0;

		int r = poll(fds, 2, timeout);
		if (r < 0) { if (errno == EINTR) continue; break; }
		if (fds[0].revents & POLLIN) _AGWatchBackendDrain(stream);
		if (fds[1].revents & POLLIN) { char b[64]; while (read(stream->wakePipe[0], b, sizeof b) > 0) { } }

		pthread_mutex_lock(&stream->mutex);
		bool pendingNow = stream->pendingCount > 0;
		bool flushNow = stream->flushRequested;
		pthread_mutex_unlock(&stream->mutex);
		if (flushNow || (pendingNow && (stream->latency <= 0.0 || r == 0))) deliverPending(stream);
	}
	return NULL;
}

FSEventStreamRef FSEventStreamCreate(CFAllocatorRef allocator, FSEventStreamCallback callback,
                                     FSEventStreamContext *context, CFArrayRef pathsToWatch,
                                     FSEventStreamEventId sinceWhen, CFTimeInterval latency,
                                     FSEventStreamCreateFlags flags)
{
	(void)allocator;
	AGEventStream *stream = calloc(1, sizeof(*stream));
	if (stream == NULL) return NULL;
	stream->paths = (pathsToWatch != NULL) ? (CFArrayRef)CFRetain(pathsToWatch)
	                                       : CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	stream->callback = callback;
	if (context != NULL) stream->context = *context;
	stream->sinceWhen = sinceWhen;
	stream->latency = (latency < 0.0) ? 0.0 : latency;
	stream->createFlags = flags;
	stream->fd = -1;
	stream->wakePipe[0] = stream->wakePipe[1] = -1;
	pthread_mutex_init(&stream->mutex, NULL);
	pthread_cond_init(&stream->cond, NULL);
	return stream;
}

Boolean FSEventStreamStart(FSEventStreamRef streamRef)
{
	AGEventStream *stream = streamRef;
	if (stream == NULL) return false;
	if (stream->running) return true;

	stream->fd = _AGWatchBackendOpen(stream);
	if (stream->fd < 0) return false;
	if (pipe(stream->wakePipe) != 0) { _AGWatchBackendClose(stream); stream->fd = -1; return false; }
	/* NON-BLOCKING, BOTH ENDS, AND IT IS NOT OPTIONAL: the reader drains the pipe with a `read` loop, and a
	 * BLOCKING pipe would hang on the read after the last byte — measured, as a test that passed every check
	 * and then hung in `FSEventStreamFlushSync`. */
	fcntl(stream->wakePipe[0], F_SETFL, fcntl(stream->wakePipe[0], F_GETFL, 0) | O_NONBLOCK);
	fcntl(stream->wakePipe[1], F_SETFL, fcntl(stream->wakePipe[1], F_GETFL, 0) | O_NONBLOCK);

	stream->stopRequested = false;
	stream->flushRequested = false;
	if (pthread_create(&stream->thread, NULL, readerMain, stream) != 0) {
		close(stream->wakePipe[0]); close(stream->wakePipe[1]);
		stream->wakePipe[0] = stream->wakePipe[1] = -1;
		_AGWatchBackendClose(stream); stream->fd = -1;
		return false;
	}
	stream->threadValid = true;
	stream->running = true;
	return true;
}

void FSEventStreamStop(FSEventStreamRef streamRef)
{
	AGEventStream *stream = streamRef;
	if (stream == NULL || !stream->running) return;

	pthread_mutex_lock(&stream->mutex);
	stream->stopRequested = true;
	pthread_mutex_unlock(&stream->mutex);
	if (stream->wakePipe[1] >= 0) { char b = 0; ssize_t w = write(stream->wakePipe[1], &b, 1); (void)w; }

	if (stream->threadValid) pthread_join(stream->thread, NULL);
	stream->threadValid = false;
	stream->running = false;

	if (stream->wakePipe[0] >= 0) { close(stream->wakePipe[0]); stream->wakePipe[0] = -1; }
	if (stream->wakePipe[1] >= 0) { close(stream->wakePipe[1]); stream->wakePipe[1] = -1; }
	if (stream->fd >= 0) { _AGWatchBackendClose(stream); stream->fd = -1; }
}

void FSEventStreamInvalidate(FSEventStreamRef streamRef) { FSEventStreamStop(streamRef); }

void FSEventStreamRelease(FSEventStreamRef streamRef)
{
	AGEventStream *stream = streamRef;
	if (stream == NULL) return;
	FSEventStreamStop(stream);
	for (size_t i = 0; i < stream->pendingCount; i++) free(stream->pending[i].path);
	free(stream->pending);
	if (stream->paths != NULL) CFRelease(stream->paths);
	pthread_mutex_destroy(&stream->mutex);
	pthread_cond_destroy(&stream->cond);
	free(stream);
}

void FSEventStreamFlushSync(FSEventStreamRef streamRef)
{
	AGEventStream *stream = streamRef;
	if (stream == NULL || !stream->running) return;

	pthread_mutex_lock(&stream->mutex);
	stream->flushRequested = true;
	pthread_mutex_unlock(&stream->mutex);
	if (stream->wakePipe[1] >= 0) { char b = 0; ssize_t w = write(stream->wakePipe[1], &b, 1); (void)w; }

	pthread_mutex_lock(&stream->mutex);
	while (stream->flushRequested) pthread_cond_wait(&stream->cond, &stream->mutex);
	pthread_mutex_unlock(&stream->mutex);
}

FSEventStreamEventId FSEventStreamGetLatestEventId(FSEventStreamRef streamRef)
{
	AGEventStream *stream = streamRef;
	if (stream == NULL) return 0;
	return stream->nextId;
}

Boolean FSEventStreamSetDispatchQueue(FSEventStreamRef streamRef, void *queue)
{
	/* NOT HONOURED, AND RETURNED AS NOT HONOURED: delivery is on the stream's own reader thread. A caller
	 * that needs it elsewhere marshals it. See EventStream.h. */
	(void)streamRef;
	(void)queue;
	return false;
}
