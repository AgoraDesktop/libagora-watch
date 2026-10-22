/*
 * EventStream_internal.h — the stream object, and the seam between the platform-independent engine and the
 * per-platform backend.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * NOT INSTALLED AND NOT PUBLIC. `EventStream.c` owns the object, the reader thread, the coalescing and the
 * delivery. A BACKEND (`EventStream-inotify.c` on Linux, `EventStream-kqueue.c` on the BSDs) opens the
 * platform's descriptor, translates its readiness into `_AGWatchRecord` calls, and closes it. The engine
 * knows nothing about inotify or kqueue; the backend knows nothing about coalescing or callbacks.
 */
#ifndef WATCH_EVENTSTREAM_INTERNAL_H
#define WATCH_EVENTSTREAM_INTERNAL_H

#include "EventStream.h"

#include <pthread.h>
#include <stdbool.h>

/* The struct tag gets a typedef too, so the engine and the backends name it without `struct`. The public
 * header's `FSEventStreamRef` is a POINTER typedef and does not make `AGEventStream` a type name. */
typedef struct AGEventStream AGEventStream;

typedef struct {
	char *path;
	FSEventStreamEventFlags flags;
} AGWatchPending;

struct AGEventStream {
	CFArrayRef                 paths;
	FSEventStreamCallback      callback;
	FSEventStreamContext       context;
	FSEventStreamEventId       sinceWhen;
	CFTimeInterval             latency;
	FSEventStreamCreateFlags   createFlags;

	int                        fd;			/* the backend's descriptor */
	int                        wakePipe[2];		/* interrupts the reader's poll for flush/stop */

	pthread_t                  thread;
	bool                       threadValid;
	bool                       running;
	bool                       stopRequested;
	bool                       flushRequested;

	pthread_mutex_t            mutex;
	pthread_cond_t             cond;

	FSEventStreamEventId       nextId;
	AGWatchPending            *pending;
	size_t                     pendingCount;
	size_t                     pendingCap;

	void                      *backend;		/* the backend's own state */
};

/* Merge one raw event — an item path and the backend's flag word — into the pending batch, applying the
 * `FileEvents` coarsening. Called by a backend from its `drain`. It takes the stream's lock itself. */
void _AGWatchRecord(AGEventStream *stream, const char *path, FSEventStreamEventFlags flags);

/* ---------------------------------------------------------------------------------------------------
 * the backend (one of EventStream-inotify.c / EventStream-kqueue.c)
 * --------------------------------------------------------------------------------------------------- */

/* Open the platform descriptor and add the stream's paths. Returns the descriptor, or -1. */
int _AGWatchBackendOpen(AGEventStream *stream);

/* Read whatever is ready and hand each event to `_AGWatchRecord`. Must not block. */
void _AGWatchBackendDrain(AGEventStream *stream);

/* Close the descriptor and free the backend's state. */
void _AGWatchBackendClose(AGEventStream *stream);

#endif /* WATCH_EVENTSTREAM_INTERNAL_H */
