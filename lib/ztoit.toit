// Copyright (C) 2026 Toit contributors.
// Use of this source code is governed by an MIT-style license that can be
// found in the lib/LICENSE file.

/**
Zephyr devices, named by devicetree node label (ztoit).

The drivers are Zephyr's: any Zephyr sensor works through $Sensor, any LED
  strip through $LedStrip, any `gpio-leds` child through $Led and any
  `gpio-keys` child through $Key, any I2C bus through $I2c, and any UART's
  receive side through $Uart. Wiring is the board's devicetree overlay.
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
    device_ = ztoit-device-find_ label

  /** Takes a sample of all channels. */
  fetch -> none:
    ztoit-sensor-fetch_ device_

  /** The value of $channel in the last $fetch, in Zephyr's units. */
  read channel/int -> float:
    return ztoit-sensor-read_ device_ channel

/** A Zephyr LED strip (`zephyr/drivers/led_strip.h`), such as a WS2812. */
class LedStrip:
  device_/int

  /** The strip with devicetree node label $label, for example "rgb_led". */
  constructor label/string:
    device_ = ztoit-device-find_ label

  /** Sets the strip's first pixels from $rgb: red, green, blue bytes per pixel. */
  set rgb/ByteArray -> none:
    ztoit-strip-update_ device_ rgb

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
    device_ = ztoit-device-find_ label

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
    return ztoit-i2c-transfer_ bus_ address_ #[] amount

  /** Writes $bytes to the target. */
  write bytes/ByteArray -> none:
    ztoit-i2c-transfer_ bus_ address_ bytes 0

  /** Writes $bytes, then reads $amount bytes after a repeated start. */
  write-read bytes/ByteArray amount/int -> ByteArray:
    return ztoit-i2c-transfer_ bus_ address_ bytes amount

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
    led_ = ztoit-led-find_ label

  on -> none: ztoit-led-set_ led_ true
  off -> none: ztoit-led-set_ led_ false
  set value/bool -> none: ztoit-led-set_ led_ value

/**
A `gpio-keys` child: a button or switch, debounced by Zephyr.

Changes arrive as events from Zephyr's input subsystem; no polling.
*/
class Key:
  static group_ ::= ztoit-key-init_

  key_/int
  state_/monitor.ResourceState_

  /** The key with devicetree node label $label, for example "user_button1". */
  constructor label/string:
    key_ = ztoit-key-find_ label
    state_ = monitor.ResourceState_ group_ (ztoit-key-open_ group_ key_)

  /** Whether the key is pressed now. */
  pressed -> bool:
    return (ztoit-key-state_ key_) & 1 == 1

  /** How many times the key has been pressed since boot. */
  presses -> int:
    return (ztoit-key-state_ key_) >> 1

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
An LC3 encoder: 16 kHz, 16-bit, mono audio in 10 ms frames, as the $Microphone records it.
There is one encoder: constructing a new one takes it over, and an older one's $encode
  then throws ALREADY_CLOSED.
*/
class Lc3Encoder:
  static RATE ::= 16000
  static FRAME-US ::= 10_000
  /** The PCM bytes of one frame: 160 samples of little-endian int16. */
  static FRAME-PCM-BYTES ::= 320

  frame-bytes/int
  generation_/int

  /** An encoder at $bitrate bits per second, 16000 to 320000 (32 kbit/s gives 40-byte frames). */
  constructor --bitrate/int=32_000:
    setup := ztoit-lc3-setup_ bitrate
    generation_ = setup >> 9
    frame-bytes = setup & 511

  /**
  Encodes $pcm, whole frames of little-endian int16 samples, and returns the encoded frames
    back to back, $frame-bytes each.
  */
  encode pcm/ByteArray -> ByteArray:
    return ztoit-lc3-encode_ pcm generation_

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
    device_ = ztoit-device-find_ label
    gain-db_ = gain-db

  /**
  Starts recording. Takes the microphone over: a recording already running, here
    or in another process, is stopped first.
  Throws "MICROPHONE_BUSY" if the driver isn't ready within a second.
  */
  start -> none:
    deadline := Time.monotonic-us + STALLED-MS_ * 1000
    while not ztoit-mic-start_ device_ gain-db_:
      if Time.monotonic-us > deadline: throw "MICROPHONE_BUSY"
      sleep --ms=10

  /**
  The next 100 ms block of little-endian int16 samples.
  Throws "MICROPHONE_STOPPED" if no block comes within a second.
  */
  read -> ByteArray:
    deadline := Time.monotonic-us + STALLED-MS_ * 1000
    while true:
      block := ztoit-mic-read_ device_
      if block: return block
      if Time.monotonic-us > deadline: throw "MICROPHONE_STOPPED"
      sleep --ms=10

  /** Stops recording; blocks not yet read are dropped. */
  stop -> none:
    ztoit-mic-stop_ device_

