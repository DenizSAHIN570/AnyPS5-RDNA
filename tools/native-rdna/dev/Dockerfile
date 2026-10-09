FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=20

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ cmake ninja-build pkg-config git ca-certificates curl gnupg \
        python3 python3-venv \
        libdrm-dev libvulkan-dev vulkan-tools mesa-vulkan-drivers glslang-tools \
        libhsa-runtime-dev \
        libx11-dev libxext-dev \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
    && rm -rf /var/lib/apt/lists/*

RUN for tool in clang ld.lld llvm-mc llvm-objdump llvm-readelf; do \
        ln -sf "$(command -v ${tool}-${LLVM_VERSION})" "/usr/local/bin/${tool}"; \
    done

WORKDIR /src
