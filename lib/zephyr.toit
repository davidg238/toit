// Copyright (C) 2026 Toit contributors.
// Use of this source code is governed by an MIT-style license that can be
// found in the lib/LICENSE file.

/**
Zephyr devices, named by devicetree node label (ztoit).

The drivers are Zephyr's: any Zephyr sensor works through $Sensor, any LED
  strip through $LedStrip, any `gpio-leds` child through $Led and any
  `gpio-keys` child through $Key, and any I2C bus through $I2c. Wiring is the
  board's devicetree overlay.
*/

import monitor
import serial

/**
A Zephyr sensor (`zephyr/drivers/sensor.h`), such as a `bosch,bmp180`.

Call $fetch, then $read each channel of that sample.
*/
class Sensor:
  // Values of Zephyr v4.2's `enum sensor_channel` (zephyr/drivers/sensor.h).
  static DIE-TEMP ::= 12      // SENSOR_CHAN_DIE_TEMP, °C.
  static AMBIENT-TEMP ::= 13  // SENSOR_CHAN_AMBIENT_TEMP, °C.
  static PRESS ::= 14         // SENSOR_CHAN_PRESS, kPa.
  static HUMIDITY ::= 16      // SENSOR_CHAN_HUMIDITY, %.
  static ALTITUDE ::= 22      // SENSOR_CHAN_ALTITUDE, m.

  device_/int

  /** The sensor with devicetree node label $label, for example "bmp180". */
  constructor label/string:
    device_ = zephyr-device-find_ label

  /** Takes a sample of all channels. */
  fetch -> none:
    zephyr-sensor-fetch_ device_

  /** The value of $channel in the last $fetch, in Zephyr's units. */
  read channel/int -> float:
    return zephyr-sensor-read_ device_ channel

/** A Zephyr LED strip (`zephyr/drivers/led_strip.h`), such as a WS2812. */
class LedStrip:
  device_/int

  /** The strip with devicetree node label $label, for example "rgb_led". */
  constructor label/string:
    device_ = zephyr-device-find_ label

  /** Sets the strip's first pixels from $rgb: red, green, blue bytes per pixel. */
  set rgb/ByteArray -> none:
    zephyr-strip-update_ device_ rgb

  /** Sets the first pixel to $r, $g, $b. */
  set-pixel r/int g/int b/int -> none:
    set #[r, g, b]

/**
A Zephyr I2C bus (`zephyr/drivers/i2c.h`), such as "i2c30".
Its targets are $serial.Device objects, so Toit drivers written for
  $serial.Registers work on it.
*/
class I2c:
  device_/int

  /** The bus with devicetree node label $label, for example "i2c30". */
  constructor label/string:
    device_ = zephyr-device-find_ label

  /** The target at 7-bit $address on this bus. */
  device address/int -> I2cDevice:
    if not 0 <= address <= 0x7f: throw "INVALID_ARGUMENT"
    return I2cDevice.private_ device_ address

/** A target on an $I2c bus, with 8-bit register addresses. */
class I2cDevice implements serial.Device:
  bus_/int
  address_/int
  registers_/I2cRegisters_? := null

  constructor.private_ .bus_ .address_:

  registers -> serial.Registers:
    if not registers_: registers_ = I2cRegisters_ this
    return registers_

  /** Reads $amount bytes from the target. */
  read amount/int -> ByteArray:
    return zephyr-i2c-transfer_ bus_ address_ #[] amount

  /** Writes $bytes to the target. */
  write bytes/ByteArray -> none:
    zephyr-i2c-transfer_ bus_ address_ bytes 0

  /** Writes $bytes, then reads $amount bytes after a repeated start. */
  write-read bytes/ByteArray amount/int -> ByteArray:
    return zephyr-i2c-transfer_ bus_ address_ bytes amount

class I2cRegisters_ extends serial.Registers:
  device_/I2cDevice

  constructor .device_:

  read-bytes register/int count/int -> ByteArray:
    return device_.write-read #[register] count

  write-bytes register/int data/ByteArray -> none:
    device_.write #[register] + data

