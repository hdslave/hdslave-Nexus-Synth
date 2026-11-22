// Loads Nexus.clap as a host does and checks it: description and ports, parameters and their texts, state, presets,
// tuning at several host rates (the 48 kHz engine resampled), the same output whatever the block size, voices (notes,
// unison and its detune, the eight-voice limit), Mono, Legato, glide, Melodee's Slide and Tie, MIDI and the channel
// modes, the modulation depths, the four amp modes, the retrigger rate, every model, bounded output at extreme
// settings, sleeping when silent, and the screen page. Writes demo renders (WAV) and the page's pictures (PNG) into the
// output folder.
//   host Nexus.clap OUT_FOLDER
#include "wav.h"
#include "png.h"
#include "../src/melodee_screen.h"
#include "../src/params.h"
#include "../src/presets.h"

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::fabs (va - vb) <= (tol))) { ++failures; \
    std::printf ("FAIL %s:%d  %s = %g, %s = %g (tolerance %g)\n", __FILE__, __LINE__, #a, va, #b, vb, (double) (tol)); } } while (0)

using namespace nexus;
const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

int rescans = 0, paints = 0, processRequests = 0, presetsLoaded = 0, presetErrors = 0, flushes = 0;
const void* hostExtension (const clap_host*, const char* id)
{
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) { rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0; },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushes; } };
    static const clap_host_melodee_screen screen { [] (const clap_host*) { ++paints; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, CLAP_EXT_MELODEE_SCREEN_HOST) == 0) return &screen;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "Nexus test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++processRequests; }, [] (const clap_host*) {},
};

struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    void midi (uint32_t time, uint8_t status, uint8_t d1, uint8_t d2)
    {
        Any a {};
        a.midi.header = { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        a.midi.port_index = 0, a.midi.data[0] = status, a.midi.data[1] = d1, a.midi.data[2] = d2;
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

// What a test plays: at a frame, a parameter (plain value), a note, or MIDI bytes.
struct Cue
{
    uint64_t frame;
    enum Kind { param, on, off, midi, choke } kind;
    uint32_t a = 0;   // parameter id, key, or MIDI status
    double b = 0.0;   // value, velocity, or MIDI data 1
    int c = 0;        // MIDI data 2
};
Cue P (uint64_t f, uint32_t id, double v) { return { f, Cue::param, id, v, 0 }; }
Cue On (uint64_t f, int key, double vel = 0.8) { return { f, Cue::on, (uint32_t) key, vel, 0 }; }
Cue Off (uint64_t f, int key) { return { f, Cue::off, (uint32_t) key, 0.0, 0 }; }
Cue Midi (uint64_t f, int status, int d1, int d2) { return { f, Cue::midi, (uint32_t) status, (double) d1, d2 }; }

struct Stereo { std::vector<float> l, r; };

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    double rate;
    std::vector<clap_process_status> statuses;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    double value (uint32_t id)
    {
        double v = 0.0;
        params->get_value (plugin, id, &v);
        return v;
    }
    bool preset (const char* key) { return presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key); }

    Stereo render (uint64_t frames, const std::vector<Cue>& cues = {}, uint32_t block = 128, bool playing = false, double bpm = 120.0)
    {
        Stereo out;
        out.l.assign (frames, 0.0f), out.r.assign (frames, 0.0f);
        statuses.clear();
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = bpm;
            tr.song_pos_beats = (clap_beattime) std::llround ((double) at / rate * bpm / 60.0 * (double) CLAP_BEATTIME_FACTOR);
            Events e;
            for (const auto& c : cues)
                if (c.frame >= at && c.frame < at + n)
                {
                    const uint32_t t = (uint32_t) (c.frame - at);
                    switch (c.kind)
                    {
                        case Cue::param: e.param (t, c.a, c.b); break;
                        case Cue::on: e.note (t, CLAP_EVENT_NOTE_ON, (int16_t) c.a, c.b); break;
                        case Cue::off: e.note (t, CLAP_EVENT_NOTE_OFF, (int16_t) c.a, 0.0); break;
                        case Cue::choke: e.note (t, CLAP_EVENT_NOTE_CHOKE, (int16_t) c.a, 0.0); break;
                        case Cue::midi: e.midi (t, (uint8_t) c.a, (uint8_t) c.b, (uint8_t) c.c); break;
                    }
                }
            e.sort();
            float* outs[2] = { out.l.data() + at, out.r.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_output_events none { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            p.out_events = &none;
            const auto status = plugin->process (plugin, &p);
            CHECK (status != CLAP_PROCESS_ERROR);
            statuses.push_back (status);
        }
        return out;
    }
};

// --- Analysis ---------------------------------------------------------------------------------------------------------
double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    to = std::min (to, x.size());
    for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
    return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
}
double peak (const std::vector<float>& x)
{
    double p = 0.0;
    for (float v : x) p = std::max (p, (double) std::fabs (v));
    return p;
}
bool finite (const std::vector<float>& x)
{
    for (float v : x) if (! std::isfinite (v)) return false;
    return true;
}
// The frequency of a sine over [from, to), from its rising zero crossings.
double frequency (const std::vector<float>& x, double rate, size_t from, size_t to)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    to = std::min (to, x.size());
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
        {
            const double t = (double) (i - 1) + (double) -x[i - 1] / (double) (x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            last = t, ++count;
        }
    return count > 1 ? (count - 1) * rate / (last - first) : 0.0;
}
double cents (double hz, double ref) { return 1200.0 * std::log2 (hz / ref); }
double semis (double hz) { return 12.0 * std::log2 (hz / 440.0); }
// The level of frequency `hz` in [from, to) (a Hann-windowed single-bin DFT).
double level (const std::vector<float>& x, double rate, double hz, size_t from, size_t to)
{
    std::complex<double> acc = 0.0;
    const size_t n = std::min (to, x.size()) - from;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) n);
        acc += w * (double) x[from + i] * std::polar (1.0, -2.0 * M_PI * hz * (double) i / rate);
    }
    return std::abs (acc) * 4.0 / (double) n;
}
// A pure sine voice: FM 2-op with no modulation, the gate bypassed (Drone), the envelope at full.
std::vector<Cue> sine (std::vector<Cue> more = {})
{
    using namespace ParamId;
    std::vector<Cue> c = { P (0, model, 10), P (0, timbre, 0), P (0, morph, 0.5), P (0, ampMode, Amp::drone), P (0, env1Sustain, 1),
                           P (0, env1Attack, 0) };
    c.insert (c.end(), more.begin(), more.end());
    return c;
}

