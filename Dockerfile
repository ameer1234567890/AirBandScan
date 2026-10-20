FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake build-essential libusb-1.0-0-dev pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/rtlsdrblog/rtl-sdr-blog.git /tmp/rtl-sdr-blog \
    && mkdir -p /tmp/rtl-sdr-blog/build \
    && cd /tmp/rtl-sdr-blog/build \
    && cmake ../ -DDETACH_KERNEL_DRIVER=ON \
    && make -j$(nproc) \
    && make install DESTDIR=/tmp/driver_out

FROM debian:bookworm-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    libhackrf0 \
    libairspy0 \
    libairspyhf1 \
    libiio0 \
    libmp3lame0 \
    libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /app/config

COPY --from=builder /tmp/driver_out/usr/local/ /usr/local/

RUN ln -sf /usr/local/lib/librtlsdr.so /usr/lib/librtlsdr.so.0 \
    && ln -sf /usr/local/lib/librtlsdr.so /usr/lib/x86_64-linux-gnu/librtlsdr.so.0 2>/dev/null || true \
    && ldconfig

#RUN ldconfig

COPY ./airbandscan /app

ENV CONFIG_FILE=/app/config/airband.conf
ENV WEB_PORT=8080
ENV RECORDINGS_DIR=/app/recordings
ENV EXTRA_ARGS=""

ENV LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

EXPOSE 8080

VOLUME ["/app/recordings"]
VOLUME ["/app/config"]

ENTRYPOINT ["sh", "-c", "./airbandscan -c \"$CONFIG_FILE\" -p \"$WEB_PORT\" -d \"$RECORDINGS_DIR\" $EXTRA_ARGS"]