# 03 — Modbus register map

## 1. Design principles

| Decision | Choice | Reason |
| :--- | :--- | :--- |
| **Color unit** | HSV (H: 0-360, S/V: 0-1000 in 0.1%) | More natural for lamp control; H fits in 16 bits. RGB would require 3 registers and make smooth transitions harder. |
| **Target vs Actual** | Separate (holding vs input) | The firmware writes the target; the driver writes the actual while it animates. They never overwrite each other. |
| **Commands** | Coils (bits) instead of magic registers | Modbus standard; the firmware triggers actions on a rising edge. |
| **Sequences** | Fixed IDs 1-15 + 100+ uploaded via buffer | Covers the simple case and demonstrates structured data transfer. |
| **Explicit heartbeat** | Counter register + configurable timeout | Safety: if the master dies, the driver enters a safe state. |
| **Diagnostics** | Input registers (read-only) | The driver is the only one that can report temperature, current, uptime. |

## 2. Slave parameters

| Parameter | Value |
| :--- | :--- |
| Protocol | Modbus TCP |
| Port | 502 (local demo: 5020) |
| Unit ID | 1 |
| Byte order (16-bit) | Big-endian (Modbus standard) |
| Byte order (32-bit) | **CDAB** (low word first, big-endian within each word) |
| Max registers per FC16 | 123 (Modbus limit) |
| Request timeout | 1000 ms |
| Retries | 2 (with 200 ms between attempts) |

> **Note on CDAB:** this is a classic source of interoperability bugs. The register map uses CDAB consistently; any new client must follow it.

## 3. Area map

| Area | Range | Type | Access from firmware | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **Coils** | 00001-00016 | R/W bit | Write (commands) | Trigger actions |
| **Discrete Inputs** | 10001-10016 | R bit | Read (status) | Status flags |
| **Holding Registers** | 40001-40050 | R/W 16-bit | Write (target/config) | Configure and command |
| **Input Registers** | 30001-30040 | R 16-bit | Read (status/diag) | Actual state and diagnostics |

## 4. Coils — Control (00001+)

Written by the firmware. A **rising edge (0→1)** triggers the action.

| Addr | Name | Description |
| :--- | :--- | :--- |
| 00001 | `CMD_APPLY_TARGET` | Applies the current target (H, S, V, CCT, brightness) |
| 00002 | `CMD_POWER_ON` | Turns on with the current target |
| 00003 | `CMD_POWER_OFF` | Turns off (with transition if `TARGET_TRANSITION_MS` > 0) |
| 00004 | `CMD_START_SEQUENCE` | Starts the sequence defined in `SEQ_ID` |
| 00005 | `CMD_STOP_SEQUENCE` | Stops the sequence and keeps the current color |
| 00006 | `CMD_SAVE_CONFIG` | Saves the state to `driver_state.bin` (also autosaved every 2 s) |
| 00007 | `CMD_LOAD_CONFIG` | Reloads the state from `driver_state.bin` |
| 00008 | `CMD_FACTORY_RESET` | Restores factory defaults |
| 00009 | `CMD_CLEAR_FAULTS` | Clears active alarms and counters |
| 00010 | `CMD_SELFTEST` | Runs a self-test (result goes to input registers) |
| 00011 | `ENABLE_HEARTBEAT_WATCHDOG` | 1=enabled, 0=disabled |
| 00012-00016 | Reserved | |

**Edge semantics:** the driver reads the coil, and if it detects a 0→1 transition it executes the action and **auto-resets** the coil to 0. This prevents accidental re-triggering by consecutive polls.

## 5. Discrete Inputs — Status (10001+)

Read by the firmware. The driver updates them.

