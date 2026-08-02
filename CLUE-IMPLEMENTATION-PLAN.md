# CLUE Complete Platform Implementation Plan

## 1. Purpose and Success Criteria

Extend Butterfly's Adafruit CLUE target into a complete, board-gated platform that exposes useful onboard hardware, supports a native BLE HID remote, and preserves unrestricted raw-WHAD radio behavior.

The completed platform must:

- Preserve the existing ST7789 display, SPIM2 EasyDMA transport, boot/status text, protocol labels, serial DFU trigger, and explicit HFXO startup.
- Expose the IMU, magnetometer, environmental sensors, optical/gesture sensor, microphone metrics, lighting, buzzer, QSPI storage, buttons, and edge connector.
- Add a canonical WHAD Board domain and coordinated support in whad-lib and Python whad-client.
- Provide native BLE HID remote profiles for Android TV, Fire TV, Portal TV, and desktop air-mouse use.
- Keep every CLUE-specific change behind the CLUE board configuration. PCA10059 and MDK builds and behavior must remain unchanged.
- Never silently erase or adopt the 2 MiB QSPI flash.
- Retain full raw sniffing, injection, jamming, and protocol switching in raw-WHAD runtime.

The display, sensors, buttons, storage, outputs, and USB Board-domain services are common platform services. Only ownership of the nRF52840 RADIO is runtime-exclusive.

## 2. Runtime and Build Architecture

### 2.1 Runtime model

The firmware automatically starts its saved runtime; normal startup does not require user selection.

- **Raw-WHAD runtime:** disable S140, explicitly start and wait for HFXO, construct the existing Radio/Core/TimerModule runtime, and advertise the current radio domains plus Board over USB.
- **BLE-HID runtime:** retain or initialize S140, construct the BLE HID/GATT/advertising runtime, and advertise Board-domain capabilities over USB. Do not construct objects that directly own RADIO, CCM/AAR, TIMER0, RTC0, TIMER3/4, or raw-radio PPI resources.

The two radio runtimes remain exclusive because S140 and Butterfly's raw radio implementation require ownership of the same RADIO and timing resources. Common board services continue operating in either runtime.

### 2.2 SDK and memory compatibility

- Build CLUE against **nRF5 SDK 15.3.0**, matching the stock S140 6.1.1 SoftDevice. Keep existing dongle targets on SDK 17.1.
- Add a real CLUE-specific SDK configuration enabling TWIM1, GPIOTE, PDM, PWM0/1, QSPI, SAADC, app_timer, nrf_sdh, BLE HIDS/GATT/advertising, Peer Manager, FDS, and Device Information Service.
- Retain application flash origin **0x26000**, flash length **0xBA000**, RAM origin **0x20002260**, and RAM length **0x3DDA0**.
- Add link-time assertions for the application origin, application end, RAM origin, and bootloader boundary.
- Before BLE initialization, require **SD_SIZE_GET(0) == 0x26000** and **SD_FWID_GET(0) == 0x00B6**. A mismatch displays **SOFTDEVICE MISMATCH** and permits recovery or raw-WHAD fallback without calling incompatible BLE APIs.