void run (const char* name, const std::function<void()>& test)
{
    const int before = failures;
    test();
    std::printf ("%-12s %s\n", name, failures == before ? "ok" : "FAILED");
}

// --- Tests ------------------------------------------------------------------------------------------------------------
void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (std::strcmp (d->id, "com.hdslave.nexus") == 0);
    CHECK (std::strcmp (d->name, "hdslave Nexus") == 0);
    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, false) == 1 && audio->count (a.plugin, true) == 0);
    clap_audio_port_info ai {};
    CHECK (audio->get (a.plugin, 0, false, &ai) && ai.channel_count == 2);
    clap_note_port_info ni {};
    CHECK (notes && notes->get (a.plugin, 0, true, &ni) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI));
    CHECK (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN) != nullptr);
}

void testParams()
{
    Instance a;
    CHECK (a.params->count (a.plugin) == (uint32_t) paramCount);
    std::set<std::string> names;
    for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (info.id == i);                   // ids are the table's places, fixed forever
        CHECK (names.insert (info.name).second); // names unique
        CHECK (info.default_value >= info.min_value && info.default_value <= info.max_value);
        CHECK_NEAR (a.value (info.id), info.default_value, 0.0);
        // Texts read back (at five places along the range).
        for (int k = 0; k <= 4; ++k)
        {
            const Param& p = paramTable[i];
            double v = info.min_value + (info.max_value - info.min_value) * k / 4.0;
            if (p.kind != Kind::continuous) v = std::round (v);
            char text[64] = {};
            CHECK (a.params->value_to_text (a.plugin, info.id, v, text, sizeof (text)));
            double back = -99.0;
            const bool parsed = a.params->text_to_value (a.plugin, info.id, text, &back);
            CHECK (parsed);
            // Texts are rounded for the eye: within a step of what they show.
            const double tolerance = p.kind != Kind::continuous ? 0.0 : std::max (0.02 * (info.max_value - info.min_value), 0.006);
            if (p.unit != Unit::gain || v > 0.0)
            {
                if (! (std::fabs (back - v) <= tolerance))
                    std::printf ("  text \"%s\" of %s (%g) read back as %g\n", text, info.name, v, back);
                CHECK (std::fabs (back - v) <= tolerance);
            }
        }
    }
    // Some texts as a person types them.
    double v = 0.0;
    CHECK (a.params->text_to_value (a.plugin, ParamId::model, "Bass Drum", &v) && v == 21);
    CHECK (a.params->text_to_value (a.plugin, ParamId::env1Attack, "1 s", &v) && std::fabs (envMs ((float) v) - 1000.0f) < 1.0f);
    CHECK (a.params->text_to_value (a.plugin, ParamId::cutoff, "1 kHz", &v) && std::fabs (cutoffHz ((float) v) - 1000.0f) < 1.0f);
    CHECK (a.params->text_to_value (a.plugin, ParamId::pan, "L50", &v) && v == -0.5);
    CHECK (! a.params->text_to_value (a.plugin, ParamId::harmonics, "nan", &v));
    char text[64];
    CHECK (a.params->value_to_text (a.plugin, ParamId::frequency, -12, text, sizeof (text)) && std::string (text) == "-12 st");
    CHECK (a.params->value_to_text (a.plugin, ParamId::volume, 1, text, sizeof (text)) && std::string (text) == "+0.0 dB");
    // Defaults as the plug-in Nexus follows starts: Virtual Analog, Gate, Poly with four notes.
    CHECK (paramTable[ParamId::model].def == 8 && paramTable[ParamId::ampMode].def == Amp::gate && paramTable[ParamId::notes].def == 4);
    CHECK_NEAR (envMs (paramTable[ParamId::env1Decay].def), 200.0, 0.01);
}

struct Stream
{
    std::vector<uint8_t> bytes;
    size_t at = 0;
    clap_ostream out { this, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
        auto* self = static_cast<Stream*> (s->ctx);
        self->bytes.insert (self->bytes.end(), (const uint8_t*) d, (const uint8_t*) d + n);
        return (int64_t) n;
    } };
    clap_istream in { this, [] (const clap_istream* s, void* d, uint64_t n) -> int64_t {
        auto* self = static_cast<Stream*> (s->ctx);
        const uint64_t take = std::min<uint64_t> (n, self->bytes.size() - self->at);
        std::memcpy (d, self->bytes.data() + self->at, take);
        self->at += take;
        return (int64_t) take;
    } };
};

