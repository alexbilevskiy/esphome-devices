#!/usr/bin/env bash
set -x

docker run --privileged --net=host -v /etc/localtime:/etc/localtime:ro -v "${PWD}":/config -v "${PWD}/.platformio":/root/.platformio -it ghcr.io/esphome/esphome $*