Nordic provides the matching SDK in its [nRF5 SDK 15.x archive](https://developer.nordicsemi.com/nRF5_SDK/nRF5_SDK_v15.x.x/).

### 2.3 Automatic startup

1. Initialize only common, SoftDevice-safe facilities: RTC2, buttons, TFT, USB transport, read-only QSPI probe, and persistent-configuration reader.
2. Preserve GPREGRET value **0x57** exclusively for the existing Adafruit DFU path.
3. Read and immediately clear GPREGRET2:
   - **0xB1:** raw-WHAD one-shot.
   - **0xB2:** BLE-HID one-shot.
4. If A+B are held for one second, enter the recovery menu.
5. Otherwise choose the runtime in this order:
   - GPREGRET2 one-shot override.
   - Runtime stored in adopted QSPI configuration.
   - Raw-WHAD default.
6. Stop RTC2 and initialize the selected runtime's normal timebase.

### 2.4 Runtime switching

The Board command, local menu, and deliberate mode gesture all use one controlled transition:

1. Reject new streams and storage operations.
2. Return success to the requesting host.
3. Finish already-started QSPI page programming, but do not start an erase.
4. Release all HID keys/buttons and disconnect BLE cleanly if active.
5. Persist the target mode when QSPI is adopted; otherwise set GPREGRET2 for a one-shot selection.
6. Arm a two-second watchdog fallback and reset.

Do not attempt in-place SoftDevice teardown/reinitialization. A reset provides deterministic RADIO, timer, PPI, IRQ, BLE, and driver ownership.

### 2.5 Scheduling and resource rules

- Introduce a Timebase interface backed by the existing 1 MHz TIMER4 source in raw-WHAD and app_timer/RTC1 in BLE-HID.
- Use fixed queues: 32 sensor samples, 16 input/status events, and 16 pending log records.
- ISRs may timestamp, capture DMA completion state, and enqueue only.
- Service USB and radio work before sensors, display, and storage.
- On queue pressure, drop the newest board sample, increment an overrun counter, and report **OVERRUN**. Never block radio/BLE or allocate unbounded memory.
- Limit dirty-region display updates to 10 Hz.
- Defer QSPI sector erases while raw radio is timing-critical or BLE uses an active low-latency interval.

## 3. CLUE Pinout and Peripheral Ownership

The assignments below follow the official [Adafruit CLUE pinout](https://learn.adafruit.com/adafruit-clue/pinouts) and [CLUE PCB sources](https://github.com/adafruit/Adafruit-CLUE-PCB).

### 3.1 Onboard devices

| Function | nRF52840 pins | Driver/configuration |
|---|---|---|
| ST7789 TFT | SCK P0.14, MOSI P0.15, CS P0.12, DC P0.13, RST P1.03, BL P1.05 | SPIM2, mode 0, 4 MHz, EasyDMA, 240×240, X offset 80 |
| Shared I²C/STEMMA QT | SDA P0.24, SCL P0.25 | TWIM1, 400 kHz, IRQ priority 6 |
| IMU interrupt | P1.06 | Active-high GPIOTE |
| APDS9960 interrupt | P0.09 | Active-low open-drain GPIOTE with pull-up |
| PDM microphone | DATA P0.00, CLK P0.01 | Mono left channel, falling edge |
| Buzzer | P1.00 | PWM1 through BSS138 |
| NeoPixel | P0.16 | PWM0 EasyDMA, GRB order |
| White LEDs | P0.10 | Active-high illumination output |
| Red status LED | P1.01 | Active-high status output |
| Button A | P1.02 | Active-low input with pull-up |
| Button B | P1.10 | Active-low input with pull-up |
| QSPI clock/chip select | P0.19/P0.20 | GD25Q16 |
| QSPI IO0/IO1/IO2/IO3 | P0.17/P0.22/P0.23/P0.21 | MOSI/MISO/WP/HOLD |

### 3.2 Fixed peripheral allocation

- SPIM2: TFT.
- TWIM1: onboard sensors and external STEMMA QT.
- SPIM3: expert external SPI.
- PWM0: NeoPixel.
- PWM1: buzzer.
- PDM: microphone.
- QSPI: external flash.
- SAADC: edge analog inputs.
- GPIOTE high-accuracy channels: IMU INT1 and APDS interrupt.
- GPIOTE low-power port events: Buttons A and B.
- TIMER3/TIMER4 and PPI channels 24/25: raw-WHAD runtime.
- RTC1/app_timer: BLE-HID runtime.
- RTC2: early startup and recovery menu only.
- All board nrfx interrupts: priority 6.

### 3.3 External edge connector

| Label | nRF pin | Analog function |
|---|---|---|
| D0/A2 | P0.04 | AIN2 |
| D1/A3 | P0.05 | AIN3 |
| D2/A4 | P0.03 | AIN1 |
| D3/A5 | P0.28 | AIN4 |
| D4/A6 | P0.02 | AIN0 |
| D5 | P1.02 | Shared with Button A |
| D6 | P1.09 | — |
| D7 | P0.07 | — |
| D8 | P1.07 | — |
| D9 | P0.27 | — |
| D10/A7 | P0.30 | AIN6 |
| D11 | P1.10 | Shared with Button B |
| D12/A0 | P0.31 | AIN7 |
| D13/SCK | P0.08 | External SPIM3 SCK |
| D14/MISO | P0.06 | External SPIM3 MISO |
| D15/MOSI | P0.26 | External SPIM3 MOSI |
| D16/A1 | P0.29 | AIN5 |
| D19/SCL | P0.25 | Shared I²C |
| D20/SDA | P0.24 | Shared I²C |

All GPIO is 3.3 V only and is not 5 V tolerant. USB D+/D−, SWD, reset, and power rails must never be exposed through the expert APIs.

## 4. Shared I²C Bus and Sensor Discovery

Probe each expected device independently. A missing or faulty sensor degrades only that capability.

| Device | Address | Identification |
|---|---:|---|
| LSM6DS33 | 0x6A | WHO_AM_I 0x69 |
| LSM6DS3TR-C revision | 0x6A | WHO_AM_I 0x6A |
| LIS3MDL | 0x1C | WHO_AM_I 0x3D |
| APDS9960 | 0x39 | ID 0xAB |
| SHT30 | 0x44 | CRC-valid response |
| BMP280 | 0x77 | ID 0x58 |

Transactions must be asynchronous and use a 10 ms timeout. Bus recovery clocks SCL nine times, generates STOP, reinitializes TWIM1, and reprobes affected devices. Drivers must serialize access through a single bus manager.

## 5. Motion, Orientation, and Remote Input

### 5.1 IMU

Detect the installed IMU variant at runtime and use a common interface.

- **CTRL3_C = 0x44:** block-data-update and register auto-increment.
- **CTRL1_XL = 0x48:** 104 Hz, ±4 g, 0.122 mg/LSB.
- **CTRL2_G = 0x44:** 104 Hz, ±500 dps, 17.5 mdps/LSB.
- **INT1_CTRL = 0x03:** accelerometer and gyro data-ready.
- Burst-read 12 bytes beginning at **OUTX_L_G (0x22)**.

Perform a two-second stationary calibration using 208 samples. Calculate and store gyro bias and acceleration gravity offset. Report clipping, missed interrupt, stale-data, and calibration state.

### 5.2 Magnetometer

Configure LIS3MDL:

- CTRL_REG1 = 0xF8.
- CTRL_REG2 = 0x00.
- CTRL_REG3 = 0x00.
- CTRL_REG4 = 0x0C.
- CTRL_REG5 = 0x40.
- Continuous 40 Hz, ±4 gauss, 6842 LSB/gauss.

Provide a guided 30-second figure-eight calibration. Store hard-iron offset and a 3×3 soft-iron correction matrix.

### 5.3 Fusion

- Run Madgwick fusion at 104 Hz with beta **0.08**.
- Use calibrated magnetometer input while field magnitude remains within 25% of its calibrated expected norm.
- Fall back to 6DoF when the magnetometer is absent, uncalibrated, saturated, or disturbed for more than 500 ms.
- Expose quaternion components as signed Q30 and Euler yaw/pitch/roll in millidegrees.

Follow the [LSM6DS3TR-C](https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf) and [LIS3MDL](https://www.st.com/resource/en/datasheet/lis3mdl.pdf) register definitions.

### 5.4 Air mouse and tilt navigation

- Generate relative mouse reports at 60 Hz.
- Apply a 0.8 dps dead zone.
- Use 8 pixels/degree base sensitivity.
- Increase sensitivity smoothly to 14 pixels/degree above 120 dps.
- Preserve fractional X/Y movement between reports and clamp each report to signed int16.
- Recenter when pointer mode is enabled or explicitly requested.
- Optional tilt-to-D-pad mode engages at 25 degrees, releases at 17 degrees, and emits only one direction at a time.

## 6. Environmental and Optical Sensors

### 6.1 SHT30

- Use single-shot, medium-repeatability command **0x240B**.
- Default to 1 Hz.
- Wait asynchronously for up to 6.5 ms.
- Validate both result words with CRC-8 polynomial **0x31**, initial value **0xFF**.
- Return temperature in milli-degrees Celsius and humidity in milli-percent RH.

### 6.2 BMP280

- Probe ID register 0xD0 for value 0x58.
- Read calibration data from 0x88 through 0xA1.
- Use Bosch's fixed-point compensation calculations.
- Default forced profile: temperature ×2, pressure ×4, **ctrl_meas 0x4D**, 1 Hz.
- Optional altitude profile: **ctrl_meas 0x4F**, **config 0x28**, approximately 14 Hz.
- Burst-read 0xF7 through 0xFC.
- Return pressure in Pa and temperature in milli-degrees Celsius.
- Calculate altitude only when the caller supplies sea-level pressure.

Use the [SHT3x](https://sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf) and [BMP280](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf) datasheets as normative references.

### 6.3 APDS9960

The optical and gesture engines share resources, so expose two mutually exclusive profiles:

- **Optical:** RGBC and proximity at up to 10 Hz. Read 16-bit RGBC values from 0x94–0x9B and proximity from 0x9C.
- **Gesture:** use P0.09 interrupt and FIFO registers to emit UP, DOWN, LEFT, RIGHT, NEAR, and FAR.

Gesture defaults:

- Entry threshold 40.
- Exit threshold 30.
- FIFO threshold 4.
- Gain 4×.
- LED drive 100 mA.
- Ten 32 µs pulses.

APDS swipes map to directional remote commands in TV profiles. Proximity remains informational unless explicitly mapped. Follow the [APDS9960 datasheet](https://cdn-learn.adafruit.com/assets/assets/000/045/848/original/Avago-APDS-9960-datasheet.pdf?1504034182).

## 7. Audio, Lighting, Display, and Buttons

### 7.1 Microphone

- Configure PDM at 1.032 MHz with a 64× ratio, yielding approximately 16.125 kHz mono PCM16.
- Select the left channel/falling edge and gain 20.
- Use two 256-sample DMA buffers.
- Publish RMS Q15, peak Q15, and dBFS×1000 at 20 Hz.
- Support threshold, hysteresis, and debounce for sound-trigger events.
- Permit raw PCM diagnostics over USB only in raw-WHAD runtime while all radio operations are idle.
- Encode diagnostic data as little-endian PCM16 chunks no larger than 40 bytes.
- Reject unsafe raw requests with BUSY. Do not send microphone audio over BLE or log raw audio to QSPI.

### 7.2 Buzzer and LEDs

- Buzzer: PWM1, 1 MHz base, 50% duty, 200–4000 Hz, nonblocking duration.
- NeoPixel: PWM0 EasyDMA, 16 MHz base, countertop 20, duty values 6 and 13, 24-bit GRB, at least 80 µs reset.
- Treat P0.10 as a white illumination output, not a generic RGB channel.
- Map generic protocol/status colors to the NeoPixel.
- Use P1.01 as the explicit activity/error LED.

### 7.3 Display

- Retain ST7789 SPIM2 EasyDMA; never reintroduce GPIO bit-banging.
- Keep initialization, font, boot/status text, protocol labels, and backlight support.
- Introduce dashboard pages for runtime, radio state, motion, environment, optical sensing, storage, and errors.
- Update only dirty regions and cap routine refresh at 10 Hz.
- Avoid full-screen transfers during timing-critical raw-radio work.

### 7.4 Buttons

- Debounce both buttons for 20 ms.
- Use a 60 ms chord-detection window.
- Raw-WHAD defaults: A toggles backlight; B cycles dashboard pages.
- BLE-HID defaults: A sends Select; B sends Back.
- Suppress all host-facing A/B reports once an A+B chord is recognized.

## 8. BLE HID Remote

### 8.1 BLE behavior

- Device name: **Butterfly CLUE Remote**.
- One peripheral connection.
- Bonding required.
- LE Secure Connections Just Works, MITM false, 16-byte key.
- Active connection interval 7.5–15 ms, slave latency 0, supervision timeout 4 seconds.
- Relax latency while motion output is idle.
- Fast advertising at 30 ms for 30 seconds, then 250 ms indefinitely.
- Reconnect bonded peers.
- Include Device Information Service.
- Omit Battery Service because the CLUE has no battery-sense path.

### 8.2 HID reports

| Report ID | Contents |
|---:|---|
| 1 | Mouse: eight buttons, relative signed int16 X/Y, int8 wheel and pan |
| 2 | Keyboard: modifiers, reserved byte, six keycodes |
| 3 | Consumer Control: one 16-bit usage selector; zero releases |

The Consumer Control selector covers usages 0x0000–0x02FF:

- Select: Menu Pick **0x0041**.
- Up/Down/Left/Right: **0x0042–0x0045**.
- Back: Menu Escape **0x0046**.
- Home: AC Home **0x0223**.
- Alternate Back: AC Back **0x0224**.
- Volume Up/Down/Mute: **0x00E9/0x00EA/0x00E2**.
- Play/Pause: **0x00CD**.
- Search: AC Search **0x0221**.

Use the [USB HID Usage Tables](https://www.usb.org/hid) as the normative source. Validate resulting actions against [Android TV navigation guidance](https://developer.android.com/design/ui/tv/guides/styles/focus-system) and [Amazon's Fire TV input reference](https://www.developer.amazon.com/docs/fire-tv/remote-input.html).

### 8.3 Remote profiles

- **android_tv**, **fire_tv**, and **portal_tv:** A always sends Consumer Menu Pick; B sends Menu Escape; APDS swipes send Consumer Menu directions.
- **desktop_airmouse:** A sends mouse button 1 if pointer motion occurred in the previous three seconds, otherwise keyboard Enter; B sends Escape.
- **custom:** independently configure button usages, APDS mappings, sensitivity, dead zone, pointer mode, and tilt mode.

Press and release reports must always be paired. Never send Consumer, keyboard, and mouse fallbacks simultaneously.

## 9. Local Menu and Deliberate Mode Gesture

### 9.1 Recovery menu

Hold A+B during reset for one second. The menu must work even if the IMU or QSPI is unavailable.

- B: next item.
- A: select.
- Long B: back.
- Items: Status, Runtime Override, Pair/Forget, IMU Calibration, Magnetometer Calibration, Remote Profile, Storage Adopt/Erase Logs, Sensor Dashboard, DFU, Exit.

### 9.2 Normal menu and runtime gesture

- Hold A+B for 1.5 seconds.
- Release without rotating to open the normal menu.
- Continue holding to perform the runtime-switch gesture:
  1. Capture the normalized initial gravity vector.
  2. Within four seconds, rotate until the current gravity dot initial gravity is at most −0.75 for 150 ms.
  3. Return until the dot product is at least 0.75 for 150 ms.
  4. Release both buttons.
  5. Display the target runtime; A confirms and B cancels within five seconds.
- Use display, NeoPixel, and buzzer feedback for armed, inverted, returned, and confirmed phases.
- If QSPI is adopted, persist the runtime and reset.
- Otherwise set the appropriate GPREGRET2 one-shot value, display **NOT SAVED**, and reset.
- If IMU detection fails, retain runtime switching through the recovery menu and USB Board command.

All destructive menu actions require a separate warning and deliberate confirmation.

## 10. QSPI Storage and Persistence

### 10.1 Read-only probe and adoption

- Probe with JEDEC command 0x9F.
- Nominal GD25Q16 ID: **C8 40 15**.
- Capacity: **2 MiB**.
- Start in mode 0 at 16 MHz.
- Do not alter the QE bit until adoption.

Before adoption, never issue program, erase, status-register-write, or other mutating commands. Sensors and HID operate with volatile defaults, and runtime changes are one-shot.

StorageInfo returns JEDEC ID, capacity, state, format version, and a fresh adoption nonce. Host adoption requires the current nonce, **erase_all=true**, and a CLI flag named **--yes-really-adopt-and-erase**. On-device adoption displays a full-erasure warning and requires holding A+B for three seconds.

### 10.2 Partition map

| Address range | Size | Purpose |
|---|---:|---|
| 0x000000–0x001FFF | 8 KiB | Two alternating 4 KiB superblocks |
| 0x002000–0x00FFFF | 56 KiB | Configuration journal |
| 0x010000–0x01FFFF | 64 KiB | Calibration and remote profiles |
| 0x020000–0x1FFFFF | 1920 KiB | Circular event and packet log |

Records are four-byte aligned and contain:

- Magic and format version.
- Record type and payload length.
- Monotonically increasing sequence.
- Timestamp in microseconds.
- Header CRC32.
- Payload CRC32.
- Payload up to 1024 bytes.

Use CRC-32/ISO-HDLC with reflected polynomial **0xEDB88320**, initial value **0xFFFFFFFF**, and final XOR **0xFFFFFFFF**.

Use copy-on-write records and alternating superblocks. At boot, select the highest valid sequence. Power loss must leave either the previous or replacement record valid. Pre-erase 4 KiB sectors only when runtime timing permits.

### 10.3 Internal BLE bond storage

- Peer Manager/FDS uses three 4 KiB pages at **0xF1000–0xF3FFF**.
- Declare the full Adafruit InternalFS range **0xED000–0xF3FFF** unavailable to Butterfly.
- Before first FDS initialization, inspect the pages. If they contain unknown non-erased data, require an on-device warning and three-second A confirmation before erasing.
- Never access the bootloader at **0xF4000** or above.
- Forget Bonds requires a separate confirmation.

## 11. WHAD Board Domain

### 11.1 Canonical protocol allocation

- Domain enum: **Board = 0x0C000000**.
- Top-level protobuf oneof field: tag **8**.
- Capability bits:
  - Read: **0x000100**.
  - Write: **0x000200**.
  - Stream: **0x000400**.
  - Store: **0x000800**.

Command bitmap:

| ID | Command | ID | Command |
|---:|---|---:|---|
| 0 | GetBoardInfo | 14 | AdcRead |
| 1 | ListSensors | 15 | SpiTransfer |
| 2 | ReadSensor | 16 | StorageInfo |
| 3 | ConfigureStream | 17 | StorageAdopt |
| 4 | StopStream | 18 | StorageReadLog |
| 5 | Calibrate | 19 | StorageEraseLog |
| 6 | GetCalibration | 20 | GetRuntimeConfig |
| 7 | SetOutput | 21 | SetRuntimeConfig |
| 8 | GetInputState | 22 | SetRuntimeMode |
| 9 | ConfigureInput | 23 | RemoteProfileGet |
| 10 | I2cTransfer | 24 | RemoteProfileSet |
| 11 | GpioConfigure | 25 | AudioConfigure |
| 12 | GpioRead | 26 | ReleasePin |
| 13 | GpioWrite | 27 | RawPcmDiagnostics |

Typed queries return one typed response. Mutations return the existing generic command result. Board errors map to INVALID_ARGUMENT, BUSY, NOT_FOUND, PERMISSION_DENIED, NOT_ADOPTED, SENSOR_FAULT, TIMEOUT, CHECKSUM, OVERFLOW, or SOFTDEVICE_MISMATCH.

### 11.2 Events and samples

Asynchronous message types:

- SensorSample.
- AudioChunk.
- InputEvent.
- GestureEvent.
- LogChunk.
- BoardStatus.

Every encoded message must remain within the existing 64-byte transport limit.

SensorSample fields:

- sensor_id: uint32.
- sequence: uint32.
- timestamp_us: uint64.
- status: uint32.
- values: up to ten packed sfixed32 values.

| Sensor ID | Values and units |
|---:|---|
| 1 | Acceleration X/Y/Z, mg |
| 2 | Gyro X/Y/Z, mdps |
| 3 | Magnetic X/Y/Z, milligauss |
| 4 | Quaternion W/X/Y/Z, Q30 |
| 5 | Yaw/Pitch/Roll, millidegrees |
| 6 | Pressure, Pa |
| 7 | BMP temperature, milli-degrees Celsius |
| 8 | Humidity, milli-percent RH |
| 9 | SHT temperature, milli-degrees Celsius |
| 10 | Clear/red/green/blue raw counts |
| 11 | Proximity, 0–255 |
| 12 | Gesture enum |
| 13 | RMS Q15, peak Q15, dBFS×1000 |
| 14 | Air-mouse dx/dy/wheel/buttons |

Status bits: CALIBRATED, STALE, SATURATED, OVERRUN, MAGNETIC_DISTURBANCE, SENSOR_FAULT.

ListSensors is index-based and returns one descriptor at a time. ConfigureStream configures one sensor using rate_millihz and flags; firmware clamps it to a supported rate and reports the actual rate.

### 11.3 Repository coordination

Implement in this order:

1. Canonical WHAD schema and specification.
2. Generated nanopb/C/C++ support in whad-lib.
3. Python whad-client generated messages and Board-domain wrappers.
4. Butterfly submodule update and firmware implementation.

Python support adds a BoardConnector with synchronous configuration/read methods, asynchronous sample/event iterators, and CLI commands for info, streams, calibration, outputs, storage, runtime/profile management, and expert I/O.

All protocol additions are append-only. Released clients must continue discovery and operation of existing radio domains when they encounter the numeric Board domain. Validate against the documented [WHAD discovery service](https://whad-protocol.readthedocs.io/en/latest/discovery.html).

## 12. Expert External I/O

Maintain a runtime pin-ownership registry. Ordinary operations return BUSY for owned or reserved pins.

With **force=true**:

1. Stop and uninitialize the current owner.
2. Grant a lease until ReleasePin or reset.
3. Emit BoardStatus naming the displaced service.
4. Reinitialize and reprobe the owner when released.

Interfaces:

- GPIO: input/output, pull-up/down, standard/high drive, sense, read, and write.
- SAADC: 12-bit, internal 0.6 V reference, gain 1/6, optional 4× oversampling. Enforce the physical 0–3.3 V limit.
- I²C: 7-bit address, repeated start, up to 256 bytes each direction, 10 ms timeout. Transactions to known onboard addresses require force=true.
- SPI: SPIM3, modes 0–3, 125 kHz–8 MHz, up to 256 bytes, leased GPIO chip select.

Taking TFT pins blanks the display. Taking I²C pins stops sensors. Taking button pins disables local controls until release/reset. Taking QSPI, PDM, NeoPixel, or buzzer pins stops the corresponding service. USB, SWD, reset, and power pins remain inaccessible regardless of force.

## 13. Delivery Sequence

### Milestone 1: Foundation

- Pin CLUE to SDK 15.3.
- Add CLUE-specific SDK configuration.
- Implement automatic runtime startup, SoftDevice validation, Timebase, controlled reboot transitions, event queues, and pin ownership.
- Preserve existing TFT, DFU, and HFXO behavior.
- Correct the CLUE LED model so the NeoPixel is RGB and P0.10 is white illumination.
- Land the minimal canonical Board API required to expose BoardInfo, runtime state, and runtime switching through generated whad-lib support, BoardConnector, and CLI commands.

### Milestone 2: Protocol and host

- Land the canonical Board domain.
- Regenerate whad-lib and Python bindings.
- Add BoardConnector and CLI.
- Add old-client compatibility tests.
- Expose each supported Board command through a typed client-library method and a corresponding CLI operation before firmware support is considered complete.

### Milestone 3: Motion platform

- Add TWIM1 bus manager.
- Support both CLUE IMU revisions and LIS3MDL.
- Add calibration, fusion, orientation streams, air mouse, tilt navigation, and the deliberate mode gesture.
- Add client-library sensor descriptors, read/configure-stream methods, asynchronous motion iterators, calibration methods, and CLI equivalents.

### Milestone 4: BLE HID

- Add S140 runtime, HIDS reports, advertising, security, Peer Manager/FDS, remote profiles, pairing UI, and target-device validation.
- Add BoardConnector and CLI support for current runtime, runtime switching, HID status, pairing/bond reset, and profile get/set operations.

### Milestone 5: Environment and optical

- Add SHT30, BMP280, APDS9960, dashboard pages, streams, optical modes, and remote swipe mappings.
- Expose environment/optical sensor descriptors, samples, stream configuration, and APDS profile selection through the client library and CLI.

### Milestone 6: Audio and outputs

- Add PDM metrics/diagnostics, sound triggers, buzzer, NeoPixel, white illumination, and status behavior.
- Expose output control, audio metrics/threshold configuration, and permitted raw PCM diagnostics through typed client methods and CLI commands.

### Milestone 7: QSPI

- Add read-only probe, explicit adoption, journals, calibration/profile persistence, logging, and power-loss recovery.
- Expose storage inspection, nonce-confirmed adoption, log retrieval/erasure, and persistence status through the client library and CLI.

### Milestone 8: Expert I/O

- Add GPIO, ADC, I²C, SPI, force takeover, release/recovery, CLI support, and safety documentation.
- Expose typed GPIO, ADC, I²C, SPI, lease, force-takeover, and release operations through the client library with matching CLI commands.

Each milestone should be a separate reviewable PR or commit series. No milestone is firmware-only: every newly usable capability must ship with its canonical schema support, generated whad-lib bindings, Python client-library method or iterator, CLI path where interactive use is meaningful, and host-side tests. Do not include unrelated working-tree changes or local flashing utilities unless separately approved.

## 14. Verification and Acceptance

### 14.1 Automated tests

- Build CLUE against SDK 15.3 and existing targets against SDK 17.1.
- Keep CLUE flash below 70% of 0xBA000 and static RAM below 50% of available application RAM.
- Unit-test all sensor register setup and scaling.
- Test SHT CRC and published Bosch BMP280 compensation vectors.
- Test fusion, magnetic-disturbance fallback, calibration serialization, and gesture state machines.
- Test pin ownership, force takeover, release, and reset restoration.
- Test protobuf round trips and enforce the 64-byte frame limit.
- Test stream backpressure and queue overflow behavior.
- Test QSPI journal recovery after interruption at every erase/program boundary.
- Test automatic runtime precedence, one-shot GPREGRET2 behavior, persisted mode, and controlled reset.

### 14.2 Hardware validation

- Test the supported CLUE IMU configuration with WHO_AM_I 0x69.
- Run all existing WHAD sniff, inject, jam, and protocol smoke tests with common board services enabled but idle.
- Verify BLE initialization never occurs in raw-WHAD and direct RADIO access never occurs while S140 is active.
- Verify released old WHAD clients discover and operate every existing domain.
- Test Android TV, Fire TV, Portal TV where available, and desktop pairing/reconnection.
- Confirm TV Select emits only Consumer Menu Pick, and Back/navigation/media mappings produce the intended host events.
- Measure median motion-to-HID latency below 25 ms.
- Run a one-hour 104 Hz motion/fusion stream with less than 0.1% sample loss, no unbounded drift, and no queue growth.
- Verify there is no QSPI mutation before explicit adoption.
- Cut power during configuration writes, log appends, garbage collection, and superblock replacement, then verify recovery.
- Exercise microphone, buzzer, NeoPixel, white LEDs, APDS interrupt, environmental sensors, GPIO, ADC, external I²C, and external SPI.
- Verify forced takeover consequences and automatic recovery on release/reset.
- Verify serial and menu DFU still enter the stock Adafruit bootloader and that UF2 flashing remains usable.

## 15. Explicit Defaults and Exclusions

- Raw-WHAD is the default runtime without persistent configuration.
- Saved runtime starts automatically; the boot menu is recovery and override only.
- Common board services and USB Board access remain active in both runtimes.
- Raw-WHAD RADIO and native BLE HID are mutually exclusive and switch through controlled reset.
- Do not automatically replace or upgrade the SoftDevice or bootloader.
- Do not implement BLE microphone streaming, proprietary voice-service cloning, fabricated battery reporting, or raw access to protected system pins.
- QSPI configuration, calibration, profiles, logs, and persistent runtime selection require explicit adoption.
- BLE bonding uses separately confirmed internal FDS.
- TV profiles prioritize Consumer Control Select/Back. Keyboard and mouse fallbacks are limited to desktop or explicitly customized profiles.
