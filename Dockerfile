# AirBandScan in Docker, with the RTL-SDR Blog driver (needed for RTL-SDR Blog V4 dongles).
# Nothing in it is x86-specific, so it should also build on a Raspberry Pi (arm64 or armhf):
#   docker compose up -d --build      then open http://<host-ip>:8080/

# ---- RTL-SDR Blog driver ----
FROM debian:bookworm-slim AS driver

ARG RTLSDR_BLOG_VERSION=v1.3.6

RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake build-essential libusb-1.0-0-dev pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Detach the kernel DVB driver from the dongle at open, since it can't be blacklisted from inside a container
RUN git clone --depth 1 --branch "$RTLSDR_BLOG_VERSION" https://github.com/rtlsdrblog/rtl-sdr-blog.git /tmp/rtl-sdr-blog \
    && cmake -S /tmp/rtl-sdr-blog -B /tmp/rtl-sdr-blog/build -DCMAKE_BUILD_TYPE=Release \
       -DDETACH_KERNEL_DRIVER=ON -DINSTALL_UDEV_RULES=OFF \
    && cmake --build /tmp/rtl-sdr-blog/build -j"$(nproc)" \
    && cmake --install /tmp/rtl-sdr-blog/build --prefix /usr/local \
    && DESTDIR=/tmp/driver_out cmake --install /tmp/rtl-sdr-blog/build --prefix /usr/local

# ---- AirBandScan ----
FROM driver AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    ninja-build libairspy-dev libairspyhf-dev libhackrf-dev libiio-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
RUN ldconfig \
    && cmake -S /src -B /src/build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && ninja -C /src/build-docker airbandscan \
    && strip /src/build-docker/airbandscan

# ---- Runtime ----
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libusb-1.0-0 libairspy0 libairspyhf1 libhackrf0 libiio0 libmp3lame0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=driver /tmp/driver_out/usr/local/ /usr/local/
RUN ldconfig

WORKDIR /app
COPY --from=builder /src/build-docker/airbandscan /app/airbandscan
COPY airband.conf.example /app/airband.conf.example
RUN mkdir -p /app/config /app/recordings

ENV CONFIG_FILE=/app/config/airband.conf
ENV WEB_PORT=8080
ENV RECORDINGS_DIR=/app/recordings
ENV EXTRA_ARGS=""

EXPOSE 8080

VOLUME ["/app/recordings"]
VOLUME ["/app/config"]

# exec, so docker stop reaches airbandscan and it shuts down cleanly
ENTRYPOINT ["sh", "-c", "exec ./airbandscan -c \"$CONFIG_FILE\" -p \"$WEB_PORT\" -d \"$RECORDINGS_DIR\" $EXTRA_ARGS"]
