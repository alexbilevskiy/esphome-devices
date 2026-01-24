#!/usr/bin/env bash
set -x
VERS=2026.1.1
# -e ESPHOME_DASHBOARD_USE_PING=true
docker run \
  --name esphome \
  --rm \
  --net=host \
  -e TZ=Europe/Moscow \
  -v /etc/localtime:/etc/localtime:ro \
  -v "${PWD}":/config \
  -v "${PWD}/.platformio":/root/.platformio \
  -v "${PWD}/.cache":/cache \
  -v "${PWD}/.build":/build \
  -it ghcr.io/esphome/esphome:$VERS \
  $*