void testState()
{
    using namespace ParamId;
    Stream s;
    {
        Instance a;
        a.render (256, { P (0, model, 5), P (0, harmonics, 0.25), P (0, route (2, 3), -0.5), P (0, assignTarget0 + 2, Target::pan) });
        CHECK (a.state->save (a.plugin, &s.out));
    }
    {
        Instance b;
        CHECK (b.state->load (b.plugin, &s.in));
        CHECK (b.value (model) == 5 && b.value (harmonics) == 0.25 && b.value (route (2, 3)) == -0.5 && b.value (assignTarget0 + 2) == Target::pan);
        Stream bad;
        bad.bytes = { 'N', 'O', 'P', 'E', 1, 0, 0, 0, 0, 0, 0, 0 };
        CHECK (! b.state->load (b.plugin, &bad.in));
        // An unknown id is skipped, a missing one keeps its default.
        Stream odd;
        const char tag[4] = { 'N', 'X', 'U', 'S' };
        const uint32_t header[2] = { 1, 2 };
        const struct { uint32_t id; float value; } pairs[2] = { { 9999, 1.0f }, { morph, 0.75f } };
        odd.bytes.insert (odd.bytes.end(), tag, tag + 4);
        odd.bytes.insert (odd.bytes.end(), (const uint8_t*) header, (const uint8_t*) header + 8);
        odd.bytes.insert (odd.bytes.end(), (const uint8_t*) pairs, (const uint8_t*) pairs + 16);
        CHECK (b.state->load (b.plugin, &odd.in));
        CHECK (b.value (morph) == 0.75 && b.value (model) == 8);
    }
}

void testPresets (const std::string& folder)
{
    // Discovery lists every preset, keys unique.
    const auto* pf = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (pf != nullptr && pf->count (pf) == 1);
    std::set<std::string> keys;
    for (const auto& p : presetTable) CHECK (keys.insert (p.key).second);
    CHECK (presetCount == 25);
    for (const auto& p : presetTable)
    {
        Instance a;
        CHECK (a.preset (p.key));
        // A phrase: a note, then a chord.
        const auto out = a.render (96000, { On (0, 60, 0.8), Off (24000, 60), On (28800, 48, 0.86), On (28800, 55, 0.7), On (28800, 64, 0.8),
                                             Off (67200, 48), Off (67200, 55), Off (67200, 64) },
                                   128, true);
        CHECK (finite (out.l) && finite (out.r));
        CHECK (rms (out.l, 0, 96000) > 1e-3);
        CHECK (peak (out.l) < 1.5);
        writeWav ((folder + "/preset-" + p.key + ".wav").c_str(), out.l, out.r, 48000);
    }
    Instance a;
    CHECK (a.preset ("filter-bass") && a.value (ParamId::model) == 0 && a.value (ParamId::frequency) == -12 && a.value (ParamId::voiceMode) == VoiceMode::legato);
    const int errors = presetErrors;
    CHECK (! a.preset ("no-such-preset") && presetErrors == errors + 1);
}

// A4 is 440 Hz at any host rate (the engine runs at 48 kHz and is resampled), and a semitone is a semitone.
void testTuning()
{
    for (double rate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        Instance a (rate);
        const auto out = a.render ((uint64_t) rate, sine ({ On (0, 69, 1.0) }));
        const double hz = frequency (out.l, rate, (size_t) (0.2 * rate), (size_t) rate);
        if (std::fabs (cents (hz, 440.0)) > 0.3) std::printf ("  %.0f Hz: A4 at %.3f Hz\n", rate, hz);
        CHECK (std::fabs (cents (hz, 440.0)) < 0.3);
        const auto high = a.render ((uint64_t) rate, sine ({ On (0, 81, 1.0) }));
        CHECK (std::fabs (cents (frequency (high.l, rate, (size_t) (0.2 * rate), (size_t) rate), 880.0)) < 0.3);
    }
    // Frequency moves pitch; a high note keeps its level through the resampler.
    Instance b (44100.0);
    const auto up = b.render (44100, sine ({ P (0, ParamId::frequency, 7), On (0, 69, 1.0) }));
    CHECK (std::fabs (semis (frequency (up.l, 44100.0, 8820, 44100)) - 7.0) < 0.01);
    Instance c (48000.0), d (44100.0);
    const auto bright = d.render (44100, sine ({ On (0, 120, 1.0) })); // 8.4 kHz
    const auto direct = c.render (48000, sine ({ On (0, 120, 1.0) }));
    CHECK_NEAR (rms (bright.l, 8820, 44100) / rms (direct.l, 9600, 48000), 1.0, 0.01);
}

// The same output whatever the host's block size (and the host rate's own: events land on the engine's 48 kHz clock).
void testBlocks()
{
    using namespace ParamId;
    auto scene = [] (double rate, uint32_t block) {
        Instance a (rate);
        const uint64_t s = (uint64_t) rate;
        return a.render (2 * s, { P (0, model, 13), P (0, lfo1Rate, 0.5), P (0, route (0, 2), 0.6), P (0, voiceMode, VoiceMode::poly),
                                   On (s / 10, 60, 0.9), On (s / 10 + 37, 64, 0.6), P (s / 3, harmonics, 0.9), Midi (s / 2, 0xe0, 0, 90),
                                   On (s / 2 + 5, 67), Off (s, 60), Midi (s + 3, 0xb0, 1, 100), Off (3 * s / 2, 64), Off (3 * s / 2, 67) },
                         block, true);
    };
    for (double rate : { 48000.0, 44100.0 })
    {
        const auto ref = scene (rate, 128);
        CHECK (rms (ref.l, 0, ref.l.size()) > 1e-3);
        for (uint32_t block : { 1u, 37u, 64u, 4096u })
        {
            const auto other = scene (rate, block);
            bool same = other.l == ref.l && other.r == ref.r;
            if (! same) std::printf ("  rate %.0f block %u differs from block 128\n", rate, block);
            CHECK (same);
        }
    }
}