| Addr | Name | Description |
| :--- | :--- | :--- |
| 10001 | `STATUS_POWER_ON` | Actual power state |
| 10002 | `STATUS_SEQUENCE_RUNNING` | Sequence active |
| 10003 | `STATUS_CONFIG_DIRTY` | Unsaved changes in flash |
| 10004 | `STATUS_FAULT_ACTIVE` | Any active fault |
| 10005 | `STATUS_OVERTEMP_WARNING` | Temperature > warning threshold |
| 10006 | `STATUS_OVERTEMP_CRITICAL` | Temperature > critical threshold |
| 10007 | `STATUS_OVERCURRENT` | Current > configured maximum |
| 10008 | `STATUS_LED_OPEN` | LED disconnected / open circuit |
| 10009 | `STATUS_HEARTBEAT_LOST` | Heartbeat watchdog expired |
| 10010 | `STATUS_SELFTEST_RUNNING` | Self-test in progress |
| 10011 | `STATUS_SEQUENCE_INVALID` | Custom sequence corrupt or incomplete |
| 10012 | `STATUS_FAULT_LATCHED` | Critical fault latched; requires `CMD_CLEAR_FAULTS` |
| 10013 | `STATUS_CONFIG_SAFE_MODE` | Corrupt configuration; safe defaults and save/load blocked |
| 10014-10016 | Reserved | |

## 6. Holding Registers — Target, configuration, commands (40001+)

### 6.1 Target block (40001-40009)

| Addr | Name | Range | Unit | Description |
| :--- | :--- | :--- | :--- | :--- |
| 40001 | `TARGET_POWER` | 0-1 | — | 0=off, 1=on |
| 40002 | `TARGET_MODE` | 0-3 | — | 0=manual HSV, 1=CCT, 2=sequence, 3=scene |
| 40003 | `TARGET_HUE` | 0-360 | degrees | Hue |
| 40004 | `TARGET_SAT` | 0-1000 | 0.1% | Saturation |
| 40005 | `TARGET_VAL` | 0-1000 | 0.1% | Value (color intensity) |
| 40006 | `TARGET_CCT` | 2000-6500 | K | Color temperature (CCT mode) |
| 40007 | `TARGET_BRIGHTNESS` | 0-1000 | 0.1% | Global brightness (post-color) |
| 40008 | `TARGET_TRANSITION_MS` | 0-10000 | ms | Duration of the transition when applying |
| 40009 | `TARGET_SCENE_ID` | 1-16 | — | Predefined scene ID |

### 6.2 Sequence block (40011-40019)

| Addr | Name | Range | Description |
| :--- | :--- | :--- | :--- |
| 40011 | `SEQ_ID` | 1-15, 100-255 | 1-15 predefined; 100+ custom |
| 40012 | `SEQ_SPEED` | 0-10000 | 1000 = 1.0×; 500 = 0.5×; 2000 = 2× |
| 40013 | `SEQ_DIRECTION` | 0-2 | 0=forward, 1=reverse, 2=ping-pong |
| 40014 | `SEQ_LOOP_COUNT` | 0-65535 | 0 = infinite |
| 40015 | `SEQ_STEP_INDEX` | 0-255 | Step index (for upload) |
| 40016 | `SEQ_TOTAL_STEPS` | 0-255 | Total steps in the custom sequence |
| 40017 | `SEQ_STEP_HUE` | 0-360 | Current step: hue |
| 40018 | `SEQ_STEP_SAT` | 0-1000 | Current step: sat |
| 40019 | `SEQ_STEP_VAL` | 0-1000 | Current step: val |

### 6.3 Custom sequence upload buffer (40020-40025)

| Addr | Name | Description |
| :--- | :--- | :--- |
| 40020 | `SEQ_STEP_RESERVED` | Reserved (0) |
| 40021 | `SEQ_STEP_BRIGHTNESS` | Step brightness (0-1000) |
| 40022 | `SEQ_STEP_DURATION` | Duration in 10 ms units (max 655 s) |
| 40023 | `SEQ_STEP_TRANSITION` | Transition in 10 ms units |
| 40024 | `SEQ_STEP_CCT` | Step CCT (2000-6500 K) |
| 40025 | `SEQ_CRC` | CRC-16/CCITT-FALSE of the steps |

> **Note:** a complete step occupies 8 registers (40017-40024), with 40020
> reserved to align with the driver struct. It is written with FC16 in a single
> transaction. The driver supports up to **16 steps**; the UI limits it to 8.

### 6.4 Configuration block (40031-40040)

