# Changes made to the vendored sources

- `stmlib/utils/random.h`, `random.cc`: the shared random state is `thread_local` (each Nexus voice saves and restores
  it around its render, so voices are independent and the output is the same however a host threads its plug-ins).
- `plaits/dsp/dsp.h`: `kCorrectedSampleRate` is 48000 Hz. The module's audio clock runs at 47872.34 Hz and the
  firmware tunes to it; Nexus runs at a true 48 kHz, so with the module's value every note was 4.6 cents sharp.
- `plaits/dsp/engine2/six_op_engine.cc`: `SixOpEngine::Reset()` clears `acc_buffer_` (in the module it does nothing).
  The 6-op engines keep the second half of their staggered rendering in that buffer, which lies in the RAM all of a
  voice's engines share, and play it in their next block. After another engine had used that RAM, the first block of
  a 6-op model played what was left there: Particle's diffuser stores 16-bit words, and a small negative sample read
  as the top half of a float is a NaN, which went to the output. After a voice was silenced, it played the tail of
  the note before.
- `stmlib/dsp/*.py` (table generators) were not copied.

Nothing else in `plaits/` or `stmlib/` is changed. Nexus's own adaptation of Plaits' voice is `src/plaits_voice.*`.
