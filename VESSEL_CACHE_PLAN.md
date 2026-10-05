# Plan: remember vessels on the microSD card

When a vessel is heard again after it has dropped out of the table, show its name, call sign, type and size straight away, from what was heard last time. Today a returning Class A vessel shows only its MMSI for up to 6 minutes, until its next type 5 message; a Class B one waits for its next type 24.

The cache only fills in vessels that are being heard right now. It never puts a vessel on the map, and never adds one to the vessel count.

## What's cached

| Cached (rarely changes) | Not cached (changes all the time, or per voyage) |
|---|---|
| Class (A/B) | Position, SOG, COG, heading |
| Name | Navigation status |
| Call sign | Destination |
| Ship type | Message counts, which source heard it |
| Dimensions A/B/C/D | |
| First seen, last seen, last changed (wall clock) | |

IMO number, draught and ETA aren't decoded, and aren't cached either.

## How it fits the current firmware

Today: parser → `vessel_db_update()` → vessel table (PSRAM, up to 500 vessels, dropped after 10 minutes unheard).

New: a `vessel_cache` module holds every vessel ever heard in PSRAM, indexed by MMSI, with a log file on the card behind it.

```
parser ─▶ vessel_db_update() ─┬─ new vessel in the table? ─▶ fill static fields from vessel_cache
                              └─ static fields changed?    ─▶ update vessel_cache, mark dirty
                                                                         │
                                         cache writer task (every 60 s) ─┴─▶ append to /sdcard/vessels/cache.log
```

There's no card I/O in `vessel_db_update()`. A lookup is a hash probe in RAM, and writes are batched by a low-priority task. Batching matters here because the card shares its SDMMC host with the Wi-Fi chip.

## Keeping it up to date

- **A live value always wins.** When a received message carries a static field that differs from the cache, the cache takes the new value and is written out on the next flush. This covers renamed vessels, a corrected ship type or size, or an MMSI that has moved to a new boat.
- **Merge field by field, not record by record.** Class B sends its name (24A) and its type, call sign and size (24B) in separate messages, so one part can change without the other.
- **A missing value never overwrites a known one.** The parser already leaves a field out when its string is blank, or when its type or dimensions are 0, so nothing has to change there.
- **Cached values look like live ones.** Nothing on the display or the web page marks a value as coming from the cache.
- **Limit the writes.** At most one write per MMSI every 10 minutes. This stops a fault from writing constantly: two transponders sharing an MMSI, or a misconfigured one that alternates names. The most recent value in RAM is still the one that's used.
- **Skip invalid MMSIs.** 0, and anything that isn't 9 digits, isn't cached. The usual defaults from unconfigured transponders, such as 123456789 and 111111111, are skipped too.
- **Last seen** is updated in RAM on every message, but only written when the stored value is more than a day old, so a vessel sitting at anchor doesn't cause a write every minute.

## File format

One append-only log, `/sdcard/vessels/cache.log`, of fixed 64-byte records:

| Bytes | Field |
|---|---|
| 4 | magic + format version |
| 4 | MMSI |
| 1 | class |
| 1 | ship type |
| 2 | which fields are known (bit mask) |
| 6 | dimensions A (2), B (2), C (1), D (1) |
| 20 | name |
| 7 | call sign |
| 12 | first seen, last seen, last changed (uint32 Unix time each; 0 if the clock wasn't set) |
| 3 | reserved |
| 4 | CRC-32 of the record |

- **At startup** the whole log is read into PSRAM. A later record for an MMSI replaces an earlier one. A record with a bad CRC (for example, half-written when the power was cut) is skipped.
- **Compaction:** when the log holds more than twice as many records as there are vessels, it's rewritten with one record per vessel. The rewrite goes to `cache.tmp`, is synced, the old log is deleted, then the new one renamed. FAT can't rename over an existing file, so at startup a `cache.tmp` with no `cache.log` beside it is taken as the log.
- **Size:** capped at 20,000 vessels (1.3 MB in PSRAM and on the card). When it's full, a new vessel replaces the one heard least recently. A busy port sees a few hundred different vessels a day, so this is years of history.
- **Losing data:** a power cut loses at most the last 60 s of changes. The web Restart button and the firmware update restart flush the cache first.
- **No card:** the cache runs in RAM only, and the status page says so.

## Steps

| # | Step | Done when |
|---|---|---|
| 1 | **Cache module** (`components/vessel_cache`): portable C with stdio file I/O and no ESP-IDF, like the AIS parser. It has the hash index, the merge and change rules, the log reading and writing, and compaction. | Host tests in `test/host` pass: save and reload, a torn last record, compaction (including a crash between delete and rename), a changed name being written once, the write limit, eviction at the cap, and invalid MMSIs being skipped. |
| 2 | **Wire into `vessel_db`:** fill new table entries from the cache, report static-field changes to it. Add the writer task: flush every 60 s, compact at startup when needed. Flush before every planned restart. | A vessel that's expired and then heard again with a position report only shows its name immediately. A renamed vessel (test with `tools/ais_sim.py`) shows the new name, and keeps it after a restart. |
| 3 | **Web page:** in `/api/status`, the cache's vessel count, file size, write count and last error. A "Forget all vessels" button. No changes to the display's screens. | The status page shows the cache growing; the button empties it, on the card too. |
| 4 | **Soak:** a few days on the live feed and the SDR together. | The log stays a sensible size (compaction runs), no card errors, and no effect on Wi-Fi throughput or map tile loading. |

## Progress (2026-10-05)

- **Step 1 done.** `components/vessel_cache`, tested by `test/host/test_vessel_cache.c` (run by `run_tests.sh`).
  - **Built as planned**, with the plan's file format, limits and rules.
  - **Locking:** the module has its own locks, so the writer task's card I/O never holds up `vessel_db_update()`. Lookups and updates only take a short in-RAM lock.
  - **A torn or corrupt record** makes the next flush rewrite the log before appending, so new records never land after damaged bytes.
  - **Checking the tests:** each rule was broken on purpose in turn (index deletion, CRC check, torn tail, temporary-file recovery, write limit, the daily "last seen" write, field-by-field merging), and the tests caught every one.
- **Step 2 built, not yet tested on the display.**
  - **Filling in:** a vessel new to the table gets its static fields from the cache. Values it has sent itself always take priority.
  - **Updating:** every message is passed to the cache after the table is updated, outside the table's lock.
  - **Writer task** (`vessel_cache`, low priority): it loads the cache at startup, off the startup path, then fills in any vessels heard while it loaded. After that it writes changes every 60 s.
  - **No card at startup:** the cache runs in RAM only.
  - **Saving first:** the web Restart button, the display firmware update and the Wi-Fi chip update all save the cache before restarting. The Wi-Fi chip update saves before it shuts down the link to the chip, because the card shares that SDMMC host.
  - **Still to do:** the step's on-device checks: a vessel heard again shows its name straight away, and a renamed one keeps its new name after a restart.
- **Step 3 built, not yet tested on the display.**
  - **`/api/status`:** a `vessel_cache` object with `state` (`loading`, `ok`, `no card` or `error`), `vessels`, `capacity`, `file_bytes`, `writes`, `compactions` and `error`.
  - **Status table:** a "Remembered vessels" row.
  - **Forget all vessels:** a button under Restart, with a confirmation, calls `POST /api/vessels/forget`. Vessels in the table keep what they've sent themselves.
- **Step 4:** not started.

## Decisions

- **Destination:** not cached. It's set per voyage and often left out of date.
- **IMO number:** not decoded or cached.
- **No "from cache" label** anywhere: a cached name looks the same as one just received.
