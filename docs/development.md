# Development

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Use `-DBUILD_PLUGIN=OFF` for core-only development. `HFM_NATIVE_ARCH` and `HFM_ENABLE_LTO` default to ON for Release targets; disable native tuning for cross-compilation or binaries intended for other CPUs. CMake checks compiler and linker support. The hyprpm manifest builds locally with both options enabled and caches the result.

Core tests cover serialization, retention boundaries, TOML validation, producer-thread enforcement, queue overflow/coalescing, debounce, storage-confirmed flush barriers, shutdown, and write retries. ThreadSanitizer cannot fully model the atomic fences used by the upstream queue; sanitizer checks complement ordering and sequential-model tests, rather than proving concurrency correctness.

The optional integration test needs a running Wayland session and kitty:

```sh
cmake -S . -B build -DBUILD_INTEGRATION_DRIVER=ON
cmake --build build -j2
python3 tests/integration.py build
```

It opens isolated nested compositor windows. Never load `hfm-test-driver.so` into the regular desktop.

Enable `-DBUILD_BENCHMARKS=ON` and run `build/memory_benchmark` to measure submission overhead. This does not measure compositor rendering performance.

Format C++ with the repository configuration:

```sh
clang-format -i src/*.cpp src/*.hpp tests/*.cpp bench/*.cpp
```

## Ownership and transport

The compositor thread owns window tracking, Lua policies, the restoration index, and the `Writer` producer interface. Construction, `submit`, `pump`, `needsPump`, `flushAsync`, `flush`, `stats`, and destruction must run on that thread. Producer methods reject other threads; error queries are synchronized separately. A `std::jthread` owns the storage map, serialization, and runtime file writes. No Hyprland or Lua objects cross threads.

ReaderWriterQueue is preallocated for at least 256 messages; upstream may round capacity up. Enqueue uses `try_enqueue`, so the queue never expands at runtime. Messages are trivially copyable and hold an immutable key pointer, dimensions, optional position, timestamp, or flush ticket.

Keys live in stable producer-owned map nodes. The consumer reads individual immutable keys, not the map. Never mutate or reclaim keys before the worker joins. Registering a key can allocate; subsequent updates reuse its slot. Registration memory grows with distinct keys seen during the writer's lifetime.

When the queue is full, each key's slot retains its newest update. An intrusive FIFO links pending slots without allocating extra nodes. A newer update cannot bypass an older pending update for the same key; it replaces that pending value instead. `pump()` retries this list before admitting a pending flush barrier. The plugin schedules retries every 5 ms only while `needsPump()` is true; normal updates also pump pending work.

The queue algorithm is lock-free, but the whole plugin is not allocation-free or lock-free. Registration may allocate, semaphore wakeups may enter the kernel, and errors and file operations have separate synchronization. `submit`, `pump`, and `flushAsync` do not wait for disk or spin waiting for the consumer.

## Wakeup, flush, and shutdown

A binary semaphore and an atomic outstanding-wakeup flag coalesce notifications. Clear the flag only after consuming a token, before draining messages. A producer racing that transition either contributes to the current drain or releases another token. Timed semaphore waits implement the 300 ms quiet interval without idle polling.

The worker merges updates by identity. Identical records do not dirty clean state. It writes dirty state after the quiet interval or on a flush barrier. Failed writes preserve dirty state for a later update or explicit flush to retry; there is no endless automatic retry. `std::expected` carries failures, and an atomic error flag avoids locking the error mutex on healthy callbacks.

Flush requests receive increasing tickets; pending requests may share a later barrier. Admit a barrier only after all pending updates. A newer value may supersede an earlier value for the same key. Acknowledge the ticket only after the storage attempt completes, even on failure. Dequeue progress and completed storage tickets are separate atomics: an empty queue does not establish durability.

`flushAsync()` returns immediately. `flush()` pumps pending work, waits for dequeue progress if needed, then waits for its storage ticket. Synchronous flush and shutdown can block, including on a stalled filesystem.

Disconnect plugin callbacks and cancel retry timers before the final flush. Writer destruction drains pending work, requests stop, wakes and joins the worker, and only then releases the queue and key storage.

## Storage and retention

CBOR is encoded as:

```text
[2, [[key, role, width, height, position, last_used_unix_seconds], ...]]
```

`position` is `null` or `[x, y]`, with signed 32-bit integer offsets in logical pixels relative to the work area. Only this schema is accepted; older schemas are rejected. Keys and roles contain 1–4096 bytes; dimensions are integers from 1 to 32768; timestamps are positive integers. The reader limits input to 10000 records and 4 MiB. Key pointers in queue messages are process-local and never serialized.

Writes use a mode-0600 temporary file, file fsync, atomic rename, and directory fsync. A storage attempt rewrites the complete map.

Retention is synchronous initialization work: validate TOML, read state, prune, and start the worker only after success. Expire records strictly older than `max_age_days × 86400` seconds. Zero disables expiry; future timestamps survive backwards clock changes. Rewrite only if records were removed, including when the result is empty. Cleanup failure aborts initialization.

Last-use timestamps use wall-clock Unix time; debounce uses a monotonic clock. See the [README](../README.md) for configuration, file locations, and user-visible behavior.

## Geometry recording

Move and resize events use Hyprland's drag controller; tiled drags, fullscreen, and programmatic changes are excluded. Check host coordinates for finiteness and integer range before rounding or narrowing. Compare geometry before updating its timestamp. Moving with position memory disabled submits no update; resizing still preserves size memory.

Restoration chooses the per-window record before shared persistent state. A saved position overrides centering only when `remember_position` is enabled. Fit relative offsets to the current work area after fitting size, without changing stored preferences. Never persist a synthetic first position or a temporarily clamped position from restoration.
