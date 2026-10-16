#!/bin/sh
# Build EBOOT.PBP with the pspdev Docker image. Usage: ./build.sh [relay-url]
set -e
cd "$(dirname "$0")"
RELAY="${1:-http://psp-homebrew-relay.YOUR-SUBDOMAIN.workers.dev}"
docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest make RELAY="$RELAY"
echo "Built $(pwd)/EBOOT.PBP"
