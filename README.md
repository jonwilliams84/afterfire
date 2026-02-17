# Afterfire Effect System - Documentation

## Overview

This is an ESP32-S3 based RC car **afterfire LED effect system** that creates realistic exhaust flame effects using WS2812B LEDs. The device reads PWM throttle signals from your RC transmitter and triggers dynamic LED animations that simulate the characteristic popping and flame effects of high-performance engine exhaust systems.

## Hardware Configuration

### Components
- **Microcontroller**: ESP32-S3 (WaveShare ESP32-S3-Zero)
- **LED Strip**: WS2812B addressable RGB LEDs (1+ configurable)
- **Signal Input**: PWM throttle signal on GPIO pin 2
- **Output**: LED control on GPIO pin 3
- **Connectivity**: WiFi enabled (2.4 GHz)

### Pin Mapping
- Pin 2: Throttle input (PWM signal from RC receiver)
- Pin 3: WS2812B LED data line
- Max brightness: 255 (configurable)

### Voltage Level Shifting

RC receivers typically output 5V logic signals, whilst the ESP32-S3 operates at 3.3V and requires signals within this range for safe GPIO input.

**Voltage Divider Configuration**:
A passive voltage divider converts the 5V PWM signal to ~3.3V:
- 10kΩ resistor from RC receiver PWM output to GPIO pin 2
- 22kΩ resistor from GPIO pin 2 to ground

This produces: **Vout = 5V × (22k ÷ 32k) ≈ 3.44V**

This is safely within the ESP32-S3 input tolerance and protects the microcontroller from voltage damage. No active level shifter is required, making this approach simple and reliable.



## Operating Principles

### PWM Signal Processing

The system uses an interrupt-driven approach to read PWM signals from your RC transmitter:

1. **Signal Capture**: Hardware interrupt on GPIO pin 2 triggers on rising and falling edges
2. **Pulse Width Measurement**: Calculates the duration between edges (typically 1000-2000 microseconds)
3. **Throttle Normalisation**: Converts pulse width to a throttle percentage:
   - **Negative values (-100% to 0%)**: Reverse/brake position
   - **0%**: Neutral/idle (dead zone)
   - **Positive values (0% to +100%)**: Forward throttle

### Calibration System

The system includes a robust 3-step calibration process to accommodate different transmitters and link configurations:

**Step 1 - Neutral Position**: Centre your throttle stick and capture the PWM value. The system creates a ±25μs dead zone around this value.

**Step 2 - Full Throttle**: Push the stick fully forward and capture. This defines the upper throttle limit.

**Step 3 - Full Brake/Reverse**: Pull the stick fully backward and capture. This defines the lower throttle limit.

Once calibrated, all future throttle readings map correctly regardless of your transmitter's specific PWM range.

## LED Effects System

### Effect 1: RPM Flicker

**Purpose**: Simulates the glow of hot exhaust gases flowing through the tailpipe during acceleration.

**Trigger**: Active whenever throttle position exceeds the configurable threshold (default: 30%)

**Behaviour**:
- Base colour intensity maps to throttle percentage
- Random ±40 heat fluctuations every cycle create flickering
- Colour progression: deep red → orange → yellow-white as throttle increases
- Fades at 40 brightness units/cycle when below threshold

**Configuration**: Adjustable start threshold (0-50%)

### Effect 2: Backfire

**Purpose**: Creates the dramatic multi-coloured flame burst characteristic of aggressive engine deceleration.

**Trigger**: Detected when:
- Throttle was previously high (>30% by default)
- Throttle suddenly drops below release threshold (<15% by default)
- Simulates unburned fuel in the exhaust igniting on overrun

**Behaviour**:
- Triggers 3-8 random-coloured bursts based on how aggressively throttle was released
- Each burst is a different colour (blue, purple, red-orange, or bright orange-yellow)
- Bursts occur at randomised intervals (20-80ms apart)
- Creates chaotic visual effect matching the unpredictability of real backfires

**Configuration**: 
- Minimum throttle before release (default: 30%)
- Maximum throttle to trigger effect (default: 15%)

### Effect 3: Brake Crackle

**Purpose**: Simulates engine braking and fuel ignition when aggressively slowing down.

**Trigger**: Detected when:
- Throttle was moderately high (>20% by default)
- Throttle drops to brake position (<-20% by default)
- Does not trigger if a burst is already active

