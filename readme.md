### These are configs for my personal esphome devices.

#### Most interesting are:
- `pet-feeder` for tuya smart pet feeder
- `acmelec-thermostat` for tuya thermostat
- `bk7231t-2gang-dimmer` as an example of [libretiny](https://docs.libretiny.eu/) 
- `esp-ups` for [ninebot battery](https://github.com/alexbilevskiy/esphome-custom/tree/master/ninebattery)
- `esp32-flipper-board` for flipper zero devboard

Many devices are not single-purpose, for example:
- pet feeder has additional comfy pwm led for cats :3
- d1-co2 uses both senseair s8 and cm1106
- esp camera has ws2812 led
- esp32-ble-proxy has 433mhz remote receiver
- 2gang-dimmer is used also as ir remote
- etc