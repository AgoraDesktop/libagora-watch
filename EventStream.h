/*
 * EventStream.h — the change stream: a path changed, and here is which way.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE MODEL IS APPLE'S FSEvents (WORKSPACE-PLAN.md §2, F3). A STREAM watches a set of paths, COALESCES the
 * changes that happen within a latency window into one event per path, and delivers them to a callback with
 * a flag word and a monotonically increasing event id. A view that redraws on this stream is live.
 *
 * THE BACKEND IS THE PLATFORM'S OWN, AND IT IS NOT SYSTEMD'S (the project's no-systemd rule): `inotify` on
 * Linux and `kqueue` with `EVFILT_VNODE` on FreeBSD, NetBSD and OpenBSD. Apple has a DAEMON behind FSEvents
 * (`fseventsd`) that keeps a persisted per-volume event log, which is what a `sinceWhen` in the past is
 * replayed from. Agora has no such log yet, so THIS STREAM IS LIVE-ONLY: `kFSEventStreamEventIdSinceNow` is
 * the supported `sinceWhen` and a past id is accepted and treated as "from now", stated in the component
 * README rather than left to surprise a caller.
 *
 * THE GRANULAR PER-ITEM FLAGS (ItemCreated, ItemRemoved, ItemRenamed, ItemModified) ARE 10.7's
 * `kFSEventStreamCreateFlagFileEvents`, not 10.6's: at 10.6 a change was reported at the DIRECTORY and the
 * consumer rescanned it (`MustScanSubDirs`). BOTH ARE HERE — without `FileEvents` the stream coarsens every
 * item to its directory with `MustScanSubDirs`, which is the 10.6 shape; with it, the granular item flags.
 * Agora's own views want the granular model, which is the one Apple settled on, so that is what the suite
 * measures.
 */
#ifndef WATCH_EVENTSTREAM_H
#define WATCH_EVENTSTREAM_H

#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AGEventStream *FSEventStreamRef;
typedef const struct AGEventStream *ConstFSEventStreamRef;

typedef uint64_t FSEventStreamEventId;
typedef uint32_t FSEventStreamEventFlags;
typedef uint32_t FSEventStreamCreateFlags;

/* ============================================================================================ *
 * THE FLAGS, WITH APPLE'S OWN VALUES                                                                  *
 * ============================================================================================ */
enum {
	kFSEventStreamEventFlagNone			= 0x00000000,
	kFSEventStreamEventFlagMustScanSubDirs		= 0x00000001,
	kFSEventStreamEventFlagUserDropped		= 0x00000002,
	kFSEventStreamEventFlagKernelDropped		= 0x00000004,
	kFSEventStreamEventFlagEventIdsWrapped		= 0x00000008,
	kFSEventStreamEventFlagHistoryDone		= 0x00000010,
	kFSEventStreamEventFlagRootChanged		= 0x00000020,
	kFSEventStreamEventFlagMount			= 0x00000040,
	kFSEventStreamEventFlagUnmount			= 0x00000080,
	kFSEventStreamEventFlagItemCreated		= 0x00000100,
	kFSEventStreamEventFlagItemRemoved		= 0x00000200,
	kFSEventStreamEventFlagItemInodeMetaMod		= 0x00000400,
	kFSEventStreamEventFlagItemRenamed		= 0x00000800,
	kFSEventStreamEventFlagItemModified		= 0x00001000,
	kFSEventStreamEventFlagItemFinderInfoMod	= 0x00002000,
	kFSEventStreamEventFlagItemChangeOwner		= 0x00004000,
	kFSEventStreamEventFlagItemXattrMod		= 0x00008000,
	kFSEventStreamEventFlagItemIsFile		= 0x00010000,
	kFSEventStreamEventFlagItemIsDir		= 0x00020000,
	kFSEventStreamEventFlagItemIsSymlink		= 0x00040000
};

enum {
	kFSEventStreamCreateFlagNone		= 0x00000000,
	kFSEventStreamCreateFlagUseCFTypes	= 0x00000001,
	kFSEventStreamCreateFlagNoDefer		= 0x00000002,
	kFSEventStreamCreateFlagWatchRoot	= 0x00000004,
	kFSEventStreamCreateFlagIgnoreSelf	= 0x00000008,
	kFSEventStreamCreateFlagFileEvents	= 0x00000010
};

#define kFSEventStreamEventIdSinceNow ((FSEventStreamEventId)0xFFFFFFFFFFFFFFFFULL)

/* ============================================================================================ *
 * THE CONTEXT AND THE CALLBACK                                                                        *
 * ============================================================================================ */
typedef struct {
	CFIndex version;
	void *info;
	const void *(*retain)(const void *info);
	void (*release)(const void *info);
	CFStringRef (*copyDescription)(const void *info);
} FSEventStreamContext;

/* `eventPaths` is a `char **` when the stream was not created with `kFSEventStreamCreateFlagUseCFTypes`,
 * which is the only form this stream produces: an array of NUL-terminated paths. `eventFlags` and
 * `eventIds` are parallel arrays of `numEvents` elements. */
typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef streamRef,
				      void *clientCallBackInfo,
				      size_t numEvents,
				      void *eventPaths,
				      const FSEventStreamEventFlags eventFlags[],
				      const FSEventStreamEventId eventIds[]);

/* ============================================================================================ *
 * THE DOORS                                                                                           *
 * ============================================================================================ */
FSEventStreamRef FSEventStreamCreate(CFAllocatorRef allocator,
				     FSEventStreamCallback callback,
				     FSEventStreamContext *context,
				     CFArrayRef pathsToWatch,
				     FSEventStreamEventId sinceWhen,
				     CFTimeInterval latency,
				     FSEventStreamCreateFlags flags);

Boolean FSEventStreamStart(FSEventStreamRef stream);
void FSEventStreamStop(FSEventStreamRef stream);
void FSEventStreamInvalidate(FSEventStreamRef stream);
void FSEventStreamRelease(FSEventStreamRef stream);

/* Deliver whatever is pending now, and return when the callback has run. The stream also delivers on its own
 * when the latency window closes; this is the door a caller uses to force the issue (a test, or a view that
 * is about to redraw). */
void FSEventStreamFlushSync(FSEventStreamRef stream);

FSEventStreamEventId FSEventStreamGetLatestEventId(FSEventStreamRef stream);

/* Accepted for Apple's signature and HONOURED IN PART: the callback always runs on the stream's own reader
 * thread, because this first version does not schedule to a run loop or a dispatch queue. A caller that
 * needs the callback on a particular thread must marshal it. Stated here rather than silently ignored. */
Boolean FSEventStreamSetDispatchQueue(FSEventStreamRef stream, void *queue);

#endif /* WATCH_EVENTSTREAM_H */
