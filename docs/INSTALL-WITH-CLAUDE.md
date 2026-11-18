# Install guide for an AI assistant (smartAircon)

**Human**: give this file to Claude (or another AI assistant) together with the link to this repository and say
"help me install this". Then answer its questions. You do the physical work; it tells you what to do and checks
each step with you.

---

**Assistant**: you are helping a person install smartAircon, an ESP32 (M5Stack Atom Lite + RS485) that **replaces the
wired wall controller** of a ducted air conditioner and exposes it to Home Assistant via MQTT. Read `README.md` and
`docs/PROTOCOL.md` of the repository first. Go **one step at a time**: give one instruction, wait for the result, then
continue. Answer in their language.

## Rules

- **Safety first.** The indoor unit is mains powered: it must be **switched off at the breaker** whenever its
  electrical box is open. The wall-controller cable itself is low voltage (12 V), but it lives next to mains wiring.
- The Atom **replaces** the wall controller: two controllers on the same bus do not work. Tell the person to keep the
  original controller, so they can always go back.
- Never ask the person to paste passwords into the chat; they go in `secrets.h`, which is not committed.
- If their unit, board or controller differ from the ones in the README, say clearly that it is an experiment. The
  frame format may differ: help them capture the bus first (step 5) instead of sending commands blindly.

## Step 1: check the unit

Ask for:
1. Indoor unit model (label on the unit), outdoor unit model, wall controller model (on its back).
2. Tested only with indoor **SDVH12M-A3NA**, controller **XKRB1**, indoor board **K32600059 SCN904-Z**, whose
   wall-controller connector is **485-1 / 485-2** with **GND, A, B, +12 V**. If they have the service manual, look for
   the PCB diagram of the indoor board.

## Step 2: what they need

- M5Stack Atom Lite + **M5Stack ATOM RS485 base** (it accepts 12 V on its DC input), or another ESP32 + RS485
  transceiver (then adapt pins and power).
- A way to connect the original controller cable to the base's terminals (B, A, DC 12V, G): a small adapter with a
  matching plug, or the cable's wires directly in the terminals.
- Home Assistant with the **Mosquitto broker** add-on and the **MQTT** integration; a Home Assistant user for the Atom.
- A computer with **arduino-cli** (or the Arduino IDE), ESP32 core **2.0.17**, libraries **M5Atom**,
  **FastLED 3.6.0** (3.10 breaks M5Atom), **PubSubClient**. Give the exact commands for their operating system.

## Step 3: firmware (on the desk)

1. `cp smartAircon/secrets.example.h smartAircon/secrets.h`, they fill in Wi-Fi, MQTT user/password, broker IP.
2. Compile and upload via USB (commands in the README). The device **"Climatizzatore (controller RS485)"** must appear
   in Home Assistant (Settings → Devices → MQTT) before going on.
3. Add the climate entity from `homeassistant/climate.yaml` to `configuration.yaml` (under `mqtt:`) and restart Home
   Assistant. Adjust `min_temp` / `max_temp` to their unit.

## Step 4: wiring

1. With the wall controller still connected and working, measure on its cable: **12 V between +12 V and GND**. Note
   which wire is which (A and B cannot be told apart with a multimeter; that is fine).
2. Disconnect the wall controller. Connect the cable to the RS485 base: GND → G, +12 V → DC 12V, A → A, B → B.
   Double-check +12 V and GND before powering: a reversed supply can damage the base.
3. Power on. In Home Assistant check `sensor.climatizzatore_controller_rs485_risposte_ricevute` increasing (hundreds
   per minute) and `…_risposte_con_crc_errato` staying near 0.
4. If no replies arrive: swap A and B (harmless) and try again.

## Step 5: first test

1. Read the decoded sensors with the unit off: room temperature, coil temperatures. They should be plausible.
2. Turn it on from the Home Assistant climate entity in **fan only**, then **cool** or **heat**. Check that
   `…_acceso_secondo_la_macchina`, `…_compressore` and `…_ventola_interna_reale` follow.
3. In heat mode the fan stays off for about 3 minutes after the compressor starts (hot start): it is normal.
4. If the unit does not react, or behaves differently from the README: switch on **Registra trama** (capture), repeat,
   and help them read the capture in `/config/smartaircon/catture/` (needs `homeassistant/pyscript/`). If the unit
   misbehaves, set **Intervallo invio comandi** back to its default (1000 ms) or put the original controller back.

## Step 6: wrap up

Close the electrical box, switch the breaker back on. Remind them that OTA has no password (keep the Atom on a
trusted network), how to update over the air, and that the set point restarts from 24 °C after a reboot until Home
Assistant sends a command.