void testVoices()
{
    using namespace ParamId;
    // Unison 2 at full detune: two tones 30 cents either side.
    {
        Instance a;
        const auto out = a.render (96000, sine ({ P (0, unison, 2), P (0, detune, 1), P (0, spread, 0), On (0, 69, 1.0) }));
        const double low = 440.0 * std::pow (2.0, -30.0 / 1200.0), high = 440.0 * std::pow (2.0, 30.0 / 1200.0);
        const double l = level (out.l, 48000.0, low, 9600, 96000), h = level (out.l, 48000.0, high, 9600, 96000);
        const double centre = level (out.l, 48000.0, 440.0, 9600, 96000);
        CHECK (l > 0.02 && h > 0.02 && std::fabs (l - h) < 0.2 * l);
        CHECK (centre < 0.3 * l);
    }
    // Spread puts unison voices apart: hard left and right at full spread.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, unison, 2), P (0, detune, 1), P (0, spread, 1), On (0, 69, 1.0) }));
        const double l = level (out.l, 48000.0, 440.0 * std::pow (2.0, -30.0 / 1200.0), 9600, 48000);
        const double r = level (out.r, 48000.0, 440.0 * std::pow (2.0, -30.0 / 1200.0), 9600, 48000);
        CHECK (l > 0.02 && r < 0.05 * l);
    }
    // Notes: four at most by default; a fifth lets the oldest go.
    {
        Instance a;
        std::vector<Cue> c = sine ({ P (0, env1Release, 0) });
        const int keys[5] = { 57, 60, 64, 67, 71 };
        for (int i = 0; i < 5; ++i) c.push_back (On ((uint64_t) i * 4800, keys[i], 1.0));
        const auto out = a.render (48000, c);
        auto hz = [] (int k) { return 440.0 * std::pow (2.0, (k - 69) / 12.0); };
        CHECK (level (out.l, 48000.0, hz (57), 9600, 14400) > 0.02);   // before the fifth
        CHECK (level (out.l, 48000.0, hz (57), 30000, 48000) < 0.002); // after: gone
        for (int i = 1; i < 5; ++i) CHECK (level (out.l, 48000.0, hz (keys[i]), 30000, 48000) > 0.02);
    }
    // Eight voices in all: unison 8 leaves room for one note.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, notes, 8), P (0, unison, 8), P (0, detune, 0.0), P (0, env1Release, 0),
                                                  On (0, 57, 1.0), On (12000, 69, 1.0) }));
        CHECK (level (out.l, 48000.0, 220.0, 36000, 48000) < 0.002);
        CHECK (level (out.l, 48000.0, 440.0, 36000, 48000) > 0.05);
    }
    // A key played again while it rings reuses its voice: never two voices on one pitch.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, env1Release, envKnob (300)), On (0, 69, 1.0), Off (12000, 69), On (14400, 69, 0.5) }));
        CHECK_NEAR (rms (out.l, 24000, 48000) / rms (out.l, 4800, 12000), 0.5, 0.05);
    }
}

void testMonoLegato()
{
    using namespace ParamId;
    const double s = 48000.0;
    // Legato: a key over a held one glides there with no new attack; back to the held key on release.
    {
        Instance a;
        const auto out = a.render (72000, sine ({ P (0, voiceMode, VoiceMode::legato), P (0, glide, glideKnob (50)), P (0, ampMode, Amp::ping),
                                                  P (0, lpgDecay, 0.9), On (0, 57, 1.0), On (24000, 69, 1.0), Off (48000, 69) }));
        CHECK_NEAR (semis (frequency (out.l, s, 12000, 23000)), -12.0, 0.02);
        CHECK_NEAR (semis (frequency (out.l, s, 40000, 47000)), 0.0, 0.03);
        CHECK_NEAR (semis (frequency (out.l, s, 60000, 71000)), -12.0, 0.05);
        CHECK (rms (out.l, 24000, 25200) < rms (out.l, 22000, 23200) * 1.1); // no new strike
    }
    // Mono: a new key strikes again.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, voiceMode, VoiceMode::mono), P (0, ampMode, Amp::ping), P (0, lpgDecay, 0.6),
                                                  On (0, 57, 1.0), On (24000, 69, 1.0) }));
        CHECK (rms (out.l, 24600, 25800) > 2.0 * rms (out.l, 22000, 23200));
    }
    // Glide: a one-pole of the glide time (time constant), from the held key.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, voiceMode, VoiceMode::legato), P (0, glide, glideKnob (100)),
                                                  On (0, 57, 1.0), On (12000, 69, 1.0) }));
        const double at = semis (frequency (out.l, s, 12000 + 4800 - 480, 12000 + 4800 + 480)); // one time constant in
        CHECK_NEAR (at, -12.0 * std::exp (-1.0), 0.6);
    }
    // Melodee's Slide: CC 84 names the held key; the next key carries its voice on (Glide, or 60 ms) with no strike.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, ampMode, Amp::ping), P (0, lpgDecay, 0.9), On (0, 57, 1.0),
                                                  Midi (24000, 0xb0, 84, 57), On (24000, 69, 1.0), Off (24000, 57) }));
        CHECK_NEAR (semis (frequency (out.l, s, 36000, 48000)), 0.0, 0.05);
        CHECK (rms (out.l, 24000, 25200) < rms (out.l, 22000, 23200) * 1.1);
        CHECK (semis (frequency (out.l, s, 24480, 25440)) < -3.0); // still on its way
    }
    // Tie: CC 68 held over it, the pitch jumps at once.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, ampMode, Amp::ping), P (0, lpgDecay, 0.9), On (0, 57, 1.0), Midi (24000, 0xb0, 68, 127),
                                                  Midi (24000, 0xb0, 84, 57), On (24000, 69, 1.0), Off (24000, 57), Midi (24000, 0xb0, 68, 0) }));
        CHECK_NEAR (semis (frequency (out.l, s, 24960, 26400)), 0.0, 0.05);
        CHECK (rms (out.l, 24000, 25200) < rms (out.l, 22000, 23200) * 1.1);
    }
}

