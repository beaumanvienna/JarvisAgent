# engine troubleshooting guide

## table of contents
- [code 244](#code-244)
- [code 250](#code-250)
- [code 301](#code-301)

## introduction
This troubleshooting guide is used to repair the vehicle.Usage: read out the engine codes and follow the troubleshooting steps.

## code 244

```mermaid
flowchart TD
    A[Start: Code 244 'engine temperature' active?] --> B{Is Code 244 active?}
    B -- No --> Z[End]
    B -- Yes --> C{Is Code 245 'low cooling liquid' present?}
    C -- Yes --> D[Follow instructions for Code 245]
    C -- No --> E{Is radiator clogged?}
    E -- Yes --> F[Clean radiator]
    F --> Z
    E -- No --> G{Does cooling pump run?}
    G -- Yes --> Z
    G -- No --> H{Is circuit breaker in?}
    H -- Yes --> I[Check and fix wiring]
    I --> J[Put circuit breaker back in]
    J --> K{Does cooling pump work now?}
    K -- Yes --> Z
    K -- No --> L[Replace cooling pump]
    L --> Z
    H -- No --> I
```

## code 250

```mermaid
flowchart TD
    A[Check engine code] --> B{Is code 250 present?}
    B -- Yes --> C[Adjust wheel alignment per Procedure 5 in tire manual]
    B -- No --> D[Check other engine codes or issues]
    C --> E[Verify tire wear is even]
    E --> F[End]
    D --> F
```

## code 301

```mermaid
flowchart TD
    A[Code 301 Present: Headlights Light Circuit Breaker Tripped] --> B[Check Headlight Wiring]
    B --> C{Wiring Faulty or Shorted?}
    C -- Yes --> D[Fix Wiring]
    D --> E[Put Circuit Breaker Back In and Switch On Headlights]
    C -- No --> E
    E --> F{Breaker Trips Again?}
    F -- No --> G[Done]
    F -- Yes --> H[Disconnect Left Light]
    H --> I[Put Circuit Breaker Back In and Switch On Lights]
    I --> J{Breaker Stays In?}
    J -- Yes --> K[Replace Left Light]
    J -- No --> L[Replace Right Light]
    K --> G
    L --> G
```
