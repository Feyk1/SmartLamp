#!/bin/sh
set -eu

# Fly's public proxy expects the app on IPv4 (0.0.0.0:8080), while 6PN
# addresses are IPv6. Expose the same process on IPv6:8081 for ephemeral
# Machines, which reach it through <app>.internal.
socat TCP6-LISTEN:8081,fork,reuseaddr TCP4:127.0.0.1:8080 &
exec uvicorn src.main:app --host 0.0.0.0 --port 8080
