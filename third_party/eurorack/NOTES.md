# Changes made to the vendored sources

- `stmlib/utils/random.h`, `random.cc`: the shared random state is `thread_local` (each Nexus voice saves and restores
  it around its render, so voices are independent and the output is the same however a host threads its plug-ins).
- `plaits/dsp/dsp.h`: `kCorrectedSampleRate` is 48000 Hz. The module's audio clock runs at 47872.34 Hz and the
  firmware tunes to it; Nexus runs at a true 48 kHz, so with the module's value every note was 4.6 cents sharp.
- `plaits/dsp/engine2/six_op_engine.cc`: `SixOpEngine::Reset()` (in the module it does nothing) starts the engine
  again as `Init()` left it, without allocating:
  - It clears `acc_buffer_`. The 6-op engines keep the second half of their staggered rendering in that buffer, which
    lies in the RAM all of a voice's engines share, and play it in their next block. After another engine had used
    that RAM, the first block of a 6-op model played what was left there: Particle's diffuser stores 16-bit words, and
    a small negative sample read as the top half of a float is a NaN, which went to the output. After a voice was
    silenced, it played the tail of the note before.
  - It re-initializes both FM voices, the patch quantizer and the active and rendered voice. `LoadUserData()` (called
    with `Reset()` at every model change, and after Nexus silences a voice) unloads the voices' patches, so a voice
    whose note was cut never rendered its gate falling: it kept its gate up and its envelopes at sustain, and the next
    note given to it (the second note after the cut, as the two voices take turns) had no attack and the cut note's
    key and velocity scaling. With some patches that note was silent.
- `stmlib/dsp/*.py` (table generators) were not copied.

Nothing else in `plaits/` or `stmlib/` is changed. Nexus's own adaptation of Plaits' voice is `src/plaits_voice.*`.
