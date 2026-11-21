FROM python:3.12.12-bookworm@sha256:c0abd0758831ad99b7a29e0c1a875da9c4abb9a2e3f21e2eeb585dbcadfb6cd0 AS build
WORKDIR /build
COPY requirements.txt .
RUN python3 -m pip install --no-cache-dir --only-binary=:all: -r requirements.txt
COPY src/ src/
COPY scripts/build.sh scripts/build.sh
RUN scripts/build.sh && mkdir /application && \
    cp build/shopping-sessions /application/ && \
    arrow_dir="$(python3 -c 'import pyarrow; print(pyarrow.get_library_dirs()[0])')" && \
    cp "$arrow_dir/libarrow.so.2200" "$arrow_dir/libparquet.so.2200" /application/

FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c
COPY --from=build /application/ /app/
RUN /sbin/ldconfig && ldd /app/shopping-sessions | tee /tmp/linked-libraries && \
    ! grep -q 'not found' /tmp/linked-libraries && rm /tmp/linked-libraries
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch HOME=/scratch NCPU=8 MEM_GB=24
USER 1000:1000
ENTRYPOINT ["/app/shopping-sessions"]