| Addr | Name | Range | Description |
| :--- | :--- | :--- | :--- |
| 40031 | `CFG_MAX_BRIGHTNESS` | 0-1000 | Upper brightness limit |
| 40032 | `CFG_MIN_BRIGHTNESS` | 0-1000 | Lower limit (prevents accidental blackout) |
| 40033 | `CFG_POWER_ON_BEHAVIOR` | 0-2 | 0=off, 1=on, 2=last state |
| 40034 | `CFG_GAMMA` | 0-2 | 0=linear, 1=sRGB, 2=gamma 2.2 |
| 40035 | `CFG_HEARTBEAT_TIMEOUT_S` | 0-65535 | 0=disabled; otherwise seconds |
| 40036 | `CFG_OVERTEMP_WARN_C` | -400 to 1250 | 0.1 °C |
| 40037 | `CFG_OVERTEMP_CRIT_C` | -400 to 1250 | 0.1 °C |
| 40038 | `CFG_MAX_CURRENT_MA` | 0-65535 | mA |
| 40039-40040 | Reserved | | |
| 40041 | `HEARTBEAT_COUNTER` | 0-65535 | See §6.5 |
| 40042-40045 | Reserved | | |
| 40046 | `SIM_FAULT_CODE` | 0-7 | Forced fault (Sim Panel; 0=none) |
| 40047 | `SIM_FAULT_DURATION_S` | 0-3600 | 0 = until cleared |
| 40048 | `SIM_FAULT_MAGIC` | 0x5A5A | Arms the forced fault; any other value disarms it |
| 40049-40050 | Reserved | | |

> The driver stores and range-checks `CFG_POWER_ON_BEHAVIOR` and `CFG_GAMMA`,
> but does not apply them. The power-on color is a backend policy
> (`power_on` service); gamma correction is not implemented. Scenes are also a
> dashboard concept, not a driver feature.

### 6.5 Heartbeat (40041)

| Addr | Name | Description |
| :--- | :--- | :--- |
| 40041 | `HEARTBEAT_COUNTER` | The firmware writes an incrementing counter every 1 s. If it does not change within `CFG_HEARTBEAT_TIMEOUT_S`, the driver enters a safe state (turns the LED off). |

### 6.6 Out-of-range writes

Modbus is the trust boundary: the driver does not assume the writer is the
dashboard or the firmware. Holding writes are sanitized before they are stored,
and a normal Modbus success response is returned. A subsequent read shows the
accepted value, not the rejected one.

- **Numeric ranges:** hue 0-360, saturation/value/brightness 0-1000, CCT
  2000-6500 K and transition 0-10000 ms are clamped.
- **Enum-like registers:** `TARGET_MODE` accepts 0-3; `CFG_POWER_ON_BEHAVIOR`
  and `CFG_GAMMA` accept 0-2; `SIM_FAULT_CODE` accepts 0-7. Any other value
  falls back to `0` (the safe/default option).
- **`CFG_MIN/MAX_BRIGHTNESS`:** both are clamped to 0-1000. The driver uses the
  smaller value as the lower bound and the larger as the upper bound, so an
  inverted pair cannot block the output. `ACTUAL_BRIGHTNESS` always stays inside
  that interval; the over-temperature derate may still go below the minimum.
- **`CFG_OVERTEMP_WARN/CRIT_C`:** stored as signed 16-bit values in the
  -400 to 1250 range. If the warning threshold is above the critical one, the
  driver sorts them (lower = warning, higher = critical).
- **Sequence steps:** hue, saturation, value, brightness and CCT are sanitized
  when they are captured. The total-step count and CRC are still validated
  before a custom sequence starts.

The same rules apply when the state file is loaded, so a value written by an
older version cannot bypass the limits.

## 7. Input Registers — Actual state and diagnostics (30001+)

### 7.1 Actual state block (30001-30009)

The firmware polls them at **20 Hz** in a single transaction (`FC04`, 9 registers).

| Addr | Name | Unit | Description |
| :--- | :--- | :--- | :--- |
| 30001 | `ACTUAL_POWER` | — | 0/1 |
| 30002 | `ACTUAL_MODE` | — | Mode in progress |
| 30003 | `ACTUAL_HUE` | degrees | Actual hue (interpolated) |
| 30004 | `ACTUAL_SAT` | 0.1% | Actual saturation |
| 30005 | `ACTUAL_VAL` | 0.1% | Actual value |
| 30006 | `ACTUAL_CCT` | K | Actual CCT |
| 30007 | `ACTUAL_BRIGHTNESS` | 0.1% | Actual brightness, always inside the configured limits |
| 30008 | `ACTUAL_SEQ_STEP` | — | Current sequence step |
| 30009 | `ACTUAL_TRANSITION_PROGRESS` | 0-1000 | 0=start, 1000=end of transition |

