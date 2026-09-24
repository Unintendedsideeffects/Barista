FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates pkg-config \
    libfreetype6-dev libnl-3-dev libnl-genl-3-dev libssl-dev libx11-dev libxtst-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build/radio -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DBARISTA_VERSION=0.1.47 -DBARISTA_BUILD_DESKTOP=OFF -DBUILD_TESTING=ON \
    && cmake --build /build/radio --parallel 2 \
    && ctest --test-dir /build/radio --output-on-failure
RUN cmake -S examples/on-demand -B /build/bridge -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAMEPAD_BUILD_PROBE=ON \
    && cmake --build /build/bridge --parallel 2

FROM debian:bookworm-slim AS radio
LABEL io.barista.upstream-revision="3bad9db554d71f1ae733d250f64fab0598d97d2b"
RUN apt-get update && apt-get install -y --no-install-recommends \
    libfreetype6 libnl-3-200 libnl-genl-3-200 libssl3 iproute2 iw \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/radio/app/drcd /usr/local/bin/barista-engine
COPY --from=build /build/radio/third_party/hostapd-drc/bin/drc-hostapd /usr/local/bin/barista-hostapd
COPY --from=build /src/assets/fonts/jost/static/Jost-Regular.ttf /usr/local/share/barista/fonts/
COPY --from=build /src/assets/fonts/jost/static/Jost-SemiBold.ttf /usr/local/share/barista/fonts/
COPY --from=build /src/assets/sounds/ui.pcm /usr/local/share/barista/sounds/
COPY --from=build /src/assets/images/barista-logo-56.rgba /usr/local/share/barista/images/
COPY examples/on-demand/runtime/radio-entrypoint.sh /usr/local/bin/gamepad-radio
ENTRYPOINT ["/usr/local/bin/gamepad-radio"]

FROM debian:bookworm-slim AS dashboard
LABEL io.barista.upstream-revision="3bad9db554d71f1ae733d250f64fab0598d97d2b"
RUN apt-get update && apt-get install -y --no-install-recommends \
    chromium xvfb openbox x11-utils libxtst6 python3 fonts-dejavu-core ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 1000 --create-home --shell /usr/sbin/nologin gamepad
COPY --from=build /build/bridge/barista-dashboard-bridge /usr/local/bin/barista-dashboard-bridge
COPY examples/on-demand/runtime/dashboard.py /usr/local/bin/gamepad-dashboard
USER gamepad
ENTRYPOINT ["python3", "/usr/local/bin/gamepad-dashboard"]

FROM radio AS validation
COPY --from=build /build/bridge/barista-media-probe /usr/local/bin/barista-media-probe
ENTRYPOINT ["/usr/local/bin/barista-media-probe"]
