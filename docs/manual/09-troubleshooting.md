# Common issues

## The dashboard does not load or says “backend unavailable”

1. Check that the stack is up:
   ```bash
   docker compose ps
   curl http://localhost:8080/health
   ```
2. Check the logs:
   ```bash
   docker compose logs --tail=100 backend
   ```
3. If you just changed `PUBLIC_ORCHESTRATOR_URL` or `DASHBOARD_ORIGINS`,
   rebuild web/backend.

## The device shows as disconnected

- `docker compose ps` should show `firmware` and `driver` up.
- Check the `firmware` logs; it should connect to the backend WS.
- If the backend restarted, wait a few seconds: the firmware reconnects on its
  own.
- If nothing works, `docker compose restart backend firmware`.
- If the device is powered off on purpose, use `Power On` in the Sim panel. An
  unpowered lamp cannot be started from the dashboard, just like a real one.

## The controls are gray

It is not always a bug. It can be:

- `stale` state (no heartbeat);
- network in `pairing`/`connecting`;
- a latched critical fault;
- configuration safe mode.

Check the banner and the network badge. If there is a fault, press `Clear faults`
and turn the lamp back on.

## I cannot pair

- Use **Power Off/On** in the Sim panel, not the dashboard's `Turn off`.
- Wait for `APP_RUNNING` after each power-on.
- Do 3 cycles of 1–10 s; not a fourth.
- `Add device` only scans; it does not force pairing.
- If nothing appears, check Sim panel → Network → `Start pairing`.

## The color does not match what I asked for

- During a transition, the swatch shows the real state. Wait for it to finish.
- If a sequence is active, the sequence controls the color.
- An overtemperature warning can limit brightness to 50 %.

## A fault appears and I cannot turn it on

That's the expected behavior for critical faults:

1. Withdraw the injection or the cause.
2. `Clear faults`.
3. `Turn on` manually.

For safety, clearing does not turn the lamp on.

## OTA does not change version/slot

- If the artifact is the same or older, anti-downgrade rejects it.
- If it is corrupt, the signature/GCM check fails.
- The panel should say `No change…`; that's correct.
- To get a slot change you need a **newer** artifact.

## The schedule does not run

- The backend must be awake.
- Check that the schedule is enabled, and the time and zone.
- If the device is in pairing/connecting, it may not be applied.
- A factory reset deletes schedules.

## My sequence disappeared

- Sequences live in `localStorage`.
- Switching browsers, clearing data or `Reset dashboard` deletes them.
- They are not saved to flash or synced between devices.

## The microphone does not work

- The browser must ask for permission; accept it.
- It works on `localhost` or HTTPS; not on remote HTTP.
- If it's denied, choose `Simulated beat`.
- The tab must stay open.

## History persists after a factory reset

In the current version, the Sim panel factory reset clears the history. If you
see old events:

- press `Clear history` under Faults;
- reload the panel;
- check that you're on the right instance.

## I'm still stuck

Gather this information before asking for help:

- `docker compose ps`
- `curl http://localhost:8080/health`
- logs from `backend`, `firmware` and `driver`
- instance (`inst-…`) and version/slot
- exact steps to reproduce it

[Back to the manual](README.md)