void testMidi()
{
    using namespace ParamId;
    // MIDI notes, bend (range in semitones), the mod wheel and pressure as sources.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, bendRange, 12), Midi (0, 0x90, 69, 127), Midi (24000, 0xe0, 127, 127) }));
        CHECK_NEAR (semis (frequency (out.l, 48000.0, 4800, 23000)), 0.0, 0.02);
        CHECK_NEAR (semis (frequency (out.l, 48000.0, 30000, 48000)), 12.0 * 8191.0 / 8192.0, 0.02);
    }
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, assignTarget0 + 3, Target::pitch), P (0, assign (3, 3), 1.0), Midi (0, 0x90, 69, 127),
                                                  Midi (24000, 0xb0, 1, 127) }));
        CHECK_NEAR (semis (frequency (out.l, 48000.0, 30000, 48000)), pitchRange * depth (1.0f), 0.02);
    }
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, assignTarget0 + 2, Target::pitch), P (0, assign (2, 2), 1.0), P (0, atCurve, -1.0),
                                                  Midi (0, 0x90, 69, 127), Midi (24000, 0xa0, 69, 64) }));
        CHECK_NEAR (semis (frequency (out.l, 48000.0, 30000, 48000)), pitchRange * depth (1.0f) * std::pow (64.0 / 127.0, 4.0), 0.02);
    }
    // The channel modes at 0: 120 cuts the sound, 123 lets keys go; neither moves a parameter.
    for (int cc : { 120, 123 })
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, env1Release, envKnob (20)), On (0, 60, 1.0), Midi (24000, 0xb0, (uint8_t) cc, 0) }));
        CHECK (rms (out.l, 12000, 24000) > 0.01);
        CHECK (rms (out.l, 36000, 48000) < 1e-4);
        for (uint32_t id = 118; id < 128; ++id) CHECK (a.value (id) == paramTable[id].def);
    }
    // A choke stops its key at once.
    {
        Instance a;
        auto c = sine ({ P (0, env1Release, envKnob (2000)), On (0, 60, 1.0), On (0, 64, 1.0) });
        c.push_back ({ 24000, Cue::choke, 60, 0.0, 0 });
        const auto out = a.render (48000, c);
        CHECK (level (out.l, 48000.0, 261.63, 30000, 48000) < 0.002);
        CHECK (level (out.l, 48000.0, 329.63, 30000, 48000) > 0.02);
    }
    // Sustain holds released keys until it lifts.
    {
        Instance a;
        const auto out = a.render (72000, sine ({ P (0, env1Release, 0), Midi (0, 0xb0, 64, 127), On (0, 60, 1.0), Off (12000, 60),
                                                  Midi (48000, 0xb0, 64, 0) }));
        CHECK (rms (out.l, 24000, 47000) > 0.01);
        CHECK (rms (out.l, 60000, 72000) < 1e-4);
    }
}

void testModulation()
{
    using namespace ParamId;
    const double s = 48000.0;
    // A square LFO at full depth on pitch: two octaves either way (the attenuverter's curve: 0.9975 at full).
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, lfo1Shape, LfoShape::square), P (0, lfo1Rate, rateKnob (1)), P (0, lfo1Retrig, 1),
                                                  P (0, route (0, 0), 1.0), On (0, 69, 1.0) }));
        CHECK_NEAR (semis (frequency (out.l, s, 2400, 21600)), 24.0 * depth (1.0f), 0.05);
        CHECK_NEAR (semis (frequency (out.l, s, 26400, 45600)), -24.0 * depth (1.0f), 0.05);
        // Half the amount reaches a quarter of the depth.
        CHECK_NEAR (depth (0.5f), 0.5 * 0.45 * 1.05, 1e-6);
    }
    // A route switched off does nothing.
    {
        Instance a;
        const auto out = a.render (24000, sine ({ P (0, route (0, 0), 1.0), P (0, route (0, 0) + 1, 0), On (0, 69, 1.0) }));
        CHECK_NEAR (semis (frequency (out.l, s, 2400, 24000)), 0.0, 0.01);
    }
    // Velocity to pitch through the assign matrix.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, assign (1, 0), 1.0), P (0, assignTarget0, Target::pitch), On (0, 69, 0.5) }));
        CHECK_NEAR (semis (frequency (out.l, s, 9600, 48000)), 24.0 * depth (1.0f) * 0.5, 0.02);
    }
    // The cycle: unipolar rises from 0 to the depth over Rise, falls over Fall.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, cycRise, cycleKnob (250)), P (0, cycFall, cycleKnob (250)), P (0, cycBipolar, 0),
                                                  P (0, cycRetrig, 1), P (0, route (2, 0), 1.0), On (0, 69, 1.0) }));
        CHECK_NEAR (semis (frequency (out.l, s, 11400, 12600)), 24.0 * depth (1.0f), 0.6);
        CHECK (semis (frequency (out.l, s, 23400, 24000)) < 1.0);
    }
    // Env 2 on pitch: sustain holds, release falls.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, env2Decay, envKnob (20)), P (0, env2Sustain, 0.5), P (0, route (1, 0), 1.0), On (0, 69, 1.0),
                                                  Off (24000, 69) }));
        CHECK_NEAR (semis (frequency (out.l, s, 14400, 24000)), 12.0 * depth (1.0f), 0.1);
    }
}

