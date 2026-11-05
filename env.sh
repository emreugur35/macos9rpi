# Source this file: build environment for macos9rpi
export MACOS9RPI="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export PATH="$MACOS9RPI/tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin:/opt/homebrew/opt/gnu-getopt/bin:/opt/homebrew/opt/make/libexec/gnubin:/opt/homebrew/opt/gnu-sed/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH"
export PATH="/opt/homebrew/opt/texinfo/bin:$PATH"
