"""Shared constants. These must match the ESP32 firmware and Ed's interface spec."""

DEVICE_NAME = "Group3Sensor"

# Custom service + five characteristics (Ed's spec)
SERVICE_UUID = "1d390000-dcea-4626-9762-d6dc5fedaa02"
CONTROL_UUID = "1d390001-dcea-4626-9762-d6dc5fedaa02"    # app WRITES commands
STATUS_UUID = "1d390002-dcea-4626-9762-d6dc5fedaa02"     # app subscribes
EVENT_UUID = "1d390003-dcea-4626-9762-d6dc5fedaa02"      # app subscribes
TELEMETRY_UUID = "1d390004-dcea-4626-9762-d6dc5fedaa02"  # app subscribes
RAW_STREAM_UUID = "1d390005-dcea-4626-9762-d6dc5fedaa02" # subscribe ONLY while collecting raw data

# Standard Battery Service (0x180F) / Battery Level characteristic (0x2A19): one byte, 0-100
BATTERY_SERVICE_UUID = "0000180f-0000-1000-8000-00805f9b34fb"
BATTERY_LEVEL_UUID = "00002a19-0000-1000-8000-00805f9b34fb"

# Reconnect policy (architecture doc, Section 3: max 5 attempts, 2 s delay)
MAX_RECONNECT_ATTEMPTS = 5
RECONNECT_DELAY_S = 2.0

# Spec section 6: request MTU 185 or higher so every packet fits in one notification.
# (bleak cannot force an MTU; the OS negotiates it. We record it and the app can warn.)
REQUIRED_MTU = 185
