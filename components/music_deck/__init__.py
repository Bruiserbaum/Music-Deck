"""Music deck: the ESPHome side. It takes the Music Assistant entry to ask (the input `entry`) and registers the plugin;
register_plugin() reads the id, version, tile memory and the screen's texts from the manifest and translations/ beside
this folder (docs/FIRMWARE_API.md)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

CONF_ENTRY = "entry"

music_deck_ns = cg.esphome_ns.namespace("music_deck")
MusicDeck = music_deck_ns.class_("MusicDeck", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MusicDeck),
        cv.Optional(CONF_ENTRY, default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_entry(config[CONF_ENTRY].strip()))
