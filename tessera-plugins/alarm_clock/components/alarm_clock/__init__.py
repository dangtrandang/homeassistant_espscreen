"""Local-first Alarm Clock plugin for Tessera (Stage 1: Engine + NVS + Local WAV)."""
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display, speaker, time
from esphome.const import CONF_ID, CONF_TIME_ID

DEPENDENCIES = ["smart_display", "time"]

CONF_SPEAKER = "speaker"

SOUNDS = Path(__file__).parent / "sounds"


def check_sounds(config):
    smart_display.read_sound(SOUNDS / "alarm.wav")
    return config


alarm_clock_ns = cg.esphome_ns.namespace("alarm_clock")
AlarmClock = alarm_clock_ns.class_("AlarmClock", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AlarmClock),
            cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    check_sounds,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    cg.add(var.set_alarm_sound(smart_display.sound(config[CONF_ID], "alarm", SOUNDS / "alarm.wav")))