**Behaviour**:
- Similar to backfire but typically fewer bursts (3-6)
- Randomised intensities (160-230 brightness)
- Creates the "crackle" effect of fuel popping in the exhaust

### Effect 4: Idle Burble

**Purpose**: Adds character during idle/neutral by simulating occasional fuel ignition.

**Trigger**: At neutral/idle position (±5% of centre)

**Behaviour**:
- Random chance: 4 in 1000 per cycle (~1 per 10 seconds statistically)
- Single flare with random intensity (100-160 brightness)
- Very subtle, low-intensity effect
- Does not trigger during active bursts

## Flame Colour Model

The system maps heat intensity (0-255) to realistic flame colours:

| Heat Range | Colour | Appearance |
|-----------|--------|-----------|
| 0-120 | Deep red RGB(H, H/4, 0) | Initial combustion, fuel-rich |
| 120-200 | Orange RGB(255, H, 0) | Hot flame, mixed combustion |
| 200-255 | Yellow-white RGB(255, 255, H-200) | Peak temperature, complete combustion |

This progression matches the actual colour signatures of engine exhaust flames.

## Burst System Architecture

Bursts are handled via a non-blocking state machine to ensure smooth LED updates:

1. **Trigger**: Effect detection sets `burstActive = true`, configures burst count and intensity
2. **Scheduling**: Each burst fires at a randomised interval (not blocking)
3. **Execution**: Colour is randomly selected from four categories
4. **Completion**: After all bursts, LEDs fade to black and system returns to normal effect handling

**Key Feature**: Non-blocking design means throttle input remains responsive even during active bursts—critical for realistic tail-car operation.

## Web Interface & Remote Control

### Features

The device hosts a web server accessible at `http://<device-ip>/` providing:

**Monitoring Dashboard**:
- Real-time PWM signal display (in microseconds)
- Current throttle position percentage
- Active burst indicator
- System uptime and WiFi signal strength

**Effect Controls**:
- Individual toggle switches for all four effects
- Real-time visual feedback of enabled/disabled status

**Sensitivity Adjustment**:
- **Backfire threshold**: Min throttle before release (10-60%)
- **Backfire release threshold**: Max throttle to trigger (5-40%)
- **RPM flicker threshold**: Throttle % where LEDs start glowing (0-50%)
- Live slider controls with instant feedback

**Manual Testing**:
- Test Backfire button
- Test Crackle button
- No calibration needed, useful for tuning effect sensitivity

**Calibration Wizard**:
- Step-by-step guided calibration with live PWM monitoring
- Visual confirmation of each captured value
- Automatic progression through steps

**Firmware Updates**:
- Over-The-Air (OTA) update capability
- Live progress monitoring
- LED status feedback during update process

### Technical Details

- **Web Server**: Built-in on port 80
- **API**: RESTful JSON endpoints for all functions
- **Update Frequency**: Status updates every 2 seconds by default
- **OTA Password**: "afterfire2026" (should be changed in production)
- **Hostname**: "afterfire-esp32" for network discovery

## Main Control Loop

The core loop executes every 5 milliseconds:

```
1. Handle incoming web requests and OTA updates
2. Read latest PWM throttle value (from interrupt)
3. Convert PWM to throttle percentage
4. Detect calibration state or normal operation
5. Call effect handlers in sequence:
   - RPM Flicker (if enabled)
   - Backfire Detection (if enabled)
   - Brake Crackle Detection (if enabled)
   - Idle Burble (if enabled)
6. Handle any active burst animations
7. Apply gradual fade-to-black if no effects are active
8. Update LED strip with current colours
9. Delay 5ms before next cycle
```

This 5ms cycle time ensures smooth 200Hz refresh rate, which is imperceptible to the human eye and provides responsive throttle tracking.

## Configuration Reference

### User Adjustable Parameters

| Parameter | Default | Range | Purpose |
|-----------|---------|-------|---------|
| `NEUTRAL_MIN` | 1890 | - | PWM lower bound of neutral zone |
| `NEUTRAL_MAX` | 1930 | - | PWM upper bound of neutral zone |
| `MIN_PULSE` | 1496 | - | Full brake/reverse PWM value |
| `MAX_PULSE` | 2000 | - | Full throttle PWM value |
| `backfireThrottleMin` | 30 | 10-60% | Minimum throttle before release |
| `backfireReleaseMax` | 15 | 5-40% | Maximum throttle to trigger backfire |
| `rpmFlickerThreshold` | 30 | 0-50% | Throttle % where flicker starts |
| `enableBackfire` | true | - | Feature toggle |
| `enableBrakeCrackle` | true | - | Feature toggle |
| `enableIdleBurble` | true | - | Feature toggle |
| `enableRPMFlicker` | true | - | Feature toggle |

