# engine troubleshooting guide

## table of contents
- [Code 244 Engine Temperature](#code-244)
- [Code 250 — Tire Alignment](#code-250)
- [Code 301 — Headlights Circuit Breaker](#code-301)

## introduction
This troubleshooting guide is used to repair the vehicle. Usage: read out the engine codes and follow the troubleshooting steps.

## Code 244 Engine Temperature

```mermaid
flowchart TD
    A["Check if code 244 engine temperature active"] --> B{"Is code 245 low cooling liquid present"}
    B -->|"Yes"| C["Follow code 245 instructions"]
    B -->|"No"| D{"Is radiator clogged"}
    D -->|"Yes"| E["Clean radiator"] --> F["Done"]
    D -->|"No"| G{"Does cooling pump run"}
    G -->|"Yes"| F
    G -->|"No"| H{"Is circuit breaker in"}
    H -->|"Yes"| I["Check and fix wiring"] --> J["Put back circuit breaker"] --> K{"Does cooling pump work now"}
    H -->|"No"| I
    K -->|"Yes"| F
    K -->|"No"| L["Replace cooling pump"] --> F
```

## Code 250 — Tire Alignment

```mermaid
flowchart TD
  A["Check for engine code 250"] --> B{"Is code 250 present?"}
  B -->|"Yes"| C["Inspect tires for uneven wear"]
  C --> D["Adjust wheel alignment"]
  D --> E["Follow procedure 5 in tire manual"]
  E --> F["Recheck tire wear after adjustment"]
  F --> G{"Is tire wear even now?"}
  G -->|"Yes"| H["Issue resolved"]
  G -->|"No"| I["Repeat alignment adjustment"]
  B -->|"No"| J["No alignment needed"]
```

## Code 301 — Headlights Circuit Breaker

```mermaid
flowchart TD
  A["Check for Code 301"] --> B{"Breaker Tripped?"}
  B -->|"No"| C["End — No Issue"]
  B -->|"Yes"| D["Check Headlights Wiring"]
  D --> E{"Wiring Faulty?"}
  E -->|"Yes"| F["Fix Wiring"]
  F --> G["Reset Breaker and Switch On"]
  E -->|"No"| G
  G --> H{"Breaker Trips Again?"}
  H -->|"No"| I["End — Issue Fixed"]
  H -->|"Yes"| J["Disconnect Left Light"]
  J --> K["Reset Breaker and Switch On"]
  K --> L{"Breaker Trips?"}
  L -->|"No"| M["Replace Left Light"]
  L -->|"Yes"| N["Replace Right Light"]
  M --> O["End"]
  N --> O
```
