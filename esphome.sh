#!/usr/bin/env bash
set -x
# -e ESPHOME_DASHBOARD_USE_PING=true
docker run --name esphome --rm --privileged --net=host -v /etc/localtime:/etc/localtime:ro -v "${PWD}":/config -v "${PWD}/.platformio":/root/.platformio -it ghcr.io/esphome/esphome $*