# Dockerfile — Build gogol for Linux and produce a minimal base image
# Usage:
#   docker build -t gogol-base .
#   docker run --rm gogol-base gogol --help
#
# For agent containers:
#   FROM gogol-base
#   ENV GOGOL_HOST=host.docker.internal:9400
#   ENV GOGOL_KEY_NAME=agentic
#   ENV GOGOL_KEY=<key>

FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN git submodule update --init --recursive 2>/dev/null || true
# GGML_NATIVE=OFF: build a portable CPU binary (no host -march=native feature
# detection). Required on aarch64 where GCC gates FP16 NEON intrinsics
# (vfmaq_f16) behind an arch feature that -march=native doesn't enable here,
# and correct for a distributable image that may run on varied hardware.
# BUILD_SHARED_LIBS=OFF: statically link llama/ggml into the gogol binary so the
# minimal runtime image needs no libllama.so/libggml*.so at runtime.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DLLAMA_METAL=OFF -DGGML_METAL=OFF \
    -DLLAMA_CUDA=OFF -DGGML_CUDA=OFF \
    -DGGML_NATIVE=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    && cmake --build build --target gogol -j$(nproc)

# Verify it runs
RUN ./build/gogol --help

# --- Minimal runtime image ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libgomp1 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/gogol /usr/local/bin/gogol

# Default env vars (override in child images or docker run)
ENV GOGOL_HOST=""
ENV GOGOL_KEY_NAME=""
ENV GOGOL_KEY=""

ENTRYPOINT ["gogol"]