/** A `gpio-leds` child: a digital output, such as an LED. */
class Led:
  led_/int

  /** The output with devicetree node label $label, for example "do0". */
  constructor label/string:
    led_ = zephyr-led-find_ label

  on -> none: zephyr-led-set_ led_ true
  off -> none: zephyr-led-set_ led_ false
  set value/bool -> none: zephyr-led-set_ led_ value

/**
A `gpio-keys` child: a button or switch, debounced by Zephyr.

Changes arrive as events from Zephyr's input subsystem; no polling.
*/
class Key:
  static group_ ::= zephyr-key-init_

  key_/int
  state_/monitor.ResourceState_

  /** The key with devicetree node label $label, for example "user_button1". */
  constructor label/string:
    key_ = zephyr-key-find_ label
    state_ = monitor.ResourceState_ group_ (zephyr-key-open_ group_ key_)

  /** Whether the key is pressed now. */
  pressed -> bool:
    return (zephyr-key-state_ key_) & 1 == 1

  /** How many times the key has been pressed since boot. */
  presses -> int:
    return (zephyr-key-state_ key_) >> 1

  /** Waits for the next press; a press shorter than this task's wakeup still counts. */
  wait-for-press -> none:
    start := presses
    while true:
      state_.clear-state 1
      if presses != start: return
      state_.wait-for-state 1

  /** Waits until the key is up. */
  wait-for-release -> none:
    while true:
      state_.clear-state 1
      if not pressed: return
      state_.wait-for-state 1

  close -> none:
    state_.dispose

/**
A PDM microphone, for example the one at devicetree node label "pdm20".
It records 16 kHz, 16-bit, mono audio in blocks of 100 ms.
*/
class Microphone:
  static RATE ::= 16000
  // The driver stops by itself when its blocks run out because Toit fell behind,
  // and then no block comes. No block for this long means that.
  static STALLED-MS_ ::= 1000

  device_/int
  gain-db_/int

  /**
  The microphone with devicetree node label $label.
  $gain-db, from -20 to 20, is added to the hardware's default gain.
  */
  constructor label/string --gain-db/int=0:
    if not -20 <= gain-db <= 20: throw "OUT_OF_RANGE"
    device_ = zephyr-device-find_ label
    gain-db_ = gain-db

  /** Starts recording. Waits while a previous $stop is still finishing. */
  start -> none:
    while not zephyr-mic-start_ device_ gain-db_: sleep --ms=10

  /**
  The next 100 ms block of little-endian int16 samples.
  Throws "MICROPHONE_STOPPED" if no block comes within a second.
  */
  read -> ByteArray:
    deadline := Time.monotonic-us + STALLED-MS_ * 1000
    while true:
      block := zephyr-mic-read_ device_
      if block: return block
      if Time.monotonic-us > deadline: throw "MICROPHONE_STOPPED"
      sleep --ms=10

  /** Stops recording; blocks not yet read are dropped. */
  stop -> none:
    zephyr-mic-stop_ device_

zephyr-device-find_ label/string -> int:
  #primitive.zephyr.device-find

zephyr-sensor-fetch_ device/int -> none:
  #primitive.zephyr.sensor-fetch

zephyr-sensor-read_ device/int channel/int -> float:
  #primitive.zephyr.sensor-read

zephyr-strip-update_ device/int rgb/ByteArray -> none:
  #primitive.zephyr.strip-update

zephyr-led-find_ label/string -> int:
  #primitive.zephyr.led-find

zephyr-led-set_ led/int on/bool -> none:
  #primitive.zephyr.led-set

zephyr-key-find_ label/string -> int:
  #primitive.zephyr.key-find

zephyr-key-init_:
  #primitive.zephyr.key-init

zephyr-key-open_ group key/int:
  #primitive.zephyr.key-open

zephyr-key-state_ key/int -> int:
  #primitive.zephyr.key-state

zephyr-i2c-transfer_ device/int address/int write/ByteArray read-count/int -> ByteArray?:
  #primitive.zephyr.i2c-transfer

zephyr-mic-start_ device/int gain-db/int -> bool:
  #primitive.zephyr.mic-start

zephyr-mic-read_ device/int -> ByteArray?:
  #primitive.zephyr.mic-read

zephyr-mic-stop_ device/int -> none:
  #primitive.zephyr.mic-stop
