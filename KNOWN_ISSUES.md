# Known issues

## Periodic ~100 ms host-side audio stall (~once per 10 s)

**Symptom**: brief audible glitch (distorted, slightly stuttering) every
~10 seconds during steady-state emulation. Most reproducible on Zelda
OoA's title-screen idle loop because the GB itself is doing nothing.

**Where it isn't**: confirmed not on our side. The GB's CPU PC stays
in the idle loop (`pc=0x0984 bank=63`) during both healthy and glitch
windows. Emulator pacing holds 59.7 Hz steady. No firmware-side drops.
The HPF added to `push_audio` cleaned up the deterministic clicks
that DID come from binjgb's u8-zero-silence convention; what remains
is purely host-side.

**Where it is**: RVVM's `sound-hda` stream worker periodically stops
draining the BDL for ≥100 ms. The guest's ring inflight (frames
queued for the host) shows the pattern clearly:

```
healthy → inflight=[829..1716]   drops=0
healthy → inflight=[841..1712]   drops=0
healthy → inflight=[829..1700]   drops=0
GLITCH  → inflight=[808..4399]   drops=2   ← host stopped, ring filled
after   → inflight=[3537..4396]  drops=0   ← stays nearly full
after   → inflight=[3525..4383]  drops=0   ← still elevated
```

The stall fires on the host's own cadence (independent of GB state);
it just *looks* deterministic in-game because Zelda's title-loop
phase is stable and the same PC is running each time the host hiccups.

**Suspected cause**: PipeWire's ALSA shim periodically re-arming or
re-quanting the graph, which blocks `snd_pcm_writei` in RVVM's
`alsa_sound_write` for the duration. Or wall-clock pacing drift in
`sound_hda_stream_drain` accumulating until xrun → `snd_pcm_prepare`
→ silent restart.

**To diagnose** (when picking this back up): patch RVVM's
`sound_hda_stream_drain` to time `subsystem.write` and log warns
when it exceeds (say) 50 ms. Splits "is it RVVM's pacing" vs "is it
the host backend" cleanly.

**Mitigation if needed before fix**: bump `PCM_RING_FRAMES`
(currently 4400 = 100 ms at 44.1 kHz) up to 8800 (200 ms). Costs
~100 ms more A/V latency.

**Couldn't file upstream**: pufit/RVVM has issues disabled and I'm
not an admin; LekKit/RVVM (upstream parent) doesn't have the
relevant `sound-hda` code in full. Parking here.
