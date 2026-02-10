# JC Technolabs — Platform Go-Kart Technical Specification

**Document:** Platform Product Datasheet  
**Revision:** 2.1  
**Date:** 2026-01-15  
**Author:** Engineering Department, JC Technolabs

---

## 1. General Performance

### 1.1 Maximum Speed

The platform go-kart achieves a top speed of **55 km/h** on a flat, dry
track surface.  This is governed by the standard 48 V / 8 kW drivetrain
and the factory gear ratio (11:1 final drive).

### 1.2 Acceleration

0–50 km/h is achieved in **4.8 seconds** under standard test conditions
(75 kg driver, dry asphalt, 20 °C ambient).

### 1.3 Kart Weight

Dry weight (without driver): **115 kg**, including battery pack, frame,
and standard bodywork.

### 1.4 Operating Time

Continuous operating time per full charge: **35 minutes** at moderate
track pace (average 40 km/h).  Sprint mode reduces this to approximately
25 minutes.

### 1.5 Operating Temperature Range

Designed operating range: **0 °C to 45 °C** ambient.  Battery management
system limits output below 5 °C and above 42 °C to protect cell life.

## 2. Propulsion and Friction Brakes

### 2.1 Electric Motor

Brushless DC motor, **12 kW peak / 8 kW continuous**, 48 V nominal.
Liquid-cooled housing with integrated controller.

### 2.2 Braking System

- **Front axle:** Hydraulic disc brakes, 200 mm ventilated rotors,
  single-piston floating calipers.
- **Rear axle:** Drum brakes, 160 mm drums, self-adjusting shoes.

All braking components are automotive-grade and meet internal durability
targets (50 000 braking cycles).

### 2.3 Emergency Braking

Emergency stop from 40 km/h: **2.8 meters** (combined hydraulic +
regenerative braking, dry asphalt, 75 kg driver).

### 2.4 Battery and Charging

- Lithium iron phosphate (LiFePO₄), **2.8 kWh** nominal capacity.
- Full charge time: **1.5 hours** via standard 48 V / 20 A charger.
- Integrated BMS with cell balancing, over-temperature, and
  over-current protection.

### 2.5 Regenerative Braking

The drivetrain includes regenerative braking that feeds kinetic energy
back to the battery during deceleration.  Energy recovery rate is
approximately 8 % of total energy expended per lap under typical
conditions.

## 3. Seat and Steering Wheel

### 3.1 Adjustable Seat

The seat rail system accommodates drivers from **150 cm to 195 cm** in
height.  Fore/aft adjustment range: 120 mm.

### 3.2 Safety Harness

A **four-point safety harness** with quick-release buckle is standard
equipment on every kart.

### 3.3 Steering Wheel

- Diameter: **300 mm** (within the 280–320 mm range).
- Quick-release hub (NRG-compatible pattern).
- Polyurethane grip with ergonomic thumb rests.

### 3.4 Lateral Seat Support

The platform seat includes **basic lateral bolsters** moulded into the
fibreglass shell.  These provide adequate support for recreational
driving but are not specifically designed for sustained high-speed
cornering loads.

## 4. Paint and Livery

### 4.1 UV-Resistant Paint

All exterior bodywork is coated with **two-pack polyurethane
automotive-grade paint**, rated for outdoor UV exposure (ISO 11341,
2 000 hours xenon arc).

### 4.2 Logo Placement

Standard livery includes provisions for **company logo placement** on
both side pods and the front nose cone.  Logo areas are flat-masked
during painting to ensure clean adhesion of vinyl decals or direct
print.

### 4.3 Number Panels

**High-visibility number panels** (300 × 200 mm) are mounted on the
front nose and both side pods.  Background colour: white; digits:
black, 150 mm height.

### 4.4 Reflective Elements

The platform kart **does not include reflective strips** as standard.
All current installations are indoor tracks with controlled lighting.

---

## Summary of Key Specifications

| Parameter                  | Platform Value           |
|----------------------------|--------------------------|
| Top speed                  | 55 km/h                  |
| 0–50 km/h                 | 4.8 s                    |
| Dry weight                 | 115 kg                   |
| Operating time             | 35 min                   |
| Ambient temp range         | 0–45 °C                  |
| Motor power (peak)         | 12 kW                    |
| Front brakes               | Hydraulic disc, 200 mm   |
| Rear brakes                | Drum, 160 mm             |
| Emergency stop (40 km/h)   | 2.8 m                    |
| Battery capacity           | 2.8 kWh                  |
| Charge time                | 1.5 h                    |
| Regenerative braking       | Yes                      |
| Seat adjustment range      | 150–195 cm               |
| Harness                    | 4-point, quick-release   |
| Steering wheel             | 300 mm, quick-release    |
| Lateral seat support       | Basic bolsters           |
| Paint                      | UV-resistant, 2K PU      |
| Logo areas                 | Both sides + front nose  |
| Number panels              | Front + both sides       |
| Reflective strips          | Not fitted               |