/**
A UART that only receives, for example a sensor's serial line at devicetree
  node label "uart2". Wiring (the RX pin) is the board's devicetree overlay.
One UART receives at a time: opening one closes any other.
*/
class Uart:
  device_/int
  closed_/bool := false

  /** Receives at $baud, 8N1, on the UART with devicetree node label $label. */
  constructor label/string --baud/int:
    device_ = ztoit-device-find_ label
    ztoit-uart-open_ device_ baud

  /**
  The bytes received since the last read; waits for at least one.
  Throws "ALREADY_CLOSED" after $close, or once another UART is opened.
  */
  read -> ByteArray:
    while true:
      bytes := ztoit-uart-read_ device_
      if bytes: return bytes
      sleep --ms=20

  /** Stops receiving. Returns how many bytes were dropped because Toit read too slowly. */
  close -> int:
    if closed_: return 0
    closed_ = true
    return ztoit-uart-close_ device_

/**
The firmware's speech synthesizer, through its amplifier and speaker.
A phrase is bytes in the synthesizer's own format (ktau: 4 bytes a phone).
*/
class Speech:
  /**
  Starts speaking $phrase, after any phrase still being spoken, and returns.
  Throws "INVALID_ARGUMENT" for a phrase the synthesizer cannot speak, and
    "UNSUPPORTED" on a firmware without a synthesizer.
  */
  static say phrase/ByteArray -> none:
    while not ztoit-speech-say_ phrase: sleep --ms=20

  /** Whether a phrase is being spoken. */
  static speaking -> bool:
    return ztoit-speech-busy_

  /** Waits until nothing is being spoken. */
  static wait -> none:
    while speaking: sleep --ms=20

/**
Posts $value as the observation $name, for example "vin/pm25": the firmware's
  agent reports it as the cell `obs/<name>` at its next check-in. The last post
  wins. Posting does not open the network: the agent checks in when the app
  opens it.
Throws "INVALID_ARGUMENT" for a name the agent cannot keep (empty or too long),
  an error when the agent's table already holds as many other names as it
  can, and "UNSUPPORTED" on a firmware without an agent.
*/
observe name/string value/int -> none:
  ztoit-observe_ name value

ztoit-device-find_ label/string -> int:
  #primitive.ztoit.device-find

ztoit-sensor-fetch_ device/int -> none:
  #primitive.ztoit.sensor-fetch

ztoit-sensor-read_ device/int channel/int -> float:
  #primitive.ztoit.sensor-read

ztoit-strip-update_ device/int rgb/ByteArray -> none:
  #primitive.ztoit.strip-update

ztoit-led-find_ label/string -> int:
  #primitive.ztoit.led-find

ztoit-led-set_ led/int on/bool -> none:
  #primitive.ztoit.led-set

ztoit-key-find_ label/string -> int:
  #primitive.ztoit.key-find

ztoit-key-init_:
  #primitive.ztoit.key-init

ztoit-key-open_ group key/int:
  #primitive.ztoit.key-open

ztoit-key-state_ key/int -> int:
  #primitive.ztoit.key-state

ztoit-i2c-transfer_ device/int address/int write/ByteArray read-count/int -> ByteArray?:
  #primitive.ztoit.i2c-transfer

ztoit-uart-open_ device/int baud/int -> none:
  #primitive.ztoit.uart-open

ztoit-uart-read_ device/int -> ByteArray?:
  #primitive.ztoit.uart-read

ztoit-uart-close_ device/int -> int:
  #primitive.ztoit.uart-close

ztoit-mic-start_ device/int gain-db/int -> bool:
  #primitive.ztoit.mic-start

ztoit-mic-read_ device/int -> ByteArray?:
  #primitive.ztoit.mic-read

ztoit-mic-stop_ device/int -> none:
  #primitive.ztoit.mic-stop

ztoit-lc3-setup_ bitrate/int -> int:
  #primitive.ztoit.lc3-setup

ztoit-lc3-encode_ pcm/ByteArray generation/int -> ByteArray:
  #primitive.ztoit.lc3-encode

ztoit-speech-say_ phrase/ByteArray -> bool:
  #primitive.ztoit.speech-say

ztoit-speech-busy_ -> bool:
  #primitive.ztoit.speech-busy

ztoit-observe_ name/string value/int -> none:
  #primitive.ztoit.observe
