FROM ubuntu:24.04

# Install build tools
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        cmake \
        build-essential \
        libglib2.0-dev \
        libjson-glib-dev \
        libhiredis-dev \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy common headers
COPY common/ ./common/

# Copy execution-manager source
COPY services/execution-manager/ ./execution-manager/

WORKDIR /app/execution-manager

# Configure and build
RUN cmake -S . -B build
RUN cmake --build build

#ENTRYPOINT ["./build/execution-manager"]
ENV G_DEBUG=gc-friendly
ENV G_SLICE=always-malloc

#ENTRYPOINT ["valgrind", "--leak-check=full", "--show-leak-kinds=all", "--track-origins=yes", "./build/execution-manager"]
ENTRYPOINT ["./build/execution-manager"]