void testAmp()
{
    using namespace ParamId;
    auto run = [] (int amp, std::vector<Cue> more = {}) {
        Instance a;
        std::vector<Cue> c = { P (0, model, 8), P (0, ampMode, amp), On (0, 60, 1.0), Off (48000, 60) };
        c.insert (c.end(), more.begin(), more.end());
        return a.render (96000, c).l;
    };
    // Gate: open while held, closes on release.
    const auto gate = run (Amp::gate);
    CHECK (rms (gate, 40000, 47000) > 0.5 * rms (gate, 2000, 9000));
    CHECK (rms (gate, 84000, 96000) < 0.05 * rms (gate, 2000, 9000));
    // Ping: decays while held.
    const auto ping = run (Amp::ping);
    CHECK (rms (ping, 40000, 47000) < 0.2 * rms (ping, 500, 3000));
    // Env: follows Env 1 (a slow attack shows).
    const auto env = run (Amp::env, { P (0, env1Attack, envKnob (400)) });
    CHECK (rms (env, 0, 2400) < 0.4 * rms (env, 24000, 30000));
    // Drone: Env 1 as a VCA, no gate: release follows Env 1's.
    const auto drone = run (Amp::drone, { P (0, env1Release, envKnob (100)) });
    CHECK (rms (drone, 40000, 47000) > 0.01);
    CHECK (rms (drone, 70000, 96000) < 0.02 * rms (drone, 40000, 47000));
}

void testRetrigger()
{
    using namespace ParamId;
    // 1/16 at 120 BPM: a strike every 6000 frames while the key is held.
    Instance a;
    const auto out = a.render (48000, { P (0, model, 10), P (0, ampMode, Amp::ping), P (0, lpgDecay, 0.2), P (0, trigRate, 5), On (0, 69, 1.0),
                                        Off (36000, 69) },
                               128, true, 120.0);
    std::vector<float> env (out.l.size());
    float e = 0.0f;
    for (size_t i = 0; i < env.size(); ++i) e = std::max (std::fabs (out.l[i]), e * 0.995f), env[i] = e;
    int strikes = 0;
    for (size_t i = 480; i < 36000; i += 6000)
    {
        const size_t at = i - 480;
        CHECK (env[at + 6000 - 600] < 0.5 * env[at + 1000]);
        ++strikes;
    }
    CHECK (strikes == 6);
}


