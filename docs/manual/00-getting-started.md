# Getting Started

## 1. Start the demo

From the repository root:

```bash
cp .env.example .env
docker compose up --build
```

Wait until the backend is healthy and open:

- Dashboard: <http://localhost:3000>
- Sim panel: <http://localhost:3000/sim.html?pin=2025>
- Interactive API: <http://localhost:8080/docs>

The default ports are `3000` (web), `8080` (backend) and `5020` (Modbus).

Check the stack:

```bash
docker compose ps
curl http://localhost:8080/health
```

You should see `backend`, `driver`, `firmware` and `web`; the health check should
return `{"status":"ok"}`. To stop without deleting data:

```bash
docker compose down
```

Don't use `docker compose down -v` unless you want to delete the flash volume and
the demo SQLite databases.

## 2. Understand the two panels

| Panel | What it's for |
| :--- | :--- |
| **Dashboard** | Normal use: power, color, scenes, sequences, schedules, power-on |
| **Sim panel** | Engineering/QA: device, Modbus, firmware, faults, logs, network |

The Sim panel asks for PIN `2025`. It is not a real admin screen: it's a
demonstration tool for injecting states and watching reactions.

## 3. Soft power vs. hard power

- **Turn off / Turn on** in the dashboard is **soft power**. It keeps Wi-Fi and
  does not count toward pairing.
- **Power Off / Power On** in the Sim panel is **hard power**. It restarts the
  firmware and does count toward pairing.

When you open the dashboard or the Simulation Panel on a fresh instance, the
lamp starts and turns on by itself. If you did an explicit `Power Off` in the
Simulation Panel, that is respected: the device stays disconnected and the
dashboard cannot start it. Use `Power On` there, exactly like the physical
switch of an unplugged lamp.

If you're pairing, always use the Sim panel buttons.

## 4. Check that you are connected

The dashboard should show two green badges:

- `device connected`: the lamp is powered and the firmware is connected to the
  backend. This badge follows the device, not the browser connection.
- `Wi-Fi · Casa-Demo`: the instance has a simulated network.

If the lamp is powered off, the Simulation Panel stopped the firmware, or the
device is still booting, the badge reads `device disconnected` and the controls
stay locked. A red or amber banner appears if the backend itself is unreachable.
See [Common issues](09-troubleshooting.md).

## 5. Reset the demo

To get back to a clean state:

- **Dashboard → Settings → Reset dashboard**: clears sequences, adaptive white,
  party mode, schedules, power-on and history. It does not erase flash.
- **Sim panel → Device → Factory reset**: clears flash, driver, schedules,
  power-on and history. It leaves the lamp like new.

The factory reset asks for confirmation. Afterwards you'll have to pair again if
you want to use the simulated network.

## Next

- [Dashboard](01-dashboard.md)
- [Pairing](02-pairing.md)
- [Common issues](09-troubleshooting.md)

[Back to the manual](README.md)