### 7.2 Diagnostic block (30011-30043)

| Addr | Name | Unit | Description |
| :--- | :--- | :--- | :--- |
| 30011 | `DIAG_TEMP_C_X10` | 0.1 °C (int16) | LED temperature |
| 30012 | `DIAG_CURRENT_MA` | mA | Measured current |
| 30013 | `DIAG_VOLTAGE_MV` | mV | LED rail voltage |
| 30014 | `DIAG_POWER_MW` | mW | Calculated power |
| 30015 | `DIAG_FAULT_CODE` | enum | 0=none, 1=overtemp_warn, 2=overtemp_crit, 3=overcurrent, 4=led_open, 5=config_corrupt, 6=seq_invalid, 7=heartbeat_lost |
| 30016 | `DIAG_FAULT_COUNT` | — | Faults since boot |
| 30017-30018 | `DIAG_UPTIME_S` | s (32-bit, CDAB) | Time since boot |
| 30019-30020 | `DIAG_ON_TIME_S` | s (32-bit) | Accumulated on time |
| 30021 | `DIAG_FW_MAJOR` | — | Firmware version |
| 30022 | `DIAG_FW_MINOR` | — | |
| 30023 | `DIAG_FW_PATCH` | — | |
| 30024 | `DIAG_HW_REV` | — | Driver hardware revision |
| 30025-30026 | `DIAG_SERIAL` | 32-bit | Product serial (public, not a credential) |
| 30027 | `DIAG_MIN_FREE_HEAP` | bytes | Historical minimum free heap |
| 30028 | `DIAG_LAST_FAULT_TS_HI` | s (32-bit) | Timestamp of the last fault |
| 30029 | `DIAG_LAST_FAULT_TS_LO` | — | |
| 30030 | `DIAG_LAST_FAULT_CODE` | enum | Last recorded fault |
| 30031 | `DIAG_HEARTBEAT_ECHO` | — | Last heartbeat value received |
| 30032-30043 | `DIAG_SERIAL_TEXT` | — | Full serial string, 12 registers, 2 ASCII chars each (high byte first), null-padded |

### 7.3 Serial number (simulated OTP)

The product identity is **written once** into a persistent file and only read
afterwards, like factory programming into OTP:

```text
/var/lib/lamp/device_identity.txt
model=SL
hw_rev=1
lot=LA
week=2647
id=8F2A036D
serial=SL-1-LA-2647-8F2A036D
provisioned_at=2026-11-23T21:00:00Z
```

- **First boot:** if `DEVICE_IDENTITY_PATH` points to a missing file, the driver
  creates it atomically and leaves it read-only (`0444`). `PROVISION_LOT`,
  `PROVISION_WEEK` (YYWW) and `PROVISION_ID` override the generated values; by
  default the lot is `LA`, the week is the current ISO week and the id comes
  from `/dev/urandom`.
- **Later boots:** the file is read and never rewritten. Factory reset deletes
  `driver_state.bin`, not the identity. Reflashing the firmware does not touch
  it because it lives on the driver volume, not in `firmware-flash`.
- **Modbus:** `30025-30026` carries the 32-bit `id`; `30032-30043` carries the
  full serial as ASCII. The firmware reads the text and publishes it in
  `POST /telemetry/health`, so the dashboard and the Simulation Panel show the
  same value.
- **Limits of the simulation:** the file is a normal file with mode `0444`, not
  a hardware OTP. Root can delete or replace it, and a different deployment can
  pre-mount its own identity. That is enough to demonstrate the write-once,
  persistence and traceability flow without a real eFuse.
- **No `DEVICE_IDENTITY_PATH`:** the driver falls back to `DEVICE_SERIAL` or a
  hash of `DEVICE_ID`, which keeps tests and ad-hoc runs working.

### 7.4 Fault semantics

- **Overtemperature warning (1):** the driver limits `ACTUAL_BRIGHTNESS` to
  **500** (50 %) while the condition remains active; it does not latch. When the
  condition disappears, brightness returns to the target.