// Edges a review found: values queued over a reset, NOTES with a key played again while it rings, the pedal's notes
// let go before a held key, notes shorter than a block, a slide off a pedal-held voice, a glide set with its note,
// unison raised under Mono, the filter coming back in.
void testEdges()
{
    using namespace ParamId;
    auto hz = [] (int k) { return 440.0 * std::pow (2.0, (k - 69) / 12.0); };
    // A value the host flushed is kept across a reset.
    {
        Instance a;
        clap_event_param_value e {};
        e.header = { sizeof (e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = ParamId::frequency, e.note_id = e.port_index = e.channel = e.key = -1, e.value = 12.0;
        const clap_event_param_value* list[1] = { &e };
        clap_input_events in { list, [] (const clap_input_events*) { return 1u; },
                               [] (const clap_input_events* x, uint32_t) { return &static_cast<const clap_event_param_value* const*> (x->ctx)[0]->header; } };
        a.params->flush (a.plugin, &in, nullptr);
        a.plugin->reset (a.plugin);
        const auto out = a.render (24000, sine ({ On (0, 69, 1.0) }));
        CHECK_NEAR (semis (frequency (out.l, 48000.0, 4800, 24000)), 12.0, 0.02);
        CHECK (a.value (ParamId::frequency) == 12.0);
    }
    // NOTES 2: a key played again while it rings counts as a note: the oldest held one lets go.
    for (int u : { 1, 3 })
    {
        Instance a;
        const auto out = a.render (120000, sine ({ P (0, notes, 2), P (0, unison, u), P (0, detune, 0), P (0, env1Release, envKnob (150)),
                                                   On (0, 57, 1.0), On (2400, 60, 1.0), Off (4800, 57), On (4800, 64, 1.0), On (9600, 57, 1.0) }));
        CHECK (level (out.l, 48000.0, hz (60), 96000, 120000) < 0.002); // B let go
        CHECK (level (out.l, 48000.0, hz (57), 96000, 120000) > 0.02);
        CHECK (level (out.l, 48000.0, hz (64), 96000, 120000) > 0.02);
    }
    // Past NOTES the pedal's notes go before a key still held.
    {
        Instance a;
        const auto out = a.render (120000, sine ({ P (0, env1Release, envKnob (100)), On (0, 45, 1.0), Midi (100, 0xb0, 64, 127),
                                                   On (2400, 57, 1.0), Off (3000, 57), On (4800, 60, 1.0), Off (5400, 60), On (7200, 64, 1.0),
                                                   Off (7800, 64), On (9600, 67, 1.0) }));
        CHECK (level (out.l, 48000.0, hz (45), 96000, 120000) > 0.02); // the held key stays
        CHECK (level (out.l, 48000.0, hz (57), 96000, 120000) < 0.002); // the oldest pedal note went
    }
    // A note shorter than a block still sounds, in every amp mode (Drone with an instant attack: its 5 ms one cannot
    // open in a quarter of a millisecond).
    for (int amp : { Amp::gate, Amp::ping, Amp::env, Amp::drone })
    {
        Instance a;
        const auto out = a.render (24000, { P (0, model, 21), P (0, ampMode, amp), P (0, env1Attack, 0), On (100, 48, 1.0), Off (100, 48) });
        if (peak (out.l) < 0.01) std::printf ("  amp %d: a zero-length note is silent\n", amp);
        CHECK (peak (out.l) > 0.01);
    }
    // A pedal-held voice slid to a key that is held is that key's now: the pedal lifting does not end it.
    {
        Instance a;
        const auto out = a.render (72000, sine ({ P (0, env1Release, envKnob (30)), Midi (0, 0xb0, 64, 127), On (0, 57, 1.0), Off (2400, 57),
                                                  Midi (4800, 0xb0, 84, 57), On (4800, 69, 1.0), Off (4800, 57), Midi (24000, 0xb0, 64, 0) }));
        CHECK (level (out.l, 48000.0, 440.0, 48000, 72000) > 0.02);
    }
    // A glide set at the same time as a legato note is the note's.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ P (0, voiceMode, VoiceMode::legato), On (0, 57, 1.0), P (12000, glide, glideKnob (200)),
                                                  On (12000, 69, 1.0) }));
        const double at = semis (frequency (out.l, 48000.0, 12000 + 2400, 12000 + 3360));
        CHECK (at < -6.0); // still on its way 50 ms in
    }
    // Mono: unison raised while keys are held; back to the first key, every voice plays at its velocity.
    {
        Instance a;
        const auto one = a.render (96000, sine ({ P (0, voiceMode, VoiceMode::mono), On (0, 69, 1.0) }));
        Instance b;
        const auto two = b.render (120000, sine ({ P (0, voiceMode, VoiceMode::mono), On (0, 69, 1.0), On (2400, 72, 1.0), P (4800, unison, 2),
                                                   Off (9600, 72) }));
        CHECK (rms (two.l, 24000, 120000) > 1.2 * rms (one.l, 24000, 96000));
    }
    // The filter comes back in from where the sound is: no step when cutoff leaves fully open.
    {
        Instance a;
        const auto out = a.render (48000, sine ({ On (0, 69, 1.0), P (24000, cutoff, 0.99) }));
        double normal = 0.0, at = 0.0;
        for (size_t i = 12001; i < 20000; ++i) normal = std::max (normal, (double) std::fabs (out.l[i] - out.l[i - 1]));
        for (size_t i = 24001; i < 26000; ++i) at = std::max (at, (double) std::fabs (out.l[i] - out.l[i - 1]));
        CHECK (at < 1.5 * normal);
    }
}

// Every model plays, finite and bounded, each at its loudest settings too.
void testModels (const std::string& folder)
{
    using namespace ParamId;
    std::vector<float> allL, allR;
    for (int m = 0; m < modelCount; ++m)
    {
        Instance a;
        const auto out = a.render (48000, { P (0, model, m), On (0, 48, 0.9), On (0, 55, 0.9), Off (24000, 48), Off (24000, 55) }, 128, true);
        CHECK (finite (out.l) && finite (out.r));
        if (rms (out.l, 0, 24000) < 1e-4) std::printf ("  model %d (%s) is silent\n", m, modelChoices[m]);
        CHECK (rms (out.l, 0, 24000) > 1e-4);
        CHECK (peak (out.l) < 1.5);
        allL.insert (allL.end(), out.l.begin(), out.l.end());
        allR.insert (allR.end(), out.r.begin(), out.r.end());
        Instance hot;
        const auto loud = hot.render (48000, { P (0, model, m), P (0, harmonics, 1), P (0, timbre, 1), P (0, morph, 1), P (0, resonance, 1),
                                               P (0, cutoff, 0.6), P (0, volume, 2), P (0, unison, 8), P (0, notes, 1), P (0, detune, 0),
                                               P (0, spread, 0), P (0, fmAtt, 1), P (0, timbreAtt, 1), P (0, route (0, 0), 1), On (0, 36, 1.0),
                                               On (100, 108, 1.0) });
        CHECK (finite (loud.l) && finite (loud.r));
        CHECK (peak (loud.l) < 64.0);
    }
    writeWav ((folder + "/models.wav").c_str(), allL, allR, 48000);
}

// Silent: the host may let it sleep; sounding: not.
void testSleep()
{
    Instance a;
    a.render (4800, { On (0, 60, 1.0) });
    for (auto s : a.statuses) CHECK (s == CLAP_PROCESS_CONTINUE);
    a.render (4800, { Off (0, 60) });
    a.render (96000, {});
    CHECK (a.statuses.back() == CLAP_PROCESS_SLEEP);
}

