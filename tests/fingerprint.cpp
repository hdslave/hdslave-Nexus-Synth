// The sound's fingerprint: renders fixed scenarios through the CLAP entry, as a host plays them, and prints a 64-bit
// hash of every output sample's bits for each. tests/fingerprint.txt holds the hashes the current sound gives, and
// `make test` fails if one changes: work meant to keep the sound (a speed-up, a clean-up) must leave them all as they
// are. A change meant to change the sound updates the file with `make fingerprint-update`.
//   fingerprint PLUGIN.clap [EXPECTED.txt]
// Scenarios: every factory preset playing, every model, the voice modes, Slide and Tie, the modulators, other rates,
// and the same scene in blocks of 1, 37 and 4096 frames, which must hash as in blocks of 128. Hashes hold for this
// toolchain and these flags (Melodee's build image, -O3 -mcpu=cortex-a55 -ffp-contract=fast): another compiler may
// round differently.
#include "../src/params.h"
#include "../src/presets.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <vector>

namespace
{
using namespace nexus;
const clap_plugin_factory* factory = nullptr;
const clap_host host { CLAP_VERSION, nullptr, "fingerprint", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                       [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };

struct Event { uint64_t frame; uint8_t status, d1, d2; uint32_t id = 0; double value = 0.0; bool param = false; };
Event M (uint64_t f, uint8_t s, uint8_t a, uint8_t b) { return { f, s, a, b }; }
Event On (uint64_t f, uint8_t key, uint8_t vel = 100) { return { f, 0x90, key, vel }; }
Event Off (uint64_t f, uint8_t key) { return { f, 0x80, key, 0 }; }
Event P (uint64_t f, uint32_t id, double v) { Event e { f, 0, 0, 0 }; e.id = id, e.value = v, e.param = true; return e; }

struct Scenario
{
    std::string name, preset;
    std::vector<Event> events;
    double rate = 48000.0, seconds = 3.0, bpm = 120.0;
    uint32_t block = 128;
    bool playing = true;
};

uint64_t fnv (uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t render (const Scenario& s)
{
    const clap_plugin* plugin = factory->create_plugin (factory, &host, "com.hdslave.nexus");
    plugin->init (plugin);
    if (! s.preset.empty())
    {
        const auto* presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        if (! presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, s.preset.c_str()))
            std::printf ("no preset %s\n", s.preset.c_str());
    }
    plugin->activate (plugin, s.rate, 1, 4096);
    plugin->start_processing (plugin);
    struct Any { union { clap_event_header header; clap_event_midi midi; clap_event_param_value param; }; };
    std::vector<Any> list;
    clap_input_events in { &list, [] (const clap_input_events* e) { return (uint32_t) static_cast<std::vector<Any>*> (e->ctx)->size(); },
                           [] (const clap_input_events* e, uint32_t i) -> const clap_event_header* { return &(*static_cast<std::vector<Any>*> (e->ctx))[i].header; } };
    clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
    const uint64_t frames = (uint64_t) (s.seconds * s.rate);
    std::vector<float> l (s.block), r (s.block);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t at = 0; at < frames; at += s.block)
    {
        const uint32_t n = (uint32_t) std::min<uint64_t> (s.block, frames - at);
        list.clear();
        for (const auto& e : s.events)
            if (e.frame >= at && e.frame < at + n)
            {
                Any a {};
                if (e.param)
                    a.param = { { sizeof (clap_event_param_value), (uint32_t) (e.frame - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 },
                                e.id, nullptr, -1, -1, -1, -1, e.value };
                else
                {
                    a.midi.header = { sizeof (clap_event_midi), (uint32_t) (e.frame - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                    a.midi.data[0] = e.status, a.midi.data[1] = e.d1, a.midi.data[2] = e.d2;
                }
                list.push_back (a);
            }
        std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; });
        clap_event_transport tr {};
        tr.header = { sizeof (tr), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (s.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        tr.tempo = s.bpm;
        tr.song_pos_beats = (clap_beattime) ((double) at / s.rate * s.bpm / 60.0 * (double) CLAP_BEATTIME_FACTOR);
        float* channels[2] = { l.data(), r.data() };
        clap_audio_buffer buffer { channels, nullptr, 2, 0, 0 };
        clap_process p {};
        p.steady_time = (int64_t) at;
        p.frames_count = n;
        p.transport = &tr;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = &in;
        p.out_events = &out;
        plugin->process (plugin, &p);
        for (uint32_t i = 0; i < n; ++i) // frame by frame, so the block size does not shape the hash
            h = fnv (h, &l[i], sizeof (float)), h = fnv (h, &r[i], sizeof (float));
    }
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    return h;
}

std::vector<Event> phrase (double rate)
{
    const uint64_t b = (uint64_t) (rate / 4); // eighth notes at 120
    return { On (0, 60, 110), Off (b, 60), On (b, 63, 80), Off (2 * b, 63), On (2 * b, 67, 100), On (2 * b + 7, 70, 60), Off (4 * b, 67),
             Off (4 * b, 70), On (5 * b, 48, 127), On (5 * b, 55, 90), On (5 * b, 64, 70), Off (8 * b, 48), Off (8 * b, 55), Off (8 * b, 64) };
}

std::vector<Scenario> scenarios()
{
    using namespace ParamId;
    std::vector<Scenario> list;
    for (const auto& p : presetTable)
    {
        Scenario s;
        s.name = std::string ("preset-") + p.key, s.preset = p.key, s.events = phrase (s.rate);
        list.push_back (s);
    }
    for (int m = 0; m < modelCount; ++m)
    {
        Scenario s;
        s.name = "model-" + std::to_string (m), s.seconds = 1.5;
        s.events = { P (0, model, m), On (0, 48, 120), On (0, 60, 60), Off (36000, 48), Off (36000, 60) };
        list.push_back (s);
    }
    const uint64_t q = 12000;
    auto add = [&] (const char* name, std::vector<Event> e, double rate = 48000.0) {
        Scenario s;
        s.name = name, s.rate = rate, s.events = std::move (e);
        list.push_back (s);
    };
    add ("mono", { P (0, voiceMode, VoiceMode::mono), P (0, glide, 0.4), On (0, 57), On (q, 64), Off (2 * q, 64), On (3 * q, 69), Off (4 * q, 57), Off (5 * q, 69) });
    add ("legato", { P (0, voiceMode, VoiceMode::legato), P (0, glide, 0.4), P (0, ampMode, Amp::env), On (0, 57), On (q, 64), Off (2 * q, 64),
                     On (3 * q, 69), Off (4 * q, 57), Off (5 * q, 69) });
    add ("slide-tie", { P (0, model, 0), On (0, 45), M (q, 0xb0, 84, 45), On (q, 52), Off (q, 45), M (2 * q, 0xb0, 68, 127), M (2 * q, 0xb0, 84, 52),
                        On (2 * q, 57), Off (2 * q, 52), M (2 * q, 0xb0, 68, 0), Off (4 * q, 57) });
    add ("unison", { P (0, unison, 3), P (0, detune, 0.6), P (0, spread, 0.8), P (0, model, 13), On (0, 50), On (q, 57), Off (5 * q, 50), Off (5 * q, 57) });
    add ("retrigger", { P (0, trigRate, 5), P (0, ampMode, Amp::ping), On (0, 60), Off (8 * q, 60) });
    add ("modulators", { P (0, model, 9), P (0, route (0, 2), 0.7), P (0, lfo1Shape, LfoShape::smooth), P (0, route (1, 1), -0.5),
                         P (0, route (2, 3), 0.6), P (0, cycBipolar, 0), P (0, route (3, 0), 0.3), P (0, rndMode, RndMode::smooth),
                         P (0, lfo2PerVoice, 1), P (0, lfo2Sync, 1), P (0, assign (0, 0), 0.6), P (0, assign (1, 2), 0.5), P (0, assign (2, 3), 1),
                         P (0, assign (3, 1), 0.4), On (0, 55), M (q, 0xb0, 1, 90), M (q, 0xa0, 55, 70), M (2 * q, 0xd0, 100, 0), On (2 * q, 62),
                         M (3 * q, 0xe0, 0, 100), Off (5 * q, 55), Off (5 * q, 62) });
    add ("filters", { P (0, filterMode, 0), P (0, cutoff, 0.4), P (0, resonance, 0.7), On (0, 48), P (q, filterMode, 1), P (2 * q, filterMode, 2),
                      P (3 * q, cutoff, 0.8), Off (5 * q, 48) });
    add ("amp-drone", { P (0, ampMode, Amp::drone), P (0, env1Attack, 0.3), On (0, 52), Off (3 * q, 52) });
    add ("rate-44100", phrase (44100.0), 44100.0);
    add ("rate-96000", phrase (96000.0), 96000.0);
    add ("rate-22050", phrase (22050.0), 22050.0);
    // Chiptune with a trigger, its keys held: recorded from 0.1.7, before Nexus closed its notes once let go.
    add ("chiptune-held-gate", { P (0, model, 7), P (0, trigRate, 5), On (0, 60), On (0, 64) });
    add ("chiptune-held-env", { P (0, model, 7), P (0, ampMode, Amp::env), P (0, trigRate, 3), P (0, timbreAtt, 0.3), On (0, 57) });
    add ("chiptune-held-aux", { P (0, model, 7), P (0, auxMix, 0.5), P (0, harmonics, 0.7), P (0, timbreAtt, -0.4), On (0, 48), On (q, 55) });
    for (uint32_t block : { 128u, 1u, 37u, 4096u })
    {
        Scenario s;
        s.name = "blocks-" + std::to_string (block), s.block = block, s.rate = 44100.0;
        s.events = phrase (44100.0);
        s.events.push_back (P (9000, ParamId::harmonics, 0.9));
        s.events.push_back (M (20000, 0xe0, 0, 80));
        list.push_back (s);
    }
    return list;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: fingerprint PLUGIN.clap [EXPECTED.txt]\n"); return 2; }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    const auto* entry = library ? static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry")) : nullptr;
    if (entry == nullptr || ! entry->init (argv[1])) { std::printf ("FAIL cannot load %s\n", argv[1]); return 1; }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    std::map<std::string, std::string> expected;
    if (argc > 2)
        if (FILE* f = std::fopen (argv[2], "r"))
        {
            char name[128], hash[32];
            while (std::fscanf (f, "%127s %31s", name, hash) == 2) expected[name] = hash;
            std::fclose (f);
        }
    int changed = 0, missing = 0, blockBreaks = 0;
    std::string blocks128;
    for (const auto& s : scenarios())
    {
        char hash[32];
        std::snprintf (hash, sizeof (hash), "%016llx", (unsigned long long) render (s));
        std::printf ("%s %s\n", s.name.c_str(), hash);
        if (s.name == "blocks-128") blocks128 = hash;
        else if (s.name.rfind ("blocks-", 0) == 0 && blocks128 != hash)
            ++blockBreaks, std::fprintf (stderr, "FAIL fingerprint: %s differs from blocks-128: the block size changes the sound\n", s.name.c_str());
        if (argc > 2)
        {
            const auto it = expected.find (s.name);
            if (it == expected.end()) ++missing, std::fprintf (stderr, "fingerprint: %s is new (%s)\n", s.name.c_str(), hash);
            else if (it->second != hash) ++changed, std::fprintf (stderr, "FAIL fingerprint: %s changed (%s, was %s)\n", s.name.c_str(), hash, it->second.c_str());
        }
    }
    entry->deinit();
    if (changed || blockBreaks || (argc > 2 && missing)) return 1;
    return 0;
}
