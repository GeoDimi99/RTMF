FROM ubuntu:24.04

# Install build tools
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        cmake \
        build-essential \
        libglib2.0-dev \
        libhiredis-dev \
        libgrpc++-dev \
        libprotobuf-dev \
        protobuf-compiler-grpc && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy proto files
COPY proto/ ./proto/

# Generate gRPC code from proto
RUN protoc --cpp_out=./proto --grpc_out=./proto \
    --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
    ./proto/task_service.proto

# Copy common headers
COPY common/ ./common/

# Copy execution-manager source
COPY services/execution-manager/ ./execution-manager/

# Copy generated proto files into execution-manager
RUN mkdir -p ./execution-manager/proto/proto/ && \
    cp ./proto/proto/*.pb.cc ./execution-manager/proto/proto/ && \
    cp ./proto/proto/*.pb.h ./execution-manager/proto/proto/ && \
    cp ./proto/proto/*.grpc.pb.cc ./execution-manager/proto/proto/ && \
    cp ./proto/proto/*.grpc.pb.h ./execution-manager/proto/proto/ && \
    ls -la ./execution-manager/proto/proto/

WORKDIR /app/execution-manager

# Configure and build
RUN cmake -S . -B build
RUN cmake --build build

ENTRYPOINT ["./build/execution-manager"]

# For debugging memory leaks, uncomment below and comment above:
#ENV G_DEBUG=gc-friendly
#ENV G_SLICE=always-malloc
#ENTRYPOINT ["valgrind", "--leak-check=full", "--show-leak-kinds=all", "--track-origins=yes", "./build/execution-manager"]