// The page: every page drawn, the wheel and Push edit, K1-K3 follow the control picked, nothing drawn while nothing
// changes, frames while voices sound.
void testScreen (const std::string& folder)
{
    using namespace ParamId;
    Instance a;
    const auto* screen = static_cast<const clap_plugin_melodee_screen*> (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN));
    std::vector<uint32_t> pixels (320 * 218, 0);
    melodee_screen_canvas_t canvas {};
    canvas.pixels = pixels.data();
    canvas.width = 320, canvas.height = 218, canvas.stride = 320;
    canvas.accent = 0xff3ee6ff;
    for (auto& k : canvas.knobs) k.position = -1, k.engaged = true;
    uint64_t now = 1000;
    auto paint = [&] (bool whole = false) { canvas.whole = whole, canvas.now_ms = now; return screen->paint (a.plugin, &canvas); };
    auto shot = [&] (const std::string& name) { paint (true); CHECK (writePng ((folder + "/screen-" + name + ".png").c_str(), pixels.data(), 320, 218, 320, 2)); };
    auto in = [&] (uint32_t kind, int value, bool shift = false) { melodee_screen_input_t e { kind, value, shift }; return screen->input (a.plugin, &e); };
    auto knobs = [&] () { std::vector<clap_id> ids (3); screen->knobs (a.plugin, ids.data()); return ids; };

    CHECK (paint (true));
    CHECK (! paint()); // nothing changed
    const auto k = knobs();
    CHECK (k[0] == model && k[1] == ParamId::frequency && k[2] == harmonics);
    // Wheel to HARMONICS, Push, turn: the value moves and the host hears of it.
    in (MELODEE_SCREEN_WHEEL, 2);
    CHECK (knobs()[0] == harmonics);
    const int before = rescans;
    in (MELODEE_SCREEN_PUSH, 0);
    in (MELODEE_SCREEN_WHEEL, 10);
    CHECK_NEAR (a.value (harmonics), 0.5 + 10.0 / 128.0, 1e-6);
    CHECK (rescans > before);
    in (MELODEE_SCREEN_PUSH, 0); // done
    // A switch flips on Push.
    in (MELODEE_SCREEN_NUDGE, 1), in (MELODEE_SCREEN_NUDGE, 1);  // ENV
    in (MELODEE_SCREEN_WHEEL, 4);                                // RTRG 1
    CHECK (knobs()[0] == env1Retrig);
    in (MELODEE_SCREEN_PUSH, 0);
    CHECK (a.value (env1Retrig) == 0);
    // A matrix: Shift + Push turns a route off.
    in (MELODEE_SCREEN_NUDGE, 1), in (MELODEE_SCREEN_NUDGE, 1), in (MELODEE_SCREEN_NUDGE, 1); // MOD
    CHECK (knobs()[0] == route (0, 0));
    in (MELODEE_SCREEN_PUSH, 0, true);
    CHECK (a.value (route (0, 0) + 1) == 0);
    in (MELODEE_SCREEN_NUDGE, -1), in (MELODEE_SCREEN_NUDGE, -1), in (MELODEE_SCREEN_NUDGE, -1), in (MELODEE_SCREEN_NUDGE, -1),
        in (MELODEE_SCREEN_NUDGE, -1);
    CHECK (knobs()[0] == harmonics);

    // Every page, silent and then sounding (a chord on the Wavetable Pad).
    static const char* const names[] = { "plaits", "voice", "env", "lfo", "cycle", "mod", "assign" };
    a.preset ("wavetable-pad");
    a.render (4800, { P (0, route (0, 0) + 1, 1), P (0, harmonics, 0.2), P (0, assign (0, 0), 0.4), P (0, assign (1, 2), -0.3) });
    for (int p = 0; p < 7; ++p)
    {
        shot (std::string (names[p]) + "-idle");
        in (MELODEE_SCREEN_NUDGE, 1);
    }
    a.render (48000, { On (0, 60, 0.9), On (0, 64, 0.8), On (0, 67, 0.7), On (0, 71, 0.6) });
    for (int p = 0; p < 7; ++p)
    {
        now += 50;
        CHECK (paint()); // sounding: a new frame
        shot (names[p]);
        in (MELODEE_SCREEN_NUDGE, 1);
    }
    // The legend with Shift.
    canvas.shift = true;
    shot ("legend");
    canvas.shift = false;
    // Silent again: no more frames.
    a.render (4800, { Off (0, 60), Off (0, 64), Off (0, 67), Off (0, 71) });
    a.render (480000, {});
    paint();
    now += 100;
    CHECK (! paint());
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host Nexus.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    entry = library ? static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry")) : nullptr;
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    const std::string folder = argv[2];
    run ("description", testDescription);
    run ("params", testParams);
    run ("state", testState);
    run ("presets", [&] { testPresets (folder); });
    run ("tuning", testTuning);
    run ("blocks", testBlocks);
    run ("voices", testVoices);
    run ("mono", testMonoLegato);
    run ("midi", testMidi);
    run ("modulation", testModulation);
    run ("amp", testAmp);
    run ("retrigger", testRetrigger);
    run ("edges", testEdges);
    run ("models", [&] { testModels (folder); });
    run ("sleep", testSleep);
    run ("screen", [&] { testScreen (folder); });
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