- **Critical (2), overcurrent (3) and open LED (4):** the driver turns the output
  off, stops the sequence and **latches** the code. `STATUS_FAULT_LATCHED`
  stays at 1; `POWER_ON`, `APPLY_TARGET`, `START_SEQUENCE` and `LOAD_CONFIG` are
  ignored until the fault is cleared. After `CMD_CLEAR_FAULTS` the lamp stays
  off: it must be turned on explicitly.
- **Heartbeat lost (7):** turns the output off and saves the target. When the
  heartbeat returns, it restores the target unless there is a latch or a
  configuration in safe mode. It does not latch by itself.
- **Corrupt config (5):** enters **safe mode** (10013). Restores the default
  configuration limits, turns the output off and blocks `SAVE_CONFIG`,
  `LOAD_CONFIG`, `POWER_ON`, `APPLY_TARGET` and `START_SEQUENCE`.
  `CMD_CLEAR_FAULTS` leaves safe mode and leaves the lamp off; the configuration
  must then be reviewed and saved to repair the file. `CMD_FACTORY_RESET` also
  erases the corrupt state.
- **Invalid sequence (6):** diagnostic only; it does not block the output.
- Simulated faults also increment `DIAG_FAULT_COUNT` and update
  `DIAG_LAST_FAULT_TS`. The `config_corrupt` injection does not disappear when
  its duration ends: safe mode persists until it is cleared.

## 8. Sequences

### 8.1 Predefined (IDs 1-15)

The firmware only writes `SEQ_ID` and `CMD_START_SEQUENCE`.

| ID | Name | Description |
| :--- | :--- | :--- |
| 1 | `breathing` | Smooth fade in/out, 4 s cycle |
| 2 | `rainbow` | Full hue cycle, 10 s |
| 3 | `strobe` | Fast flash, 10 Hz |
| 4 | `candle` | Random warm flicker |
| 5 | `police` | Alternating red/blue |
| 6 | `sunrise` | Gradual sunrise, 60 s |
| 7 | `pairing` | Slow white pulse for pairing |
| 8-15 | Reserved | |

### 8.2 Custom (IDs 100-255) — upload flow

```text
1. FC16: SEQ_TOTAL_STEPS = N              (40016)
2. For each step i (0..N-1):
   a. FC16: SEQ_STEP_INDEX = i            (40015)
   b. FC16: 8 step registers              (40017-40024)
   c. (optional) FC03: read-back verify
3. FC16: SEQ_CRC = CRC16(steps)           (40025)
4. FC16: SEQ_ID = 100 (or 100+custom_id)  (40011)
5. FC05: CMD_START_SEQUENCE = 1           (00004)
```

**CRC:** CRC-16/CCITT-FALSE (init `0xFFFF`, polynomial `0x1021`, no
reflection) over the 8 registers of each step in order, low byte then high byte.
The backend (`src/sequences/crc.py`) and the driver implement the same
algorithm.

**Integrity check:** the driver checks the CRC and the number of steps when it
receives `CMD_START_SEQUENCE`. If they do not match, it does not start and sets
`STATUS_SEQUENCE_INVALID` and `DIAG_FAULT_CODE = 6`.

**Implementation:** `POST /instances/{id}/sequences` uploads the steps
and sends `start_sequence` to the firmware; the dashboard editor stores up to 8
steps in `localStorage` and allows reordering, changing the duration or stopping
the sequence.

### 8.3 Driver state persistence

`DRIVER_STATE_PATH` (default `/var/lib/lamp/driver_state.bin` in the instance
image) stores 20 registers: power, mode, H, S, V, CCT, brightness, scene, the
sequence config and the configuration block (brightness limits, power-on
behavior, gamma, heartbeat timeout, over-temperature thresholds and maximum
current). The format is
`magic(4) + version(2) + count(2) + values + CRC16(2)` in little-endian; it is
written to `<path>.tmp` and renamed (atomic). The driver loads it on startup,
saves every 2 s if there are changes and erases it on `CMD_FACTORY_RESET`.
Files with the 12-register layout are migrated on load: the target and sequence
state is restored and the configuration keeps its defaults.
The backend also sends that reset to the driver when performing a factory reset
from the Sim Panel.

## 9. Firmware polling strategy

