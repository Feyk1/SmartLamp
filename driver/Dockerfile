FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target driver

FROM debian:bookworm-slim

WORKDIR /app
COPY --from=build /src/build/bin/driver /app/driver
COPY --from=build /src/build/lib/libmodbus.so* /app/

ENV LD_LIBRARY_PATH=/app
ENV MODBUS_PORT=5020

EXPOSE 5020
CMD ["/app/driver"]
