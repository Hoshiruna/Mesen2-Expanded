# Genesis Refresh-Delay Parity Note

## Scope

This note covers the refresh-delay slice that was added after the open-bus work.

The goal was not full BlastEm-equivalent prefetch timing. The goal was to stop ignoring refresh entirely and add a backend-level refresh penalty model that matches BlastEm's broad distinction between:

- normal 68K accesses that can incur refresh delay
- VDP / I/O-style accesses that should be treated as refresh-free for the current access

## What Changed

The implementation lives in:

- [GenesisNativeBackend.h](/d:/Mesen2-Expanded/Core/Genesis/GenesisNativeBackend.h)
- [GenesisNativeBackend.cpp](/d:/Mesen2-Expanded/Core/Genesis/GenesisNativeBackend.cpp)

Main additions:

- `_refreshCounterMclk`
- `_refreshLastAccessClock`
- `IsRefreshFreeAccess(...)`
- `GetRefreshWaitStates(...)`

The backend now:

1. Tracks refresh timing against the live 68K execution clock using `GetCurrentExecutionMasterClock()`.
2. Accumulates elapsed master clocks into `_refreshCounterMclk`.
3. Applies a periodic wait-state penalty on non-free accesses.
4. Treats these regions as refresh-free for the current access:
   - I/O registers
   - memory-mode register
   - Z80 bus request / reset registers
   - mapper registers
   - TMSS registers
   - VDP ports
5. Serializes the refresh accumulator state in savestates.

## Current Model

The current approximation is:

- refresh interval: `7 * 128` master clocks
- refresh delay: `2` extra 68K cycles per elapsed interval

That penalty is added through [`CpuBusWaitStates()`](/d:/Mesen2-Expanded/Core/Genesis/GenesisNativeBackend.cpp), not through a deeper instruction scheduler.

## Why This Was Added

Before this change, the Genesis native backend had no refresh-delay model at all.

That left one of the remaining machine-layer gaps identified during the BlastEm comparison:

- BlastEm advances a refresh counter continuously
- BlastEm suppresses refresh delay on certain free-access paths
- our backend previously did neither

This note documents the first corrective step, not a full parity endpoint.

## Limitations

This is still an approximation.

It does **not** currently do the following:

- emulate true prefetch-driven open-bus/refresh interaction
- integrate refresh timing into a fully instruction-exact machine scheduler
- model every special-case access path the same way BlastEm does

So the correct interpretation is:

- refresh timing is now represented
- refresh timing is still coarser than BlastEm

## Expected Next Step

Refresh timing was the last major non-sprite item from the planned parity list.

The next meaningful step is either:

1. targeted verification of the accumulated timing work, or
2. the deferred sprite slot-accuracy rewrite

The recommended order is verification first, because sprite timing is the most invasive remaining change.