All throttle-dependent parameters can be adjusted via the web interface in real-time.

## Serial Debugging

The system outputs comprehensive debugging information at 115200 baud via USB serial:

- Boot sequence and chip information
- Throttle signal analysis (500ms intervals)
- Effect triggering events
- Calibration progress
- Web server and OTA events
- Error diagnostics

Connect via USB-C to monitor system operation or troubleshoot issues.

## WiFi & Network Configuration

### First Boot Setup (Access Point Mode)

On first boot, if no WiFi credentials are stored in EEPROM, the device automatically enters **Access Point (AP) mode** for initial setup:

1. **Device broadcasts AP SSID**: `afterfire-setup`
2. **AP Password**: `afterfire` (default)
3. **AP IP Address**: `192.168.4.1`

**First-Time Setup Flow**:
1. Connect your mobile device or laptop to the `afterfire-setup` WiFi network
2. Open a web browser and navigate to `http://192.168.4.1`
3. The web interface displays available WiFi networks with signal strength
4. Select your home/office WiFi network from the list (or enter manually)
5. Enter your WiFi password
6. Click "Connect & Save"
7. Device saves credentials to EEPROM and reboots
8. Device connects to your WiFi and exits AP mode

**Visual Feedback**: The LED flashes orange rapidly during AP mode to indicate setup interface is active.

### WiFi Connection

Once credentials are saved, the device performs normal WiFi connection:

- **Timeout**: 10 seconds (20 attempts × 500ms)
- **SSID/Password**: Retrieved from EEPROM on every boot
- **Connection Success**: LED displays normal effects, web interface available at device IP
- **Connection Failure**: Device re-enters AP mode for reconfiguration

### Features by Connection Status

| Feature | Connected | AP Mode |
|---------|-----------|---------|
| Throttle effects (backfire, crackle, etc.) | Yes | Yes |
| Web interface monitoring | Yes | Yes (setup page) |
| Effect configuration | Yes | No |
| OTA firmware updates | Yes | No |
| Calibration wizard | Yes | Yes |

### Resetting WiFi Configuration

To forget saved WiFi and return to AP mode setup:

1. Connect to the device when it's in normal WiFi mode
2. Go to Settings page (future enhancement)
3. Click "Forget Network" or "Reset WiFi"
4. Device will reboot into AP mode

**Alternative via Serial**: Send factory reset command via USB serial to clear all EEPROM settings

### Network Security

- **OTA Password**: Default is `afterfire2026` (changeable in code before deployment)
- **AP Password**: Default is `afterfire` (changeable in code)
- **Credentials Storage**: WiFi credentials stored in EEPROM (AES-128 encryption recommended for future versions)

---

## Persistent Settings (EEPROM)

### What Gets Saved

The device automatically saves the following to EEPROM (non-volatile storage):

**Calibration Values** (10 bytes):
- Neutral position (PWM range: ±25μs)
- Full throttle PWM value
- Full brake/reverse PWM value

**Effect Settings** (4 bytes):
- Backfire enabled/disabled toggle
- Brake crackle enabled/disabled toggle
- Idle burble enabled/disabled toggle
- RPM flicker enabled/disabled toggle

**Sensitivity Thresholds** (10 bytes):
- Backfire throttle minimum
- Backfire release maximum
- Brake throttle minimum
- Brake throttle maximum
- RPM flicker threshold

**WiFi Credentials** (64 bytes):
- SSID (up to 32 characters)
- Password (up to 32 characters)

**Metadata** (5 bytes):
- Version number (for future compatibility)
- CRC32 checksum (data corruption detection)

**Total**: ~94 bytes of 512 bytes EEPROM used (81% available for future features)

### When Settings Are Saved

Settings are automatically persisted in these scenarios:

1. **After WiFi configuration** via AP setup page
2. **After calibration completion** (Step 3/3)
3. **After toggling any effect** (on/off)
4. **After adjusting any sensitivity threshold** (via slider)

### Data Validation

- **CRC32 Checksum**: Validates all saved data on boot
- **Version Number**: Ensures forward compatibility with future firmware versions
- **Fallback**: If corrupted settings detected, device uses hardcoded defaults and enters AP mode for reconfiguration

