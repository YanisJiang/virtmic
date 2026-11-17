# Final Phase 1 Review — Fix Summary

## Status

**DONE** — Important findings from final Phase 1 review addressed on `feat/phase1-passthrough`.

## Fixes

| # | Finding | Change |
|---|---------|--------|
| 1 | SHM publish ordering | `ShmWriter::write_frame` / `write_silence_frame`: `MemoryBarrier()` then `InterlockedIncrement` on `write_index` so PCM is visible before index publish; brief comments + `shm_layout.hpp` note |
| 2 | Passthrough + `read_frame` fail | `Engine::process_one_`: write silence frame (underrun policy) instead of skipping the tick |
| 3 | Hard-coded VS path | `driver/scripts/build-driver.ps1`: locate MSBuild via `vswhere.exe` |
| 4 | Feeder IOCTL silence | `virtmic-feeder`: log first 16 `DeviceIoControl` failures with `GetLastError` |

Deferred (per review): pipe ACL, admin install.

## Tests

```text
ctest --test-dir build -C Debug --output-on-failure
100% tests passed, 0 tests failed out of 15
```

CMake/ctest: VS 2022 Community bundled path under `D:\Soft\Microsoft\Visual Studio\2022\Community\...`.

## Commit

```text
fix: address Phase 1 Important SHM/engine/driver review findings
```

Branch: `feat/phase1-passthrough` (not pushed).
