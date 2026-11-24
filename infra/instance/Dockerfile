# Ephemeral instance image (one Fly.io Machine per visit).
#
# Build context: repository root.
#   docker build -f infra/instance/Dockerfile -t lamp-instance .
#
# Bundles the Modbus driver, firmware, bootloader and supervisor in a single
# container; the orchestrator talks to the supervisor over 6PN (5022) and to
# the driver over Modbus (5020).

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target firmware supervisor bootloader driver

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/bin/firmware /app/firmware
COPY --from=build /src/build/bin/bootloader /app/bootloader
COPY --from=build /src/build/bin/supervisor /app/supervisor
COPY --from=build /src/build/bin/driver /app/driver
COPY --from=build /src/build/lib/libmodbus.so* /app/
COPY keys/ota_public.pem /keys/ota_public.pem
COPY infra/instance/entrypoint.sh /app/entrypoint.sh

RUN chmod +x /app/entrypoint.sh /app/driver /app/firmware \
    /app/bootloader /app/supervisor \
    && mkdir -p /var/lib/lamp /data/ota

ENV LD_LIBRARY_PATH=/app \
    FLASH_PATH=/var/lib/lamp/flash.bin \
    DRIVER_STATE_PATH=/var/lib/lamp/driver_state.bin \
    DEVICE_IDENTITY_PATH=/var/lib/lamp/device_identity.txt \
    MODBUS_HOST=127.0.0.1 \
    MODBUS_PORT=5020 \
    MODBUS_UNIT_ID=1 \
    SUPERVISOR_PORT=5022 \
    BOOTLOADER_BIN=/app/bootloader \
    FACTORY_APP=/app/firmware \
    OTA_PUBLIC_KEY_PATH=/keys/ota_public.pem \
    HW_REV=1

EXPOSE 5020 5022
ENTRYPOINT ["/app/entrypoint.sh"]
