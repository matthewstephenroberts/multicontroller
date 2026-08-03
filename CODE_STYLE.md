# Code Style & Comment Guidelines

This document describes the style and commenting conventions used in MultiController.

## Philosophy

**Comments explain WHY, not WHAT.** Code should be self-documenting through clear naming; comments explain design decisions, constraints, and non-obvious behavior.

❌ **Bad comment (what):**
```c
// Initialize the sensor
sensor_init();

// Loop through drivers
for (int i = 0; i < n_drivers; i++) {
```

✅ **Good comment (why):**
```c
// Sensors are initialized before buses so bus probes work immediately during scan.
sensor_init();

// Try each driver in registration order; the first to probe successfully wins
// (generic fallback is always last, so every type string gets a match).
for (int i = 0; i < n_drivers; i++) {
```

## File headers

Every source file starts with a **one-line summary** + blank line, describing the module's role:

```c
// sensor.c — driver registry + read/describe dispatch.
#include ...
```

```tsx
// App.tsx — Main UI shell, tab routing, and device connection state.
import ...
```

No multi-line comment blocks at the top. Keep it short.

## Function documentation

**Public functions** (declared in `.h` or exported in `.ts`) get a comment describing their purpose and any non-obvious constraints:

```c
// Read a configured sensor via its driver, locking the I2C bus to prevent
// concurrent mux conflicts with bus_scan. Non-fatal errors (e.g., sensor disconnected)
// return a partial or zero count; esp_err_t is only for catastrophic failures.
esp_err_t sensor_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count);
```

**Private functions** only need comments if they're complex or have side effects:

```c
// Recursively apply the poll floor to each configured sensor, accounting for
// multi-channel reads like AS7341 (fresh frame per two polls) that need doubling.
static void apply_polling_floors(void) {
```

## Inline comments

**Use sparingly.** Only when the logic itself is non-obvious:

```c
// Each PaHub channel uses address 0x70 + mux_id. IDs 0–7 are unique; ID 8 is
// reserved (would collide with channel 0 of the next PaHub in a chain).
uint8_t pfahub_addr = 0x70 + cfg->mux_id;
```

**Don't explain what the code does:**

```c
// ❌ Bad — the code is already clear
i++;  // Increment i

// ✅ If needed, explain why:
i++;  // Skip the generic fallback; it's always last in the registry
```

## Comments on constraints

When a limit or constant is unintuitive, explain it:

```c
// MAX_DRIVERS is sized for 18 named drivers + 1 generic fallback + 5 headroom.
// Previous default of 16 caused silent registry overflow (last-added drivers
// were dropped). See sensor_registry_add() for fallback behavior.
#define MAX_DRIVERS 24
```

## Firmware (C) style

- **K&R bracing** (opening brace on same line):
  ```c
  if (condition) {
      // code
  }
  ```

- **Function names:** snake_case, module_action form: `sensor_read()`, `bus_i2c_lock()`
- **Private static functions:** single-line comments for simple ones, longer for complex
- **Error checking:** Use `ESP_ERROR_CHECK()` for fatal failures; log + return for recoverable
  ```c
  if (bus_i2c_init() != ESP_OK) {
      ESP_LOGW(TAG, "I2C init failed");
      return ESP_FAIL;
  }
  ```

## Web app (TypeScript/React) style

- **Function names:** camelCase
- **Component names:** PascalCase
- **Comments:** Explain hooks, state management, or non-obvious prop drilling
  ```tsx
  // Inline edit mode for sensor name — avoids an extra dialog/form layer
  const [editing, setEditing] = useState(false);
  
  // Mirror the user's draft independently so blur/escape can cancel cleanly
  const [draft, setDraft] = useState(name);
  ```

- **No JSDoc comments** on components (React DevTools shows prop types), unless the logic is particularly tricky

## Files that need comments

Add comments to:
- **Module entry points** (main.c, App.tsx) — boot flow
- **New sensor drivers** — how they probe and read
- **Complex state machines** (BLE connect/reconnect logic)
- **Workarounds for hardware quirks** — explain the bug and the fix

## When NOT to comment

- Variable names that are clear (`poll_ms`, `device_name`)
- Loop bodies that are self-evident (`sensor_read()` calls in a scheduler)
- Standard error handling patterns (`if (!ptr) return ESP_ERR_NO_MEM`)

## Consistency checks

Before committing:

- [ ] File has a one-line header? (non-generated, non-third-party files)
- [ ] Public functions have a comment explaining their purpose?
- [ ] Complex logic has an inline comment explaining WHY it's done that way?
- [ ] No commented-out code blocks? (use git history if you need old code)
- [ ] No multi-line header comment blocks? (keep them short)

## Questions?

If you're unsure whether something needs a comment, imagine explaining it to a smart colleague who hasn't seen the code:
- **Yes, they'd need context** → Add a comment
- **The code is already obvious** → Don't comment it

---

That's it! Good comments make code easy to maintain. Bad comments make it harder. When in doubt, leave it out. 💙
