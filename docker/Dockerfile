# Build environment for the muOS device build (aarch64), and its lint. Debian trixie: GCC 14 for
# C++23, and glibc 2.41, which must not exceed the device's 2.44 (a binary needs a glibc at least as
# new as the one it was built against). Runs natively on arm64 hosts: no cross toolchain.
#
#   docker build --platform linux/arm64 -t muos-sendspin-build docker/
FROM --platform=linux/arm64 debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates curl pkg-config pipx shellcheck \
    && rm -rf /var/lib/apt/lists/*

# SDL 2.28.5, matching the device's /usr/lib/libSDL2-2.0.so.0.2800.5 (same SONAME and API; muOS's
# patches change behaviour). We compile and link against it but never ship it: at runtime the
# binary loads the device's own patched copy.
ARG SDL_VERSION=2.28.5
RUN curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz" \
        | tar -xz -C /tmp \
    && cmake -S /tmp/SDL2-${SDL_VERSION} -B /tmp/sdl-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/sdl2 \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    && cmake --build /tmp/sdl-build && cmake --install /tmp/sdl-build \
    && rm -rf /tmp/SDL2-${SDL_VERSION} /tmp/sdl-build

# The lint's clang tools, pinned: their output changes between versions. Bump both together. After
# SDL, so a bump doesn't rebuild it.
RUN PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin pipx install clang-format==23.1.3 \
    && PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin pipx install clang-tidy==23.1.4

WORKDIR /src
