FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target firmware supervisor bootloader

FROM debian:bookworm-slim

WORKDIR /app
COPY --from=build /src/build/bin/firmware /app/firmware
COPY --from=build /src/build/bin/bootloader /app/bootloader
COPY --from=build /src/build/bin/supervisor /app/supervisor
COPY --from=build /src/build/lib/libmodbus.so* /app/

ENV LD_LIBRARY_PATH=/app
ENV FLASH_PATH=/var/lib/lamp/flash.bin
ENV MODBUS_HOST=driver
ENV MODBUS_PORT=5020
ENV ORCH_WS_URL=ws://backend:8080/ws/device
ENV ORCH_HTTP_URL=http://backend:8080
ENV DEVICE_ID=lamp-8f2a
ENV BOOTLOADER_BIN=/app/bootloader
ENV FACTORY_APP=/app/firmware
ENV OTA_PUBLIC_KEY_PATH=/keys/ota_public.pem
ENV HW_REV=1
ENV DEVICE_TOKEN=dev-token-local

VOLUME ["/var/lib/lamp"]
ENTRYPOINT ["/app/supervisor"]
