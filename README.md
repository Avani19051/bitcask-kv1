<div align="center">

# bitcask-kv

### A persistent, crash-recoverable key-value store written from scratch in C++

An append-only storage engine (the **Bitcask** model) with crash recovery, log
compaction, and a **multithreaded TCP server** — no database library, no framework.
The durability, the concurrency, and the on-disk format are all built by hand.

</div>

---

## What it is

`bitcask-kv` is a key-value database engine built from the ground up in C++. It stores
data durably on disk, survives crashes, reclaims space as data churns, and serves many
clients at once over a network socket — the same core responsibilities a real database
has, implemented directly rather than imported.

It follows the **Bitcask** design (the storage engine behind Riak): every write is
appended to a log file, and an in-memory hash index maps each key to the byte offset of
its latest value. Reads are a single index lookup plus one disk seek.

```
   client ──TCP──▶ server ──▶ KVStore
                                 │
              in-memory index:  key ──▶ (file offset, size)
                                 │
              on-disk log:  [..][key3=v][key1=v'][key2=TOMBSTONE][key1=v'']
                              (append-only — newest record for a key wins)
```

## Benchmarks

Measured on a Windows machine (MinGW g++ 15.2, `-O2`), single node, 50,000 keys.

| Operation | Throughput | Latency |
|---|---|---|
| **Write** (flush-to-OS, crash-safe) | ~99,000 ops/sec | ~10 µs/op |
| **Write** (fsync-to-disk, power-loss-safe) | ~1,200 ops/sec | ~840 µs/op |
| **Read** (random key) | ~241,000 ops/sec | ~4 µs/op |
| **Recovery** | 50k keys in ~234 ms | — |

Two things worth reading off this table. **Reads outpace writes** because a read is a
lookup-and-seek while a write also appends and syncs. And the **durability knob is
expensive**: forcing every write all the way to the physical disk (`fsync`) is ~80× slower
than flushing to the OS — the classic durability-vs-throughput tradeoff, here as a
measured number rather than a hand-wave. Recovery time is linear in the number of keys.

*(Run `./bench <N>` to reproduce these at any scale on your own machine.)*

## Built in stages

Each stage is an independently working milestone — the store is usable and correct at the
end of every one.

1. **Append-only log + in-memory index.** Every `SET` appends a length-prefixed record;
   an in-memory map holds `key → (offset, size)`. `GET` is one lookup + one seek. `DEL`
   appends a tombstone. O(1) reads, sequential writes.
2. **Crash recovery.** On startup the engine replays the entire log front-to-back to
   rebuild the index. Because records are replayed in write order, the newest value of
   each key naturally wins. Data survives a clean exit **and** a hard kill identically —
   the on-disk file is the single source of truth.
3. **Compaction.** The append-only log accumulates superseded records and tombstones.
   Compaction rewrites the live data (defined exactly by the current index) into a fresh
   file and **atomically swaps it in** — so a crash mid-compaction leaves the original
   file intact. In testing, a churned 127-byte file compacted to 40 bytes with no data loss.
4. **Multithreaded TCP server.** The engine is exposed over a simple text protocol
   (`SET`/`GET`/`DEL`/`COMPACT`). Each client is handled on its own thread; a mutex
   serializes access to the shared index and file so concurrent clients can't corrupt
   state. Verified with multiple simultaneous clients.

## Design decisions worth calling out

- **Append-only, newest-wins.** Writes never modify data in place; they append. Recovery
  replays in order, so the last record for a key is authoritative with no extra
  bookkeeping. The same property makes compaction trivial: the index already *is* the set
  of live data.
- **Crash-safe compaction via atomic rename.** The new file is written and flushed
  completely before a single `rename` commits it. There is no intermediate state in which
  data is half-written and observable — before the rename the old file is truth, after it
  the new file is.
- **Concurrency via a mutex.** Many clients connect and are served on separate threads; a
  single lock protects the shared index and file position, trading a small amount of
  parallelism for guaranteed correctness. A read/write lock (many readers, one writer)
  is the natural next step.
- **Durability, as a knob.** Two modes: *flush-to-OS* (default) survives a process
  crash and is fast; *fsync-to-disk* (opt-in via `KVStore(path, true)`) survives power
  loss by forcing every write to the physical disk, at ~80× the cost. Both are
  implemented and benchmarked; you pick based on whether you're optimizing for speed or
  for zero data loss.

## On-disk record format

Each record is length-prefixed so the log can be replayed unambiguously:

```
[ key_size : 4 bytes ][ value_size : 4 bytes ][ key bytes ][ value bytes ]
```

`value_size = 0xFFFFFFFF` marks a tombstone (a deletion); no value bytes follow it.
Storing the sizes first is what lets recovery always know exactly how many bytes to read
next.

## Tech

- **Language:** C++17
- **Storage engine:** Bitcask model (append-only log + in-memory hash index)
- **Networking:** raw BSD/Winsock sockets (cross-platform), one thread per client
- **Concurrency:** `std::thread` + `std::mutex`
- **No external dependencies.**

## Build & run

Requires a C++17 compiler (g++/MinGW on Windows, g++/clang on Linux/Mac).

```bash
# 1. Local interactive store (type commands directly)
g++ -std=c++17 -O2 main.cpp kvstore.cpp -o kvstore
./kvstore                 # then: set city Ludhiana | get city | del city | compact | exit

# 2. Network server
#    Windows:  add  -lws2_32   at the end of the command
g++ -std=c++17 -O2 -pthread server.cpp kvstore.cpp -o server
./server                  # listens on port 6380

# 3. Client (in another terminal)
python client.py          # then type: SET city Ludhiana | GET city | quit

# 4. Benchmark
g++ -std=c++17 -O2 bench.cpp kvstore.cpp -o bench
./bench 200000
```

## Project structure

```
bitcask-kv/
├── kvstore.h      # KVStore class: the storage engine interface
├── kvstore.cpp    # set / get / del / recover / compact  (the engine)
├── main.cpp       # local interactive REPL
├── server.cpp     # multithreaded cross-platform TCP server
├── client.py      # simple interactive network client
└── bench.cpp      # throughput + recovery benchmark harness
```

## Honest limitations

- **Whole key set lives in memory.** Like all Bitcask stores, the index holds every key
  in RAM (values stay on disk). Great for many-values / bounded-keys workloads; not for
  datasets whose *keys* exceed memory.
- **Single node, single file.** No replication, no sharding, one active data file.
- **Compaction is manual.** Triggered by a command rather than automatically in the
  background.

## Future work

- **Stage 5 — LSM-tree engine.** A memtable that flushes to sorted immutable SSTables,
  merged on read and during compaction — the design used by LevelDB / RocksDB / Cassandra,
  and the natural path to datasets larger than memory.
- Automatic background compaction triggered by a dead-record ratio.
- A read/write lock for higher read concurrency (many readers, single writer).

---

<div align="center">
Built from scratch: append-only storage → crash recovery → compaction → concurrent network server.
</div>