| Task | Frequency | Transaction | Registers |
| :--- | :--- | :--- | :--- |
| `vLightPollTask` | 20 Hz (50 ms) | FC04 read 30001-30009 | 9 |
| `vDiagPollTask` | 1 Hz | FC04 read 30011-30026 + 30032-30043 | 28 |
| `vHeartbeatTask` | 1 Hz | FC06 write 40041 (counter++) | 1 |
| `vWatchdogTask` | 200 ms | FC01 read 10001-10011 (status) | 11 |

Each transaction is a small TCP frame (~20-40 bytes). At 20 Hz that is ~800 bytes/s, negligible.

## 10. Transaction examples

### 10.1 Turning on warm orange with a 500 ms transition

```text
FC16 write 40001-40005 = [1, 0, 32, 850, 600]   # power, mode, H, S, V
FC16 write 40007-40008 = [800, 500]              # brightness, transition
FC05 write 00001 = 1                             # CMD_APPLY_TARGET
```

### 10.2 Starting the "breathing" sequence at 1.5×

```text
FC16 write 40011-40014 = [1, 1500, 0, 0]         # seq_id, speed, dir, loop
FC05 write 00004 = 1                             # CMD_START_SEQUENCE
```

### 10.3 Reading the actual state (what the firmware does at 20 Hz)

```text
FC04 read 30001-30009 → [1, 0, 32, 850, 600, 0, 800, 0, 0]
# power=on, mode=manual, H=32°, S=85%, V=60%, CCT=0, bright=80%, seq=0, progress=0
```

### 10.4 Detecting that a transition is in progress

```text
FC04 read 30009 → 450
# progress = 45% → the driver is mid-transition; the next poll returns the actual color
```

### 10.5 Simulating overheating from the Simulation Panel

The panel **cannot write input registers** (they are read-only). What it does is inject the fault into the **simulated driver** through its internal REST API (`POST /sim/inject_fault`), and the driver updates its own `DIAG_TEMP_C_X10` and `STATUS_OVERTEMP_*`. The firmware detects it on the next poll and reacts.

### 10.6 Uploading a 3-step custom sequence

```text
# Step 0: red, 1s, 100ms transition
FC16 40015 = 0
FC16 40017-40022 = [0, 1000, 600, 800, 100, 10]
# Step 1: green, 1s, 100ms transition
FC16 40015 = 1
FC16 40017-40022 = [120, 1000, 600, 800, 100, 10]
# Step 2: blue, 1s, 100ms transition
FC16 40015 = 2
FC16 40017-40022 = [240, 1000, 600, 800, 100, 10]
# Set the total and start
FC16 40016 = 3
FC16 40011 = 100                                  # custom id 100
FC05 00004 = 1                                    # CMD_START_SEQUENCE
```

## 11. Notes and trade-offs

- **Hue in 0-360** wastes resolution (only 361 values in 16 bits). If more smoothness were wanted, 0-3600 (0.1°) could be used and the dashboard adjusted. It is left at 0-360 for readability; this is a conscious decision.
- **There is no "reset" register** in coils: the driver must not be able to restart itself over Modbus. Device reset is controlled by the supervisor (outside the Modbus bus).
- **The heartbeat is one-directional** (firmware → driver). If the driver dies, the firmware detects it through a TCP connection timeout or when `DIAG_HEARTBEAT_ECHO` stops updating.
- **32-bit in CDAB:** low word first, big-endian within each word; it is a classic source of interoperability bugs in Modbus.
- **Scalability:** if more LEDs are added in the future (RGBW, addressable strips), the map grows with additional blocks starting at 40051 and 30041, without breaking compatibility.
- **Modbus RTU is not modeled:** although the real driver could be RTU over RS-485, TCP is simpler for the simulation and the register map is identical; this is a simulation decision.
- **Edge semantics on coils:** the auto-reset to 0 after execution prevents re-triggering. It is a design decision that differs from "pure" Modbus but is common in real integrations with LED drivers.
- **Commands in coils vs. holding:** coils were chosen for one-shot actions (triggering) and holding for persistent configuration. This is the standard separation in industrial integrations.
- **Unit ID 1:** in TCP it is not strictly necessary, but it is kept for compatibility with future TCP↔RTU bridges.

## 12. References

- [Modbus Application Protocol V1.1b3](https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- [Modbus Messaging on TCP/IP V1.0b](https://modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf)
- [libmodbus documentation](https://libmodbus.org/documentation/)
