#!/bin/sh
# Everything for the muOS device. build and lint run in the arm64 Debian container
# (docker/Dockerfile), natively on arm64 hosts (Apple Silicon, arm64 Linux); elsewhere Docker
# emulates arm64 (slow).
#
#   scripts/device.sh build [--clean]    # the package: dist/Sendspin, dist/Sendspin.zip
#   scripts/device.sh lint               # scripts/lint.sh on the device build, pinned tools
#   scripts/device.sh deploy [user@host] # install dist/Sendspin over SSH (default root@muos.local)
set -eu
cd "$(dirname "$0")/.."

IMAGE=muos-sendspin-build
CONFIGURE='cmake -S . -B build-device -G Ninja -DMUOS_DEVICE=ON -DCMAKE_BUILD_TYPE=Release'

in_container() {
    # Always build the image: the layer cache makes it instant unless docker/Dockerfile changed.
    docker build --quiet --platform linux/arm64 -t "$IMAGE" docker/ >/dev/null
    # As the calling user, so build-device/ and dist/ aren't owned by root on Linux hosts.
    docker run --rm --platform linux/arm64 --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$PWD":/src -w /src "$IMAGE" sh -euc "$1"
}

build() {
    [ "${1:-}" = "--clean" ] && rm -rf build-device
    in_container "$CONFIGURE && cmake --build build-device --target package-muos"
    ls -la dist/Sendspin dist/Sendspin.zip
}

lint() {
    in_container "$CONFIGURE >/dev/null && BUILD_DIR=build-device scripts/lint.sh"
}

# Replaces the app's own files but never its state (state.json, settings.ini, the logs). Stops the
# app if it's running: muOS then returns to Applications, where Sendspin starts again.
deploy() {
    device=${1:-root@muos.local}
    app=/run/muos/storage/application/Sendspin  # muOS binds SD1's or SD2's application folder here
    stage=/tmp/sendspin-deploy
    if [ ! -f dist/Sendspin/muos-sendspin ]; then
        echo "no dist/Sendspin: run make device-build first" >&2
        exit 1
    fi
    # COPYFILE_DISABLE: no AppleDouble ._ files from macOS tar.
    # shellcheck disable=SC2029  # $stage and $app expand here on purpose; \$ marks the device side
    installed=$(COPYFILE_DISABLE=1 tar -C dist -cf - Sendspin | ssh "$device" "set -e
        rm -rf $stage && mkdir $stage && tar -xf - -C $stage
        if pid=\$(pidof muos-sendspin); then
            kill -TERM \$pid
            for _ in 1 2 3 4 5 6 7 8 9 10; do pidof muos-sendspin >/dev/null || break; sleep 1; done
            if pidof muos-sendspin >/dev/null; then echo 'the app did not stop' >&2; exit 1; fi
        fi
        mkdir -p $app && cp -R $stage/Sendspin/. $app/ && sync && rm -rf $stage
        cksum < $app/muos-sendspin")
    if [ "$installed" = "$(cksum < dist/Sendspin/muos-sendspin)" ]; then
        echo "installed on $device: start Sendspin from Applications"
    else
        echo "the binary on $device doesn't match dist/Sendspin/muos-sendspin" >&2
        exit 1
    fi
}

usage() {
    echo "usage: scripts/device.sh build [--clean] | lint | deploy [user@host]" >&2
    exit 2
}

case "${1:-} $#" in
    "build 1" | "build 2") [ "${2:---clean}" = --clean ] || usage ;;
    "lint 1" | "deploy 1" | "deploy 2") ;;
    *) usage ;;
esac
command=$1
shift
"$command" "$@"
