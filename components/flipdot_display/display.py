"""Flipdot display: a monochrome display built from daisy-chained TM1824 PWM
drivers. One component with two interchangeable transports selected by
`connection`: the single-wire TM1824 chain (RMT) or DMX512 over RS-485 (UART
plus an external transceiver).

The chain is 48 TM1824 chips x 32 bits (four 8-bit duty bytes each) = one duty
byte per dot. Within a block the pixels are wired as a mirrored Z: every row is
scanned right-to-left, pixel 0 is the top-right corner of the block and the
last pixel is the bottom-left corner. Blocks are chained as a snake: block row
0 right-to-left, row 1 left-to-right, alternating per row, top-to-bottom
across block rows.
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import display, esp32, esp32_rmt
from esphome.components.esp32 import include_builtin_idf_component
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INVERTED,
    CONF_LAMBDA,
    CONF_NUMBER,
    CONF_PAGES,
    CONF_PIN,
    CONF_RMT_SYMBOLS,
)
from esphome.types import ConfigType

DEPENDENCIES = ["esp32"]

CONF_BLOCK_WIDTH = "block_width"
CONF_BLOCK_HEIGHT = "block_height"
CONF_COLS = "cols"
CONF_CONNECTION = "connection"
CONF_CONCURRENCY = "concurrency"
CONF_DE_PIN = "de_pin"
CONF_DMX_TRANSPORT_ID = "dmx_transport_id"
CONF_EOT_LEVEL = "eot_level"
CONF_MODULE_ORDER = "module_order"
CONF_OFF_LEVEL = "off_level"
CONF_ON_LEVEL = "on_level"
CONF_ROWS = "rows"
CONF_RMT_TRANSPORT_ID = "rmt_transport_id"
CONF_STEP_INTERVAL = "step_interval"
CONF_SWITCHING_EFFECT = "switching_effect"

flipdot_display_ns = cg.esphome_ns.namespace("flipdot_display")
FlipdotDisplay = flipdot_display_ns.class_(
    "FlipdotDisplay", display.DisplayBuffer, cg.PollingComponent
)
FlipdotDmxTransport = flipdot_display_ns.class_("FlipdotDmxTransport")
FlipdotRmtTransport = flipdot_display_ns.class_("FlipdotRmtTransport")
# enum class SwitchingEffect, defined in the component header; the cv.enum
# mapping values must codegen as the C++ constants, not bare ints.
SwitchingEffect = flipdot_display_ns.enum("SwitchingEffect", is_class=True)
ConnectionType = flipdot_display_ns.enum("ConnectionType", is_class=True)

# Order modes for dot transitions (config: switching_effect).
SWITCHING_EFFECTS = {
    "none": SwitchingEffect.NONE,
    "wave": SwitchingEffect.WAVE,
    "random": SwitchingEffect.RANDOM,
}

CONNECTION_SINGLE_WIRE = "single_wire"
CONNECTION_DMX = "dmx"


def _validate_connection(config: ConfigType) -> ConfigType:
    """Transport-specific keys are only valid on their transport. The RMT
    bench knobs (eot_level, rmt_symbols) are tolerated-but-ignored in DMX
    mode: their defaults are always present after validation, so an explicit
    set cannot be told apart from the default."""
    if config[CONF_CONNECTION] == CONNECTION_DMX:
        if CONF_DE_PIN not in config:
            raise cv.Invalid("de_pin is required with connection: dmx")
    else:
        if CONF_DE_PIN in config:
            raise cv.Invalid("de_pin is only valid with connection: dmx")
        if CONF_MODULE_ORDER in config:
            raise cv.Invalid("module_order is only valid with connection: dmx")
    return config


def _validate_rmt_variant(config: ConfigType) -> ConfigType:
    """Only the single-wire transport needs an RMT peripheral; DMX runs on a
    UART and works on every ESP32 variant."""
    if config[CONF_CONNECTION] == CONNECTION_SINGLE_WIRE:
        return esp32.only_on_variant(
            unsupported=list(esp32_rmt.VARIANTS_NO_RMT),
            msg_prefix="Flipdot display single-wire connection",
        )(config)
    return config


def _validate_step_interval(config: ConfigType) -> ConfigType:
    if (
        config[CONF_CONNECTION] == CONNECTION_DMX
        and config[CONF_CONCURRENCY] > 0
        and config[CONF_STEP_INTERVAL].total_milliseconds < 25
    ):
        raise cv.Invalid(
            "step_interval must be at least 25ms in DMX mode when concurrency > 0 "
            "(one DMX frame is ~23ms at 250 kbaud)"
        )
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(FlipdotDisplay),
            cv.GenerateID(CONF_DMX_TRANSPORT_ID): cv.declare_id(FlipdotDmxTransport),
            cv.GenerateID(CONF_RMT_TRANSPORT_ID): cv.declare_id(FlipdotRmtTransport),
            # Exactly one transport, selected by the connection type:
            # single_wire drives the TM1824 chain from an RMT channel on `pin`,
            # dmx drives the RS-485 bus (UART TX on `pin`, transceiver DE on
            # `de_pin`).
            cv.Required(CONF_CONNECTION): cv.one_of(CONNECTION_SINGLE_WIRE, CONNECTION_DMX, lower=True),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_DE_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_COLS): cv.positive_int,
            cv.Required(CONF_ROWS): cv.positive_int,
            cv.Optional(CONF_BLOCK_WIDTH, default=8): cv.int_range(min=1, max=128),
            cv.Optional(CONF_BLOCK_HEIGHT, default=8): cv.int_range(min=1, max=128),
            cv.Optional(CONF_ON_LEVEL, default=255): cv.int_range(min=0, max=255),
            # Dot-transition throttling (see "Throttled switching" in the doc):
            # with concurrency > 0 the frame diff is sent in batches of at most
            # `concurrency` dots, one batch per step_interval; 0 = all dots of a
            # transition switch in one frame (the classic behavior).
            cv.Optional(CONF_CONCURRENCY, default=0): cv.int_range(min=0),
            cv.Optional(CONF_STEP_INTERVAL, default="100ms"): cv.positive_time_period_milliseconds,
            # Order of dots within a transition.
            cv.Optional(CONF_SWITCHING_EFFECT, default="none"): cv.enum(SWITCHING_EFFECTS),
            cv.Optional(CONF_OFF_LEVEL, default=0): cv.int_range(min=0, max=255),
            # DMX mode only: factory position (1..8, the vendor's per-line
            # limit) of every module, in chain order (the top-right block of
            # row 0 first; the chain snakes across block rows).
            # Module n gets the universe window (n-1)*block_pixels+1 ..
            # n*block_pixels (the factory layout). Absent -> positions 1..N.
            cv.Optional(CONF_MODULE_ORDER): cv.ensure_list(cv.int_range(min=1, max=8)),
            # Single-wire mode only: wire level after the transmitted frame.
            cv.Optional(CONF_EOT_LEVEL, default=0): cv.int_range(min=0, max=1),
            cv.SplitDefault(
                CONF_RMT_SYMBOLS,
                esp32=192,
                esp32_c3=96,
                esp32_c5=96,
                esp32_c6=96,
                esp32_h2=96,
                esp32_p4=192,
                esp32_s2=192,
                esp32_s3=192,
            ): cv.int_range(min=2),
        }
    ),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
    _validate_connection,
    _validate_rmt_variant,
    _validate_step_interval,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    connection = config[CONF_CONNECTION]

    if connection == CONNECTION_SINGLE_WIRE:
        # Single-wire transport: the RMT channel drives DIN.
        # Re-enable ESP-IDF's RMT driver (excluded by default to save compile time)
        include_builtin_idf_component("esp_driver_rmt")

        transport = cg.new_Pvariable(config[CONF_RMT_TRANSPORT_ID])
        cg.add(transport.set_pin(config[CONF_PIN][CONF_NUMBER]))
        if config[CONF_PIN][CONF_INVERTED]:
            cg.add(transport.set_inverted(True))
        pixels = (
            config[CONF_COLS] * config[CONF_ROWS] * config[CONF_BLOCK_WIDTH] * config[CONF_BLOCK_HEIGHT]
        )
        cg.add(transport.set_frame_size(pixels))
        cg.add(transport.set_eot_level(config[CONF_EOT_LEVEL]))
        cg.add(transport.set_rmt_symbols(config[CONF_RMT_SYMBOLS]))
        cg.add(var.set_connection(ConnectionType.SINGLE_WIRE))
    else:
        # DMX transport: the UART drives the RS-485 transceiver.
        transport = cg.new_Pvariable(config[CONF_DMX_TRANSPORT_ID])
        cg.add(transport.set_pin(config[CONF_PIN][CONF_NUMBER]))
        cg.add(transport.set_de_pin(config[CONF_DE_PIN][CONF_NUMBER]))
        cg.add(var.set_connection(ConnectionType.DMX))
    cg.add(var.set_transport(transport))

    cg.add(
        var.set_grid(
            config[CONF_COLS],
            config[CONF_ROWS],
            config[CONF_BLOCK_WIDTH],
            config[CONF_BLOCK_HEIGHT],
        )
    )
    if connection == CONNECTION_DMX:
        blocks = config[CONF_COLS] * config[CONF_ROWS]
        block_pixels = config[CONF_BLOCK_WIDTH] * config[CONF_BLOCK_HEIGHT]
        positions = config.get(CONF_MODULE_ORDER)
        if positions is None:
            # Factory default: modules in kit order.
            positions = list(range(1, blocks + 1))
        if len(positions) != blocks:
            raise cv.Invalid(f"module_order must list exactly {blocks} entries (cols * rows)")
        if len(set(positions)) != len(positions):
            raise cv.Invalid(f"module_order lists the same module twice: {positions}")
        addresses = [(pos - 1) * block_pixels + 1 for pos in positions]
        for pos, addr in zip(positions, addresses):
            if addr + block_pixels - 1 > 512:
                raise cv.Invalid(
                    f"module {pos} does not fit the universe (window {addr}..{addr + block_pixels - 1})"
                )
        cg.add(var.set_module_addresses(addresses))
    cg.add(var.set_on_level(config[CONF_ON_LEVEL]))
    cg.add(var.set_off_level(config[CONF_OFF_LEVEL]))
    cg.add(var.set_concurrency(config[CONF_CONCURRENCY]))
    cg.add(var.set_step_interval(config[CONF_STEP_INTERVAL].total_milliseconds))
    cg.add(var.set_switching_effect(config[CONF_SWITCHING_EFFECT]))
    await display.register_display(var, config)

    if lambda_config := config.get(CONF_LAMBDA):
        lambda_ = await cg.process_lambda(
            lambda_config, [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
