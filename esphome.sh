#!/usr/bin/env bash
#VERS=2025.5.2
VERS=2026.8.2
TTY_FLAG="-it"
if [ "$1" = "--no-tty" ]; then
  TTY_FLAG=""
  shift
fi
set -x
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
  $TTY_FLAG \
  ghcr.io/esphome/esphome:$VERS \
  $*
