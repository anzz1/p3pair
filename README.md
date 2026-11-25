# p3pair
Automatically pair Bluetooth HID gamepads on Windows, and keep them paired without any user interaction.

While the target device is not connected, runs a Bluetooth inquiry. When the
device shows up (i.e. it is in pairing mode), any stale pairing is removed,
the device is paired (PIN / confirmation answered automatically) and its HID
service is enabled, so Windows installs the driver silently instead of
showing the "device is trying to connect" balloon and the hardware wizard.

### Usage
```
p3pair <address> [-pin PIN] [-scan N]
  address   device address, e.g. 41:42:F6:48:76:C8
  -pin PIN  PIN for legacy pairing (default 0000)
  -scan N   inquiry length in 1.28 s units, 1..48 (default 4)
```

Windows Vista SP2 / 7 or newer, Microsoft Bluetooth stack.
