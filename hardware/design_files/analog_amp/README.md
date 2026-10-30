# Analog Amplifier - Proton-Precession Magnetometer (ECE 455)

## Purpose
The analog amplifier conditions the weak precession signal from the sensing coil for downstream frequency measurement. The design targets a signal near 2.4 kHz and interfaces with a 0–3.3 V ADC.

## Architecture
- Sensing-coil/tank input (`TANK`).
- OPA4227 quad op-amp signal conditioning, including preamplification, filtering and additional gain.
- Nominal first-stage non-inverting gain: `1 + 100 kΩ / 1 kΩ = 101 V/V` (not measured system gain).
- ±5 V analog supply and 3.3 V reference input.
- 10 kΩ / 10 kΩ divider nominally generating 1.65 V `VMID`; separate buffered bias output for ADC level shifting.

## Folder guide
- `kicad/`: editable PCB, schematic and project configuration from supplied uploads.
- `bom/placement_bom.csv`: one row per footprint extracted from PCB.
- `bom/grouped_bom.csv`: grouped BOM; manufacturer and supplier part numbers require confirmation.
- `docs/`: hardware test log and manufacturing instructions.
- `manufacturing/gerbers/`: **no Gerbers included**; export from verified PCB in KiCad.
- `mechanical/stl/`: **no STL included**; no matching 3D CAD source supplied.

## Prototype status (2026-10-30)
PCB assembled and bench-tested with laboratory supply, oscilloscope, multimeter and function generator. ±5 V and 3.3 V supply readings were subsequently reported correct. Reference-node measurements have been inconsistent (nominal VMID +1.65 V; previous readings include about +1.78 V and −2.23 V), and require repeatable probing against PCB ground. End-to-end gain and frequency response have not been validated.




