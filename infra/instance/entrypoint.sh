#!/bin/sh
# Start the simulated Modbus driver and leave the supervisor as PID 1.
# The supervisor does the firmware auto power-on and serves the control
# channel (5022) that the orchestrator uses for power/reset/factory reset.
#
# The firmware retries Modbus if the driver is not listening yet, so there is
# no need to wait actively. When the Machine stops, the runtime sends SIGTERM
# to the supervisor (PID 1) and the rest of the container processes terminate.
set -eu

/app/driver &
exec /app/supervisor
