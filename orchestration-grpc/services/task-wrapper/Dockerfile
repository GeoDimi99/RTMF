FROM ubuntu:24.04

# Install build tools and dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
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

# Debug: list generated files
RUN echo "=== Files in proto directory ===" && ls -la ./proto/ && echo "=== End list ==="

# Copy task-wrapper source
COPY services/task-wrapper/ ./task-wrapper/
COPY common/include/ ./task-wrapper/include/
COPY common/src/ ./task-wrapper/src/

# Create proto directory structure and copy generated proto files into task-wrapper
# The generated .grpc.pb.h includes "proto/task_service.pb.h", so we need proto/proto/ structure
RUN mkdir -p ./task-wrapper/proto/proto/ && \
    echo "Copying proto files from proto/proto/..." && \
    cp ./proto/proto/*.pb.cc ./task-wrapper/proto/proto/ && \
    cp ./proto/proto/*.pb.h ./task-wrapper/proto/proto/ && \
    cp ./proto/proto/*.grpc.pb.cc ./task-wrapper/proto/proto/ && \
    cp ./proto/proto/*.grpc.pb.h ./task-wrapper/proto/proto/ && \
    echo "Files copied to task-wrapper/proto/proto/:" && \
    ls -la ./task-wrapper/proto/proto/




#WORKDIR /app/task-wrapper

# Configure and build
#RUN cmake -S . -B build
#RUN cmake --build build

#ENV TASK_NAME=sum
#ENV TASK_QUEUE_NAME=sum

#ENTRYPOINT ["./build/task-wrapper"]