### Boot Sequence with EEPROM

1. Serial output: `[Settings] Loading from EEPROM...`
2. If valid: `[Settings] ✓ Valid settings found` → Calibration values loaded
3. If invalid: `[Settings] No valid settings in EEPROM, using defaults` → Device uses hardcoded defaults
4. WiFi credentials checked
5. If credentials exist: Attempts to connect to saved network
6. If credentials missing or connection fails: Enters AP mode

### Manual EEPROM Reset

**Via Serial Command** (future enhancement):
```
RESET
```

**Via Factory Reset Button** (if hardware button added):
- Hold for 5+ seconds during boot

**Via Code**:
Connect via USB serial and issue reset command to clear all EEPROM and return to AP setup mode.

---

## Safety Considerations

1. **RF Interference**: PWM signal should be well-shielded and kept away from high-frequency components
2. **Power Supply**: Ensure adequate power for both ESP32 and LED strip (WS2812B can draw significant current at full brightness)
3. **Thermal Management**: Allow adequate ventilation for the microcontroller
4. **Default Credentials**: Change OTA password before deploying to untrusted networks
5. **Calibration Critical**: Improper calibration may cause effects to trigger unexpectedly during normal operation

## Production-Ready Features

### Credentials Management
- **No Hardcoded Secrets**: WiFi credentials stored securely in EEPROM
- **First-Boot Setup**: Automatic AP mode for initial WiFi configuration
- **Credential Reconfiguration**: Can change WiFi network via AP mode reset
- **OTA Password**: Changeable before deployment

### Persistent Storage
- **CRC32 Validation**: Automatic corruption detection on settings load
- **Version Control**: Settings structure supports firmware version upgrades
- **Graceful Fallback**: Invalid EEPROM triggers default values and AP mode
- **Atomic Writes**: EEPROM.commit() ensures data consistency

### Boot Modes
- **Normal Mode**: Connects to saved WiFi, full feature set available
- **Access Point Mode**: Activated on first boot or failed connection, setup interface at 192.168.4.1
- **Offline Mode**: All effects fully functional regardless of WiFi status

### Data Integrity
- **Automatic Persistence**: All user configuration changes saved immediately
- **EEPROM Wear Levelling**: ESP32 EEPROM implements wear-levelling across 512 byte allocation
- **Configuration Preservation**: Settings survive power cycles and firmware updates

### Security Considerations
- WiFi credentials stored in plaintext in EEPROM (improvement: add AES-128 encryption in future)
- OTA password should be changed before production deployment
- AP mode password should be changed before public deployment
- HTTPS not implemented (local network assumed trusted)

## Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| LEDs don't respond | PWM signal not reaching GPIO 2 | Check receiver connection and PWM calibration |
| Effects trigger unexpectedly | Calibration incorrect | Run 3-step wizard to recalibrate |
| Web interface unreachable | WiFi not connected | Check WiFi credentials or operate via AP mode |
| LEDs stay dim | Power supply insufficient | Upgrade PSU or reduce `MAX_BRIGHTNESS` |
| Erratic throttle readings | Electrical noise | Add capacitor to throttle input, shield wiring |
| Device stuck in AP mode | No WiFi credentials saved | Enter credentials via AP setup page (192.168.4.1) |
| Settings lost after reboot | EEPROM corrupted | Device uses defaults; reconfigure via web interface |

## Customisation Options

The code is structured for easy modification:

- **Number of LEDs**: Change `NUM_LEDS` constant
- **Pin assignments**: Modify `THROTTLE_PIN` and `LED_PIN`
- **Effect timing**: Adjust `delay(5)` and burst timing ranges
- **Colour palettes**: Modify RGB values in `setFlame()` and burst colour selection
- **Sensitivity defaults**: Update all threshold constants (NOTE: Will be overridden by EEPROM on subsequent boots)
- **AP Mode SSID/Password**: Change in `startAPMode()` function
- **OTA Password**: Change in `setup()` function before deployment

**Note on Defaults**: Hardcoded sensitivity defaults and calibration values are only used on first boot if EEPROM is empty. After initial configuration, all values are loaded from EEPROM and survive firmware updates.

All parameters are well-commented for easy identification and modification.

---

**Version**: 1.0  
**Date**: 17 February 2026  
**Microcontroller**: ESP32-S3 (WaveShare ESP32-S3-Zero)  
**LED Type**: WS2812B (NeoPixel compatible)
