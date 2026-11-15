"""BLE client for the Wellue O2Ring pulse oximeter.

GATT surface, the AA-framed packet format and the CRC8 were reverse-engineered
against the physical ring -- see this component's README for the framing and
for the device constraints that shape everything here (reachability tied to
being worn or charging, storage eviction on wear-start, wrong published .vld
offsets). No bonding, no encryption, no secret: this is a plain unauthenticated
GATT peer.

Walks the ring's whole FileList (oldest first) once connected -- see
viatom_o2ring.h's State enum for the state machine. Two ways a sync starts:

- Manual (always available): the `start_sync()` method, typically called
  from an HA button -- see start_sync() in the .cpp.
- Automatic (`auto_sync: true`, off by default): the component's own
  advertisement listener connects ONCE when the ring appears after being off
  air continuously for `off_air_threshold` (default 2h), syncs, disconnects,
  then stays silent however long the ring keeps advertising -- see
  parse_device()/maybe_start_auto_sync_() in the .cpp. Every connect costs
  the ring's own battery, which is why the default is manual only.

Either way, a failed connect is retried by maybe_retry_connect_() (raw BLE
flakiness, several attempts within one sync), and automatic sync also
retries the whole sync once before giving up until the ring's next
appearance (on_sync_campaign_done_()).
"""

import esphome.codegen as cg
from esphome.components import ble_client, ble_device_base, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_STATE,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

DEPENDENCIES = ["ble_client"]
# json::parse_json() (used to decode the INFO reply) needs USE_JSON defined
# and ArduinoJson pulled in, which only happens if the json component's own
# to_code() runs -- nothing else on this board configures a `json:` block to
# trigger that, so it has to be auto-loaded here. sensor and text_sensor
# likewise: this component's own entities need their headers built even on a
# board that configures no other sensor.
AUTO_LOAD = ["json", "sensor", "text_sensor"]

CONF_SERIAL_NUMBER = "serial_number"
CONF_FILE_LIST = "file_list"
CONF_LAST_SYNC = "last_sync"
CONF_AUTO_SYNC = "auto_sync"
CONF_OFF_AIR_THRESHOLD = "off_air_threshold"

viatom_o2ring_ns = cg.esphome_ns.namespace("viatom_o2ring")
ViatomO2Ring = viatom_o2ring_ns.class_(
    "ViatomO2Ring",
    cg.Component,
    ble_client.BLEClientNode,
    ble_device_base.ESPBTDeviceListener,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ViatomO2Ring),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SERIAL_NUMBER): text_sensor.text_sensor_schema(
                icon="mdi:identifier",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Raw CurState ("0"/"2" etc) rather than a decoded string -- only
            # the transport is verified, not what every value means.
            cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(
                icon="mdi:sleep",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FILE_LIST): text_sensor.text_sensor_schema(
                icon="mdi:file-document-multiple-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # The ring's own CurTIME from the last walk that finished with no
            # failed transfer -- device-side half of telling "nobody wore the
            # ring" (nothing new, walk still finishes clean) apart from "we
            # couldn't capture it" (this stops advancing). See
            # start_next_download_() in the .cpp.
            cv.Optional(CONF_LAST_SYNC): text_sensor.text_sensor_schema(
                icon="mdi:clock-check-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Off by default: manual start_sync() only, so the component never
            # connects to the ring on its own.
            cv.Optional(CONF_AUTO_SYNC, default=False): cv.boolean,
            cv.Optional(
                CONF_OFF_AIR_THRESHOLD, default="2h"
            ): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(minutes=1), max=cv.TimePeriod(hours=24)),
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    # Also a BLE_DEVICE, not just a BLE client: automatic sync listens for
    # the ring's advertisement (to detect the off-air -> on-air edge) as well
    # as talking GATT to it once connected. register_ble_device() needs
    # ble_hub_id from this schema.
    .extend(ble_device_base.BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Advertisement listener as well as GATT client -- see the docstring above.
    await ble_device_base.register_ble_device(var, config)
    cg.add(var.set_auto_sync(config[CONF_AUTO_SYNC]))
    cg.add(var.set_off_air_threshold(config[CONF_OFF_AIR_THRESHOLD]))

    if battery_config := config.get(CONF_BATTERY_LEVEL):
        sens = await sensor.new_sensor(battery_config)
        cg.add(var.set_battery_level_sensor(sens))
    if sn_config := config.get(CONF_SERIAL_NUMBER):
        ts = await text_sensor.new_text_sensor(sn_config)
        cg.add(var.set_serial_number_text_sensor(ts))
    if state_config := config.get(CONF_STATE):
        ts = await text_sensor.new_text_sensor(state_config)
        cg.add(var.set_state_text_sensor(ts))
    if file_list_config := config.get(CONF_FILE_LIST):
        ts = await text_sensor.new_text_sensor(file_list_config)
        cg.add(var.set_file_list_text_sensor(ts))
    if last_sync_config := config.get(CONF_LAST_SYNC):
        ts = await text_sensor.new_text_sensor(last_sync_config)
        cg.add(var.set_last_sync_text_sensor(ts))
