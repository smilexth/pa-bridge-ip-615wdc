# Build environment. Match the tag to the Debian release of the machine that will
# RUN the binary, not the one you are building on: glibc is backward-compatible,
# not forward-compatible, so a binary built on Debian 13 will not start on 12.
#
#   docker build -t pa-bridge-build .
#   docker run --rm -v "$PWD:/src" -w /src/src pa-bridge-build make
#
# The vendor SDK must already be in vendor/sdk/Include/ — see README.
FROM debian:12
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends build-essential file \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
