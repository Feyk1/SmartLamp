# ADR-002 — `mmap` over a file to simulate Flash memory

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The firmware needs non-volatile memory for the bootloader, A/B slots, metadata and configuration.

## Context

The firmware requires:

- **Persistent bootloader:** the bootloader code must survive reboots.
- **Two application slots (A/B):** for OTA with rollback.
- **Metadata:** active slot, pending slot, confirmation flags, active version.
- **Configuration:** last color, brightness, mode, with double buffering and CRC.
- **OTA staging:** temporary space to download the `.ota` before applying it.

On a real MCU, this is solved with **internal flash** (or external flash, or FRAM). In POSIX there is no physical flash, so an abstraction is needed.

Three options were evaluated:

1. **Text / JSON files** (one per slot, one for metadata, one for config).
2. **Embedded database** (SQLite or similar).
3. **Single binary file mapped with `mmap`**.

## Decision

**A single binary file (`flash.bin`, 1 MB) mapped with `mmap()` is chosen**, with a layout identical to that of a real MCU:

| Offset | Size | Region |
| :--- | :--- | :--- |
| 0x00000 | 64 KB | Bootloader |
| 0x10000 | 352 KB | Slot A |
| 0x68000 | 352 KB | Slot B |
| 0xC0000 | 16 KB | Metadata (active slot, pending slot, confirmed, version) |
| 0xC4000 | 16 KB | Configuration (double buffer, CRC) |
| 0xC8000 | 208 KB | `.ota` staging |
| 0xFC000 | 16 KB | Reserved / padding |

> **Sizing:** 352 KB per slot leaves headroom for debug builds, future growth
> and a rollback image. Staging is 208 KB so it can hold the ~149 KB `.ota`
> plus its 208 B header; a smaller partition would not fit.

Access is done through a HAL `flash_read(addr, buf, len)` / `flash_write(addr, buf, len)` / `flash_erase(addr, len)`, which internally operates on the mapping.

## Alternatives considered

### Alternative A: Text / JSON files

**Advantages:**
- Easy to inspect during development (`cat flash.json`).
- Easy to version in Git (although that is not the main use).
- No additional dependencies.

**Drawbacks:**
- **Not faithful to reality:** an MCU does not store JSON in flash.
- **Inefficient:** parsing JSON on every read consumes CPU and memory.
- **Does not allow direct `execve`:** the slot binary must be extracted to an executable file before `execve`, which breaks the illusion of "jumping to slot A".
- **Hard to corrupt realistically:** the Simulation Panel needs to inject corruption into specific bytes; with JSON this is artificial.
- **Does not allow atomic writes with CRC:** it requires serializing the whole file on every write.

**Discarded because:** fidelity to hardware is a goal of the project, and the text format breaks it.

### Alternative B: Embedded database (SQLite)

**Advantages:**
- Atomic transactions (ACID) without manually implementing double buffering.
- Structured queries for metadata and configuration.
- Mature and well documented.

**Drawbacks:**
- **Adds a heavy dependency** (~1 MB) to the firmware, which would not exist on a real MCU.
- **Not faithful:** an MCU does not run SQLite from flash.
- **Does not allow direct `mmap` of the slot binary:** SQLite stores blobs, not mappable executable files.
- **Does not allow realistic corruption:** injecting a corrupt byte into a SQLite database requires special transactions; on real flash it is writing a byte.

**Discarded because:** it solves the atomicity problem, but at the cost of fidelity and portability.

### Alternative C: Single binary file mapped with `mmap` (chosen)

**Advantages:**
- **High fidelity:** accessing "flash" is accessing memory (`*(uint32_t*)addr`), just like on an MCU.
- **Allows direct `execve`:** the slot binary can be executed from the mapping (or copied to `/tmp` with `memfd_create` for `fexecve`).
- **Automatic persistence:** changes in the mapping are synchronized with the file using `msync()`.
- **Realistic corruption:** injecting a corrupt byte is writing to the corresponding offset.
- **No external dependencies:** only POSIX `mmap`.
- **Layout identical to that of an MCU:** the memory map is the same as on real hardware.

