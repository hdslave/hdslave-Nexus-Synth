# Third-party code and type

| Material | Origin and use |
|---|---|
| Plaits DSP | Mutable Instruments' Plaits, by Emilie Gillet, MIT licence: the 24 synthesis models, the internal decay envelope, the low-pass gate and the output stage, from [pichenettes/eurorack](https://github.com/pichenettes/eurorack) (commit in `third_party/eurorack/SOURCE.txt`). `src/plaits_voice.*` adapts the module's voice to a plug-in (float output, no flash user data). [Licence](licenses/LICENSE-MIT-mutable-instruments.txt) · [What changed](third_party/eurorack/NOTES.md) |
| stmlib | Emilie Gillet's DSP and utility library, MIT licence, from [pichenettes/stmlib](https://github.com/pichenettes/stmlib) at the commit eurorack pins. [Licence](licenses/LICENSE-MIT-mutable-instruments.txt) |
| CLAP headers | The CLAP plug-in API, MIT licence. [Licence](third_party/clap/LICENSE) |
| Departure Mono | Copyright 2022-2024 Helena Zhang. SIL Open Font License 1.1. The page's glyphs (`src/ui/font.h`). [Licence](licenses/OFL-departure-mono.txt) |
| `src/melodee_screen.h` | Melodee screen SDK by Kerem Kiliç (Ellic Studio), [MIT licence](licenses/MIT-melodee-screen.txt). |

"Plaits" and "Mutable Instruments" are named only to say where the code comes from; Nexus is not made or endorsed
by Mutable Instruments.
