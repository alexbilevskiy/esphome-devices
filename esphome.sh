#!/usr/bin/env bash
set -x

docker run --privileged -v "${PWD}":/config -v "${PWD}/.platformio":/root/.platformio -it ghcr.io/esphome/esphome $*