# Provenance

**First-party: written for Agora, not extracted from anywhere.** No upstream revision to pin and no
third-party code to licence.

| what | where |
|---|---|
| the guide | Apple's FSEvents model — a stream over a set of paths, coalesced by a latency window, delivered to a callback with flags and event ids |
| the Linux backend | **in-tree**, over the kernel's `inotify` (`EventStream-inotify.c`) — no library |
| the BSD backend | **in-tree**, over the kernel's `kqueue`/`EVFILT_VNODE` (`EventStream-kqueue.c`) — no library |
| third-party code | **none** |
| systemd | **none**, and by construction: `inotify` and `kqueue` are kernel interfaces, not services |
| licence | MIT, per file (`LICENSE`) |

## What was decided, and what was measured

**Two backends, one engine.** `EventStream.c` knows nothing about either platform: a backend opens a
descriptor and calls `_AGWatchRecord`, and the engine coalesces and delivers. That is what lets the Linux
backend be measured here while the BSD one is written and waiting for a BSD run.

**The kqueue backend is written and not yet measured.** This tree is developed on Linux. Saying so is the
point: `pkg`-style confidence about an unrun backend is exactly what this project records rather than
implies. On a BSD, `NOTE_WRITE` on a directory is coarser than inotify (it does not name the entry), so a
directory change is reported with `MustScanSubDirs` — the 10.6 FSEvents shape — and a watched file gets the
granular flags.

**The granular flags are 10.7's, and the 10.6 shape is kept as the default.** `ItemCreated`, `ItemRemoved`,
`ItemRenamed` and `ItemModified` are `kFSEventStreamCreateFlagFileEvents`, which is 10.7 (Lion). At 10.6 a
change was reported at the directory with `MustScanSubDirs`. Both behaviours are implemented; the flag picks.
The plan's F3 test asks for create/remove/rename, which is the granular model, so that is what the suite
creates the stream with.

**A bug this component met and fixed, recorded because it is the kind that passes tests and hangs.** The
reader wakes from `poll` through a self-pipe, and drains it with `while (read(...) > 0)`. With a *blocking*
pipe that loop hangs on the read after the last byte — measured, as a suite that printed **every check as
`ok`** and then hung in `FSEventStreamFlushSync`. Both ends are now `O_NONBLOCK`, and the reason is written at
the `pipe` call so it is not undone.

## Licence per file

Every file is MIT. There is no third-party code in this component, so there is no per-file licence split to
record.
