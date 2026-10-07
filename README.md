# AfriCAN DETECT IT: Marine Proton-Precession Magnetometer Towfish

This project is a towed underwater magnetometer for marine magnetic surveying and object recovery. It's dragged behind a boat and maps small changes in Earth's magnetic field, the kind made by shipwrecks, debris, and other metal objects on or under the seafloor.

The goal is a low-cost instrument with **sub-1 nT sensitivity**, every reading tagged with **GPS position**, and everything packed into a **waterproof towfish** that works in the field.

## How it works

The system is a proton-precession magnetometer (PPM). A coil is wrapped around a container of hydrogen-rich fluid, such as water or kerosene.

1. **Polarize.** A large DC current through the coil sets up a strong field that lines up the proton spins in the fluid.
2. **Release.** The current is cut quickly. The protons start to precess around Earth's field.
3. **Sense.** The precessing protons induce a weak signal in the same coil, in the microvolt range and decaying over a few seconds.
4. **Measure.** The precession frequency is directly proportional to field strength (about 42.58 Hz per µT, which is roughly 2 kHz in Earth's field). Counting that frequency precisely gives an absolute field reading.
5. **Log.** Each reading is timestamped and paired with a GPS fix, so a survey becomes a map of the magnetic field.

The method is attractive because the measurement comes from a physical constant, the proton's gyromagnetic ratio. It doesn't drift and needs no calibration. The hard part is pulling a tiny, decaying signal out of noise right after a high-current pulse.

## System design

```
 ┌────────────┐   ┌──────────┐   ┌──────────────┐   ┌──────────────┐   ┌─────────┐
 │Polarization│──▶│ Sensor   │──▶│ Blanking +   │──▶│ Filtering &  │──▶│   MCU   │
 │  driver    │   │  coil    │   │   preamp     │   │    gain      │   │ freq.   │
 └────────────┘   └──────────┘   └──────────────┘   └──────────────┘   │ counter │
                                                                       └────┬────┘
                                                          GPS ─────────────▶│
                                                                            ▼
                                                                    Logged survey data
```

| Subsystem | Role |
|---|---|
| **Polarization driver** | Switches high current into the coil, then shuts it off fast and cleanly |
| **Sensor coil** | Polarizes the fluid, then picks up the precession signal |
| **Blanking + preamp** | Shields the sensitive front end during turn-off, then amplifies the weak signal with as little added noise as possible |
| **Signal conditioning** | Band-pass filtering and gain centered on the expected precession frequency |
| **Frequency measurement** | MCU-based period counting for precise frequency estimates over a short, decaying signal |
| **GPS integration** | Position and timing for every reading |
| **Isolation** | Keeps switching noise and ground currents from the power side out of the analog chain |
| **Towfish housing** | Waterproof, non-magnetic enclosure that keeps the sensor stable and away from the boat's own magnetic signature |

## Our approach

We're building this as a set of independent subsystems. Each one is designed, built, and tested on its own before integration. That lets us confirm each stage works and track down problems without guessing which part of the chain is at fault.

**1. Design for the signal.** The precession signal is tiny and short-lived, so every design choice starts from the noise budget: coil geometry and wire, preamp noise, filter bandwidth, and how fast the front end can recover after the polarizing pulse.

**2. Prototype and test on the bench.** Each subsystem gets its own custom PCB and bench test. We check polarization switching, front-end blanking and gain, coil pickup, and frequency and GPS logging separately against known inputs.

**3. Fight noise and ringing.** The biggest challenges are coil ringing after turn-off, environmental and switching noise, and weak-signal recovery. We handle these with fast, well-damped switching, careful blanking, narrow filtering, shielding, and isolating the high-current side from the analog side.

**4. Integrate in stages.** Subsystems are combined one at a time, and we confirm performance at each step before adding the next.

**5. Package and field test.** The finished electronics go into the towfish. We test it in the water, first over open water and then over known magnetic targets, to verify sensitivity and positioning in real survey conditions.

## Repo layout

```
hardware/     Schematics and PCB layouts for each subsystem
firmware/     MCU code for timing, frequency measurement, and GPS logging
analysis/     Scripts for processing survey data and making field maps
mechanical/   Towfish housing design files
docs/         Design notes, test results, and bring-up procedures
```

## Safety

The polarization stage switches high current into an inductive load. Read the bring-up notes in `docs/` before powering the hardware, and confirm the blanking and flyback paths are in place first.

## Team

**AfriCAN DETECT IT**, Electrical & Computer Engineering, University of Wisconsin–Madison.
