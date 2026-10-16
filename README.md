# libagora-watch

**The change stream for Agora** — a path changed, and here is which way. It is what keeps a file manager's
views live.

Agora is an installable desktop environment for Linux and the BSDs; see the project README in the parent
directory (`../README.md`). `libagora-watch` is Agora's FSEvents: it sits on the platform's own kernel
interface and on Core Foundation, and it depends on nothing else.

## Provenance

Written for Agora, against Apple's FSEvents model — not extracted from anywhere. See
[`PROVENANCE.md`](PROVENANCE.md).

## Status

**Built, C throughout over Core Foundation alone, and measured on Linux.**

```
$ readelf -d build/libagora-watch.so | grep NEEDED
  Shared library: [libagora-core.so]
  Shared library: [libc.so.6]
```

**No library is linked for the watching and no systemd component is involved**: the backend is the kernel's
own — `inotify` on Linux, `kqueue` with `EVFILT_VNODE` on FreeBSD, NetBSD and OpenBSD — chosen by the build.

### The backends, and which is measured

- **`inotify` (Linux) — BUILT AND MEASURED HERE.** A watch is added for every directory under each watched
  path, and a directory that appears inside the tree is watched the moment it is seen, so a file created in a
  **new** subdirectory is a live event and not a miss.
- **`kqueue` (FreeBSD / NetBSD / OpenBSD) — WRITTEN, NOT YET RUN.** This tree is developed on Linux; a BSD
  run is what turns "written" into "measured", and the file says so at its top. On a BSD, a directory's
  `NOTE_WRITE` says its entries changed but not which, so a directory change is reported with
  `MustScanSubDirs` — the 10.6 model exactly — while a watched file gets the granular flags.

### What is measured, not assumed

`ctest` runs one suite, `tests/eventstream.c`, which **mutates a directory from a shell** (another process,
which is what a file manager actually watches for). **12 checks, 0 fail**:

- creating a file is `ItemCreated`; writing it is `ItemModified`; renaming it is `ItemRenamed` at the
  destination; removing it is `ItemRemoved`;
- **forty writes to one file are COALESCED** — measured at **one** event, not forty;
- a file in a **new subdirectory** is reported, so the tree stays live;
- `FSEventStreamSetDispatchQueue` **returns false**, because delivery is on the reader thread and the door
  says so rather than pretending.

## The doors

`EventStream.h` — Apple's FSEvents shape: `FSEventStreamCreate`, `FSEventStreamStart`, `FSEventStreamStop`,
`FSEventStreamInvalidate`, `FSEventStreamRelease`, `FSEventStreamFlushSync`, `FSEventStreamGetLatestEventId`,
`FSEventStreamSetDispatchQueue`, the `FSEventStreamCallback`, the context, and the flag vocabulary.

### What is deliberately not here

- **No event history.** Apple's FSEvents is backed by a daemon (`fseventsd`) that keeps a persisted
  per-volume log, which is what a `sinceWhen` in the past is replayed from. Agora has no such log, so this
  stream is **live-only**: `kFSEventStreamEventIdSinceNow` is the supported value and a past id is treated as
  "from now". History replay is a later concern and a persisted log of its own.
- **No run-loop or dispatch-queue scheduling.** The callback runs on the stream's reader thread;
  `FSEventStreamSetDispatchQueue` is accepted and returns false.
- **The granular per-item flags are 10.7's `kFSEventStreamCreateFlagFileEvents`, not 10.6's.** Without that
  flag the stream **coarsens** every item change to its directory with `MustScanSubDirs`, which is the 10.6
  shape; with it, the granular model Apple settled on. Both are here; the suite measures the granular one.

## Build and run

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
      -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
cmake --build build
cd build && ctest --output-on-failure
```

## Licence

MIT — see [`LICENSE`](LICENSE).
