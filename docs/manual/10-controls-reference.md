# Controls Reference

This page lists everything you can do from the app. If a control is not listed
here, it is not part of the public demo.

## Local assistant

| Control | What it does |
| :--- | :--- |
| `Ask` | Opens the rule-based assistant |
| `Apply` / `Cancel` | Confirms or cancels a proposed action |
| Suggestion chips | Run a common question |
| `Send` | Sends the typed question |

## Dashboard header

| Control | What it does |
| :--- | :--- |
| `EN / ES` | Switches the language; saved in the browser |
| `device connected` | Device presence; green = powered and connected |
| `Wi-Fi · SSID` | The instance's simulated network |
| `Help` | Quick guide in a dialog |
| `Settings` | Reset the dashboard and clear history |
| `Add device` | Scans for and connects a lamp in pairing |
| `Simulation panel` | Opens the Sim panel in another page |

## Light card

| Control | What it does |
| :--- | :--- |
| Large swatch | Real lamp state; when off it shows the off icon |

> There is only one swatch, and it shows the actual state. The Simulation
> Panel's Modbus tab shows target (`TARGET_*`) and actual (`ACTUAL_*`) values.
| `Turn on` / `Turn off` | Soft power off; keeps Wi-Fi |
| `Soft power` text | Reminds you that power off does not count toward pairing |
| `State` | Last known state (on/off, color, brightness) |
| `Health` | Firmware version, heap and temperature |
| `Last message` | Last ack/nack or app message |
| `Update firmware` | Opens the update flow |
| `POWER-ON` | Policy for a hard Power On |
| `LIGHT` | Color/white selector |
| `White` | CCT mode, 2000–6500 K |
| `Color` | HSV wheel and presets |
| `Adaptive white` | Follows local time; `Configure` edits points |
| `Brightness` | 0–1000; reflected in the swatch |
| `SCENES` | Reading, Relax, Focus, Night |
| `SCHEDULES` | Summary and `Manage` |
| `EFFECTS` | Breathing, Rainbow, Candle, Strobe, Police, Sunrise |
| `PARTY` | Browser microphone or simulated beat |
| `SEQUENCES` | Summary and `Manage` |

## Dashboard dialogs

| Dialog | Controls |
| :--- | :--- |
| `Help` | Quick guide to pairing, schedules, sequences and credentials |
| `Settings` | Device serial, `Reset dashboard`, `Clear fault history` |
| `Add device` | Scan, SSID, password, `Connect` |
| `Schedules` | Time, repeat, days, action, `Add schedule`, list, edit/delete, `Reset schedules` |
| `Sequences` | `Add step`, swatch, duration, arrows, delete, `Fade`, `Speed`, `Start`, `Stop`, `Reset` |
| `Adaptive white` | Time/temperature points, `Add point`, transition, brightness, `Reset`, `Apply` |
| `Color picker` | Color/White, wheel, presets, brightness, `Apply` |

## Sim panel header

| Control | What it does |
| :--- | :--- |
| `EN / ES` | Language |
| `connected` | Backend state |
| `Help` | Simulation guide |
| `Sign out` | Clears the panel session |
| Tabs | `Device`, `Firmware`, `Modbus`, `Faults`, `Logs`, `Network` |

## Device tab

| Control | What it does |
| :--- | :--- |
| `State` | Current firmware state |
| `Health` | Serial, slot, version, uptime, heap, temperature |
| `RTOS metrics` | Stack high-water mark per task |
| `Power On` / `Power Off` | Hard power; counts toward pairing |
| `Reset` | Restarts the firmware |
| `Enter bootloader` | Leaves the device in recovery |
| `Factory reset` | Clears flash, driver, schedules, power-on and history |
| `Timeline` | Recent events |

## Firmware tab

| Control | What it does |
| :--- | :--- |
| `Current` | Current version and slot |
| `Mode` | `Legit`, `Corrupt`, `Downgrade` |
| Artifact table | Version, size, badge and `Deploy` |
| `fw-status` | Wait, result `Updated` or `No change` |

## Modbus tab

| Control | What it does |
| :--- | :--- |
| `Type` | Holding, input, coil, discrete |
| `Address` / `Value` | Address and value to read/write |
| `Apply target` | Applies the target after writing |
| `Read` | Reads the range |
| `Write` | Writes the register/coil |
| `Auto-refresh` | Reads at 1 Hz |
| Tables | Register map and current values |

Modbus writes go **straight to the driver**; the firmware does not see them as a
legitimate command. It is a QA tool. The driver normalizes out-of-range values
before storing them: numeric ranges are clamped, invalid enum values fall back to
their default, and `ACTUAL_BRIGHTNESS` always stays inside the configured
limits. Read the register again after writing to see the accepted value.

## Faults tab

| Control | What it does |
| :--- | :--- |
| `Duration` | Seconds; `0` = until cleared |
| Catalog | Injects each fault with `Inject` |
| `Clear faults` | Clears injection and latch |
| `Clear history` | Clears only fault events |
| History | Time, origin, fault, code, severity, effect |

## Logs tab

| Control | What it does |
| :--- | :--- |
| Filter | Searches by text |
| List | Events with level and timestamp |
| Download | Exports a `.log` |

## Network tab

| Control | What it does |
| :--- | :--- |
| `Start pairing` | Enters pairing without cycles |
| `Cancel pairing` | Exits pairing |
| `Forget Wi-Fi & pair` | Forgets the network and pairs |
| `Refresh` | Re-reads the state |
| Whitelist | Adds/edits SSIDs and passwords |
| `Allow any` | Accepts any SSID |
| Faults | Wrong password, unknown network, timeout |

## What the app does not do

- It does not control real hardware; the firmware is POSIX.
- It does not let you choose a slot manually.
- It does not bypass anti-downgrade.
- It does not install corrupt images.
- It does not use the microphone to send audio to the backend.
- It does not use an LLM; the local assistant is rule-based.

[Back to the manual](README.md)
