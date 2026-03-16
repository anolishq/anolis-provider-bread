# Troubleshooting

Common failure modes with their diagnostic signatures and fixes.

## Reading The Logs

All log lines are written to stderr and do not appear on stdout.
The ADPP framed-stdio transport uses stdout exclusively, so logs are always safe to redirect or capture separately.

| Prefix | Stream | Meaning |
|--------|--------|---------|
| `[INFO]` | `stderr` | Normal progress and startup events |
| `[WARN]` | `stderr` | Recoverable failure or degraded state |
| `[ERROR]` | `stderr` | Fatal failure or unrecoverable condition |

---

## Provider Fails To Start

### Config parse error

The provider exits with code 1 before opening the bus.

```
[ERROR] <yaml exception or "required field missing: ..." message>
```

Use `--check-config` to validate before running:

```bash
./anolis-provider-bread --check-config config/example.local.yaml
```

A valid config prints `[INFO] Config valid: ...` and exits with code 0.

### Bus open failure

Hardware builds only (`ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=ON`).
The provider exits with code 1 after loading config.

```
[ERROR] open failed code=TransportError attempts=1 message="failed to open Linux I2C bus '/dev/i2c-1'"
```

Check that the bus path exists and the process has permission:

```bash
ls -la /dev/i2c-*
sudo setfacl -m u:$USER:rw /dev/i2c-1  # or run as root temporarily during dev
```

Confirm `hardware.bus_path` in your config matches the actual device node.

---

## Device Not Responding During Startup

The probe sequence runs once at startup.
A device that fails any probe step is excluded from the inventory.

### Version query fails (timeout or transport error)

```
[ERROR] query_read failed addr=0x08 code=TransportError attempts=3 native_code=-5 message="..."
[WARN]  probe 0x08 version failed: <message>
[WARN]  1 unsupported or incompatible probe(s) excluded from inventory
[WARN]    excluded 0x08 status=VersionReadFailed detail=version query failed: ...
```

The `[ERROR]` line is the raw CRUMBS session failure. The `[WARN]` lines are the higher-level probe summary.

Common causes:
- Wrong I2C address in `discovery.addresses`
- Device not powered or not wired
- `hardware.query_delay_us` too low for the device to prepare its reply

Try increasing `query_delay_us` (default 10 000 µs). On a working device this probe always succeeds.

### Unexpected version reply opcode

```
[WARN] probe 0x08 unexpected version reply opcode 0xNN
```

The device responded but returned an unexpected opcode. Most likely cause is a bus collision or a non-BREAD device at that address.

### Unknown BREAD type ID

```
[INFO] probe 0x08 unsupported type_id 0x05
[WARN]  excluded 0x08 status=UnsupportedType detail=unknown BREAD type_id: 0x05
```

The device probed successfully but its type is not supported by this provider version.
This is not a wiring issue; it is a firmware/software mismatch.
Either update the provider to support the new type or remove the address from the config.

---

## Version Mismatch

Version compatibility is checked after a successful version query.
A mismatch excludes the device from the inventory.

```
[WARN] probe 0x08 compat failed: <detail>
[WARN]  excluded 0x08 status=IncompatibleCrumbsVersion detail=...
```

The `status` field indicates what was incompatible:

| Status | Meaning | Fix |
|--------|---------|-----|
| `IncompatibleCrumbsVersion` | Device firmware uses a CRUMBS version older than the minimum required by `bread-crumbs-contracts` | Update device firmware |
| `IncompatibleModuleMajor` | Module major version mismatch (breaking change) | Update firmware to match the contracts version expected by this provider |
| `IncompatibleModuleMinor` | Module minor version below required minimum | Update firmware |

The exact minimum versions expected are defined in `bread-crumbs-contracts`. See [feast-stack docs/devices.md](../../feast-stack/docs/devices.md) for the version compatibility table.

---

## GET_CAPS Failure — Baseline Fallback

If the capability query (opcode `0x7F`) fails or returns a parse error, the provider falls back to the baseline capability profile for that device type.

```
[WARN] probe 0x08 caps query failed (timeout), using baseline fallback
```

or after a successful caps response that cannot be parsed:

```
[WARN] probe 0x08 caps parse failed, using baseline fallback
```

The device is still added to the inventory. The impact depends on the device type:

| Device | Baseline flags | Practical impact |
|--------|----------------|-----------------|
| RLHT | All flags set | No impact, full function set available |
| DCMT | `OPEN_LOOP \| BRAKE_CONTROL` only | Closed-loop control functions absent from `DescribeDevice` |

For DCMT, falling back to baseline means `set_speed_closed_loop` and related closed-loop functions are not exposed until a successful caps query can confirm the device supports them.

If caps queries fail intermittently, check for I2C bus contention or try increasing `hardware.query_delay_us`.

---

## Expected Device Absent — Provider Degraded

When a config `devices:` entry specifies an `id` and `address` but the device is not found or excluded during startup, the provider enters a degraded state.

```
[WARN] 1 expected device(s) not found in inventory
[WARN]   missing expected device id=dcmt0
```

`GetHealth` returns:

```
provider.state  = STATE_DEGRADED
provider.message = "provider ready but 1 expected device(s) not found"
```

Per missing device:

```
device_health.device_id = "dcmt0"
device_health.state     = STATE_UNREACHABLE
device_health.message   = "expected device not found during startup"
```

`WaitReady` still completes — the provider is technically ready — but `GetHealth` reflects the reduced inventory.

Common causes:
- Hardware not plugged in or not powered
- Mismatch between `address` in config and actual I2C address on the bus
- Device excluded due to a probe failure (check for earlier `[WARN]` or `[ERROR]` lines at the same address)

Fix: correct the `devices:` list in the config or resolve the underlying hardware issue.

---

## Runtime Read And Call Failures

Hardware faults during normal operation are now surfaced on stderr.

### `read_signals` failure

```
[ERROR] query_read failed addr=0x08 code=Timeout attempts=3 native_code=-110 message="..."
[WARN]  read_signals device='rlht0' failed: <message>
```

The ADPP client receives an error status on the `ReadSignals` RPC. The provider stays running.

### `call` failure

```
[ERROR] query_write failed addr=0x09 code=TransportError attempts=3 native_code=-5 message="..."
[WARN]  call device='dcmt0' fn='set_speed_open_loop' failed: <message>
```

The ADPP client receives an error status on the `Call` RPC.

### Common session error codes and fixes

| Code | Meaning | Fix |
|------|---------|-----|
| `Timeout` | Device did not respond within `timeout_ms` | Increase `hardware.timeout_ms`; check device power |
| `TransportError` | I2C bus-level error (EIO, ENXIO, etc.) | Check wiring, check I2C pull-ups, check bus speed |
| `CrcError` | CRUMBS frame CRC mismatch | EMI or loose connection; check I2C signal quality |
| `AlreadyOpen` | Session open called twice | Provider logic error; should not occur in normal use |

Persistent errors from a previously healthy device usually indicate a hardware or wiring fault.
Isolated errors that self-resolve are consistent with I2C bus transients; tuning `query_delay_us` can help.

---

## Checking Runtime Health

The `GetHealth` RPC returns a snapshot. Key fields:

```
provider.metrics["device_count"]           — number of devices in inventory
provider.metrics["degraded"]               — "true" if any expected device missing
provider.metrics["unsupported_probe_count"] — probes excluded at startup
provider.metrics["inventory_mode"]         — "hardware" or "config-seeded"
```

In foundation builds (`ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF`), `inventory_mode` will be `config-seeded` and no I2C activity occurs. All probe-related warnings and errors are hardware-path only.
