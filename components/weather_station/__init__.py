import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID

weather_station_ns = cg.esphome_ns.namespace("weather_station")
WeatherStation = weather_station_ns.class_("WeatherStation", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WeatherStation),
        cv.Required("panel_width"): cv.int_,
        cv.Required("panel_height"): cv.int_,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_panel_size(config["panel_width"], config["panel_height"]))
