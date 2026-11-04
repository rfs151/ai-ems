ARG REGISTRY=docker.io
FROM ${REGISTRY}/library/ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev \
      libmodbus-dev libmosquitto-dev libsqlite3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make clean && make -j"$(nproc)"

FROM ${REGISTRY}/library/ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libmodbus5 libmosquitto1 libsqlite3-0 tzdata \
      && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/ems-core /src/build/ems-sync /usr/local/bin/
COPY config ./config
ENV EMS_DB=/data/ems.db
VOLUME /data
CMD ["ems-core", "-c", "/app/config"]
