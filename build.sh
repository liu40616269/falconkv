#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/falconkv}"

# Default values
ACTION="build"
TARGET="all"
BUILD_TYPE="Release"
EXTRA_CMAKE_ARGS=""

usage() {
    cat <<EOF
Usage: $0 [action] [target] [options]

Actions:
  build       Build the project (default)
  test        Run tests
  clean       Clean build directory
  install     Install to ${INSTALL_DIR}

Targets:
  all         Build everything (default)
  falconkv    Build FalconKV libraries

Options:
  --debug           Build in Debug mode
  --with-python     Build Python bindings
  --install-dir DIR Install directory (default: ${INSTALL_DIR})
  --verbose         Verbose build output
  -h, --help        Show this help message

Examples:
  $0                                    # Full Release build
  $0 build falconkv --debug             # Debug build
  $0 test                               # Run unit tests
  $0 install                            # Install
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        build|test|clean|install)
            ACTION="$1"
            shift
            ;;
        falconkv|all)
            TARGET="$1"
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --with-python)
            EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS} -DFALCONKV_BUILD_PYTHON=ON"
            shift
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --verbose)
            EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS} -DCMAKE_VERBOSE_MAKEFILE=ON"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

do_clean() {
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
    echo "Done."
}

do_build() {
    echo "Building FalconKV (${BUILD_TYPE})..."
    mkdir -p "${BUILD_DIR}"

    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DFALCONKV_BUILD_TESTS=ON \
        ${EXTRA_CMAKE_ARGS}

    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    echo "Build complete."
}

do_test() {
    echo "Running tests..."
    if [[ ! -d "${BUILD_DIR}" ]]; then
        do_build
    fi
    cd "${BUILD_DIR}"
    ctest --output-on-failure -j"$(nproc)"
    echo "Tests complete."
}

do_install() {
    echo "Installing to ${INSTALL_DIR}..."
    if [[ ! -d "${BUILD_DIR}" ]]; then
        do_build
    fi
    cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
    echo "Install complete."
}

case "${ACTION}" in
    clean)
        do_clean
        ;;
    build)
        do_build
        ;;
    test)
        do_test
        ;;
    install)
        do_install
        ;;
    *)
        echo "Unknown action: ${ACTION}"
        usage
        exit 1
        ;;
esac
