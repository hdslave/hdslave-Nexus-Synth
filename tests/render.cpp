// Renders Nexus from a note script on stdin, for listening and for comparing with other renders. Writes interleaved
// stereo float32 (raw) or a WAV (when the name ends in .wav) to OUTPUT and prints the peak and the time it took.
//   render Nexus.clap OUTPUT < script
// The script, a command a line:
//   rate HZ | len FRAMES | block FRAMES | preset KEY | tempo BPM PLAYING
//   set ID VALUE          a parameter's plain value
//   norm ID VALUE         a parameter by its place 0..1 along its range (times and rates as milliseconds and hertz
//                         along a linear range, as some hosts scale them: see plainOf)
//   at FRAME on KEY VEL | off KEY | cc NUMBER VALUE | pat KEY VALUE | cat VALUE | bend VALUE14 | set ID VALUE
#include "wav.h"
#include "params.h"

#include <clap/clap.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
const clap_host host { CLAP_VERSION, nullptr, "render", "", "", "1",
                       [] (const clap_host*, const char*) -> const void* { return nullptr; }, [] (const clap_host*) {},
                       [] (const clap_host*) {}, [] (const clap_host*) {} };

// A value 0..1 along a range to the plain value: envelope times along 0-5 s, glide 0-2 s, the cycle 1 ms-5 s and rates
// 0-40 Hz, linearly; everything else along its own range.
float plainOf (uint32_t id, float v)
{
    using namespace nexus;
    using namespace nexus::ParamId;
    switch (id)
    {
        case env1Attack: case env1Decay: case env1Release: case env2Attack: case env2Decay: case env2Release: return envKnob (v * 5000.0f);
        case glide: return glideKnob (v * 2000.0f);
        case cycRise: case cycFall: return cycleKnob (1.0f + v * 4999.0f);
        case lfo1Rate: case lfo2Rate: case rndRate: return rateKnob (v * 40.0f);
        default: break;
    }
    const Param& p = paramTable[id];
    float x = p.min + v * (p.max - p.min);
    if (p.kind != Kind::continuous) x = std::round (x);
    return x;
}