**Drawbacks:**
- **Manual atomic writes:** double buffering + CRC must be implemented for configuration and metadata (it is not automatic as in SQLite).
- **Synchronization with `msync()`:** if the process dies without syncing, the last changes may be lost. Mitigation: `msync(MS_SYNC)` after every critical write.
- **Fixed size:** `flash.bin` is created with 1 MB and does not grow dynamically. This is a conscious limitation (an MCU does not have infinite flash either).
- **No wear-leveling:** repeatedly writing the same offset wears out the simulated "flash". In a file it does not matter, but the real behavior is not modeled.

**Chosen because:** the balance between fidelity, portability and implementation simplicity is the best of the three options.

## Consequences

### Positive

- **Fidelity to hardware:** the bootloader and firmware code is identical to what would run on an MCU, except for the HAL.
- **Realistic corruption:** the Simulation Panel can inject faults by writing bytes at specific offsets or by corrupting `driver_state.bin` (which enters safe mode).
- **Direct `execve`:** the bootloader can `execve` the slot binary without extracting it to another file.
- **Persistence across reboots:** `flash.bin` survives process restarts, just like real flash.
- **Documentable layout:** the memory map can be shown in the README and in the Simulation Panel.

### Negative

- **Manual double buffering:** the A/B metadata requires atomic writes (two records with CRC32, alternate on every save). The LED-driver configuration uses its own `driver_state.bin` with a CRC, outside `flash.bin`. It is more code than SQLite, but also more faithful.
- **Explicit `msync()`:** critical writes must be followed by a sync. The `flash_write` HAL performs `msync(MS_SYNC)` by default.
- **Fixed size:** if the firmware grows beyond 352 KB, the A/B slot does not fit. Mitigation: the limit is documented and 1 MB is a generous size for a lamp.
- **Staging is smaller than a slot:** the maximum stageable payload is `208 KB − 208 B = 212,784 B`. An image between ~208 KB and 352 KB would fit a slot but could not be downloaded until `flash.bin` or the slot/staging split changes. Mitigation: documented here and in `docs/04`; enlarge `flash.bin` or rebalance the slots if the firmware grows.
- **No wear-leveling:** a file has no erase cycles, so there is nothing to level. It is documented as a conscious limitation.

### Neutral

- **`flash.bin` is created when the container starts** if it does not exist. This is analogous to a new MCU with empty flash.
- **`flash.bin` is destroyed with the ephemeral Machine.** This is intentional: every visit starts with a "factory" device.

## Implementation notes

- The 1 MB budget is exact: 64 KB bootloader + 2×352 KB slots + 16 KB metadata + 16 KB config + 208 KB staging + 16 KB reserved = 1024 KB.
- The HAL exposes `flash_read`, `flash_write`, `flash_erase`, `flash_sync`.
- `flash_write` performs `msync(MS_SYNC)` after writing, to guarantee persistence.
- The `0xC4000` configuration region is reserved for a future on-flash config. The current LED-driver configuration is stored in `driver_state.bin` on the driver volume, not in `flash.bin`.
- The A/B metadata uses two 44-byte records 64 bytes apart with magic `LMPA`, schema 1 and CRC32 (`firmware/src/bootloader/ab.c`); a power cut leaves the other copy intact.
- The Simulation Panel can write directly to `flash.bin` via `POST /instances/{id}/sim/flash/write` to inject corruption.
- In the ARM build (future), the HAL is implemented over the MCU's real flash without changing the bootloader or firmware code.

## References

- [`01-general-architecture.md`](../01-general-architecture.md) — section 7.1
- [`04-ota-and-image-security.md`](../04-ota-and-image-security.md) — section 7
- [`mmap(2)` — Linux man pages](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [`msync(2)` — Linux man pages](https://man7.org/linux/man-pages/man2/msync.2.html)
- [Zephyr `native_posix` flash simulator](https://docs.zephyrproject.org/latest/boards/native/native_posix/doc/index.html)
