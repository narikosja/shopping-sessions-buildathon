FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir --no-deps --only-binary=:all: pyarrow==21.0.0

WORKDIR /src
COPY src/ src/
COPY scripts/build.sh scripts/build.sh
RUN ARROW_DIR=/usr/local/lib/python3.12/site-packages/pyarrow \
    ARROW_RPATH=/opt/shopping/lib scripts/build.sh /tmp/shopping-sessions \
    && mkdir -p /opt/shopping/lib \
    && cp /tmp/shopping-sessions /opt/shopping/ \
    && cp /usr/local/lib/python3.12/site-packages/pyarrow/libarrow.so.2100 \
          /usr/local/lib/python3.12/site-packages/pyarrow/libparquet.so.2100 /opt/shopping/lib/

FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c
RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/shopping /opt/shopping
USER 1000:1000
CMD ["/opt/shopping/shopping-sessions"]