struct Timed { long frame; clap_event_midi midi; clap_event_param_value param; bool isParam; };
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "render Nexus.clap OUTPUT < script\n"); return 2; }
    void* lib = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) { std::fprintf (stderr, "%s\n", dlerror()); return 1; }
    const auto* entry = static_cast<const clap_plugin_entry*> (dlsym (lib, "clap_entry"));
    entry->init (argv[1]);
    const auto* factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* plugin = factory->create_plugin (factory, &host, "com.hdslave.nexus");
    plugin->init (plugin);
    const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    const auto* presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));

    double rate = 48000.0, bpm = 120.0;
    long length = 48000;
    uint32_t block = 128;
    bool playing = false;
    std::vector<std::pair<uint32_t, float>> sets;
    std::vector<Timed> timed;
    std::string presetKey;
    std::string line;
    while (std::getline (std::cin, line))
    {
        std::istringstream s (line);
        std::string w;
        s >> w;
        if (w == "rate") s >> rate;
        else if (w == "len") s >> length;
        else if (w == "block") s >> block;
        else if (w == "preset") s >> presetKey;
        else if (w == "tempo") { int p; s >> bpm >> p; playing = p != 0; }
        else if (w == "set" || w == "norm")
        {
            uint32_t id; float v;
            s >> id >> v;
            sets.push_back ({ id, w == "norm" ? plainOf (id, v) : v });
        }
        else if (w == "at")
        {
            long f; std::string k;
            s >> f >> k;
            Timed t {};
            t.frame = f;
            int a = 0, b = 0;
            uint8_t status = 0;
            if (k == "on") s >> a >> b, status = 0x90;
            else if (k == "off") s >> a, status = 0x80;
            else if (k == "cc") s >> a >> b, status = 0xb0;
            else if (k == "pat") s >> a >> b, status = 0xa0;
            else if (k == "cat") s >> a, status = 0xd0;
            else if (k == "bend") { int v; s >> v; status = 0xe0; a = v & 127; b = v >> 7; }
            else if (k == "set" || k == "norm")
            {
                uint32_t id; float v;
                s >> id >> v;
                t.isParam = true;
                t.param.header = { sizeof (t.param), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                t.param.param_id = id;
                t.param.note_id = t.param.port_index = t.param.channel = t.param.key = -1;
                t.param.value = k == "norm" ? plainOf (id, v) : v;
                timed.push_back (t);
                continue;
            }
            t.midi.header = { sizeof (t.midi), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
            t.midi.port_index = 0;
            t.midi.data[0] = status, t.midi.data[1] = (uint8_t) a, t.midi.data[2] = (uint8_t) b;
            timed.push_back (t);
        }
    }
    if (! presetKey.empty())
        presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, presetKey.c_str());
    {
        // The settings before the first block, as a host flushes them.
        std::vector<clap_event_param_value> list;
        for (auto& [id, v] : sets)
        {
            clap_event_param_value e {};
            e.header = { sizeof (e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
            e.param_id = id, e.note_id = e.port_index = e.channel = e.key = -1, e.value = v;
            list.push_back (e);
        }
        struct In { std::vector<clap_event_param_value>* l; } in { &list };
        clap_input_events events { &in, [] (const clap_input_events* e) { return (uint32_t) static_cast<In*> (e->ctx)->l->size(); },
                                   [] (const clap_input_events* e, uint32_t i) -> const clap_event_header* { return &(*static_cast<In*> (e->ctx)->l)[i].header; } };
        params->flush (plugin, &events, nullptr);
    }
    plugin->activate (plugin, rate, 1, block);
    plugin->start_processing (plugin);

    std::vector<float> out (2 * (size_t) length), left (block), right (block);
    double seconds = 0.0;
    for (long at = 0; at < length; at += block)
    {
        const uint32_t n = (uint32_t) std::min<long> (block, length - at);
        std::vector<const clap_event_header*> list;
        std::vector<Timed> now;
        for (auto& t : timed)
            if (t.frame >= at && t.frame < at + n) now.push_back (t);
        for (auto& t : now)
        {
            if (t.isParam) t.param.header.time = (uint32_t) (t.frame - at), list.push_back (&t.param.header);
            else t.midi.header.time = (uint32_t) (t.frame - at), list.push_back (&t.midi.header);
        }
        clap_input_events in { &list, [] (const clap_input_events* e) { return (uint32_t) static_cast<std::vector<const clap_event_header*>*> (e->ctx)->size(); },
                               [] (const clap_input_events* e, uint32_t i) { return (*static_cast<std::vector<const clap_event_header*>*> (e->ctx))[i]; } };
        clap_output_events outEvents { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
        clap_event_transport transport {};
        transport.header = { sizeof (transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        transport.tempo = bpm;
        transport.song_pos_beats = (clap_beattime) std::llround ((double) at / rate * bpm / 60.0 * (double) CLAP_BEATTIME_FACTOR);
        float* channels[2] = { left.data(), right.data() };
        clap_audio_buffer buffer { channels, nullptr, 2, 0, 0 };
        clap_process process {};
        process.steady_time = at;
        process.frames_count = n;
        process.transport = &transport;
        process.audio_outputs = &buffer;
        process.audio_outputs_count = 1;
        process.in_events = &in;
        process.out_events = &outEvents;
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process (plugin, &process);
        seconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        for (uint32_t i = 0; i < n; ++i)
            out[2 * (size_t) (at + i)] = left[i], out[2 * (size_t) (at + i) + 1] = right[i];
    }
    const std::string name = argv[2];
    if (name.size() > 4 && name.substr (name.size() - 4) == ".wav")
    {
        std::vector<float> l ((size_t) length), r ((size_t) length);
        for (long i = 0; i < length; ++i) l[(size_t) i] = out[2 * (size_t) i], r[(size_t) i] = out[2 * (size_t) i + 1];
        writeWav (name.c_str(), l, r, (int) rate);
    }
    else
    {
        FILE* f = std::fopen (argv[2], "wb");
        std::fwrite (out.data(), sizeof (float), out.size(), f);
        std::fclose (f);
    }
    double peak = 0.0;
    for (float x : out) peak = std::max (peak, (double) std::fabs (x));
    std::printf ("peak=%.4f cpu=%.1f%%\n", peak, 100.0 * seconds / ((double) length / rate));
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    entry->deinit();
    return 0;
}
