# mf_follow tutorial

This tutorial goes from an empty machine to a voice you can follow, record, loop and play as a groove. Each part builds on the one before. Code is shown as you would write it in a project's `src/user_project.hpp`.

---

## 1. Setup

### Install MayaFlux nightly

The nightly package downloads a rolling release under a fixed version number, so package managers often think it is already up to date. Build it from a clone of the AUR package instead, deleting the cached download each time:

```sh
git clone https://aur.archlinux.org/mayaflux-dev-bin.git
cd mayaflux-dev-bin
rm -f MayaFlux-*-dev-linux-arch.tar.gz*
makepkg --force --cleanbuild
sudo pacman -U mayaflux-dev-bin-*.pkg.tar.zst
```

Do not use `pacman -U --needed`: the version is usually unchanged, so `--needed` skips the newer build. Install the matching `-debug` package too if you want readable backtraces.

Headers land in `/usr/include/MayaFlux`, guides in `/usr/share/MayaFlux/guides`.

### Make a project and add the module

Make a project with Weave. Then add the module:

```sh
cd my_project
git clone https://github.com/kdlydia/mf_follow community/mf_follow
echo mf_follow >> community.cmake
cmake --preset release
cmake --build --preset release --parallel
```

CMake prints `Community module linked: mf_follow` when it is picked up. Weave can also add it, since it is in the community registry.

### Two rules the module follows

1. Nodes are the per-sample path. Buffers and buffer processors are the per-block path. Each feature is written for both, on top of one shared Kinesis function, not one as a wrapper around the other.
2. Nothing in the module includes `API/`. Anything that needs the buffer manager or the scheduler takes it as an argument. In your project you pass the engine's:

```cpp
*get_buffer_manager()      // Buffers::BufferManager&
*get_scheduler()           // Vruta::TaskScheduler&
Config::get_buffer_size()  // frames per block
```

### Microphone input

Turn input on in `settings()`:

```cpp
void settings()
{
    auto& stream = Config::get_global_stream_info();
    stream.input.enabled = true;
    stream.input.channels = 1;
}
```

---

## 2. Envelope: how loud is it

An envelope follower measures loudness with separate attack and release times. Peak mode follows the absolute value; RMS mode follows loudness.

### The kernel

```cpp
namespace D = Kinesis::Discrete;

auto coefs = D::envelope_follow_state(0.01, 0.2, 48000.0, D::EnvelopeMode::RMS);
double level = D::envelope_block(samples, {}, coefs);   // run over a block, keep the last value
double db = D::amplitude_to_db(level);
```

`coefs` holds the settings and the current level. Keep it and pass it again for the next block.

### On a buffer

```cpp
auto follow = std::make_shared<Buffers::EnvelopeProcessor>(0.01, 0.2, D::EnvelopeMode::RMS);
add_processor(follow, my_buffer);

double now = follow->latest();      // from any thread
```

Three output modes:

| Mode | What it does to the buffer |
| ---- | -------------------------- |
| `FEATURE` (default) | Nothing. Read the level with `latest()` or `feature(buffer)`. |
| `SIGNAL` | Replaces the audio with its envelope. |
| `GAIN` | Multiplies the audio by the envelope, or by your gain law. |

Ducking music under a voice:

```cpp
auto duck = std::make_shared<Buffers::EnvelopeProcessor>(
    0.005, 0.3, D::EnvelopeMode::RMS, Buffers::EnvelopeOutput::GAIN);
duck->set_sidechain(voice_buffer);
duck->set_gain_law([](double env) { return env > 0.05 ? 0.25 : 1.0; });
add_processor(duck, music_buffer);
```

### On a node

```cpp
auto follower = std::make_shared<Nodes::Filters::EnvelopeFollower>(source_node, 0.01, 0.2);
follower | Audio[0];
follower->exclude_output_from_graph();   // measure without hearing it

double level = follower->get_last_output();
```

The node and the buffer processor give the same numbers on the same samples.

---

## 3. Formants: which vowel is it

Formants are the resonances of the mouth. F1 and F2 together mostly decide the vowel.

### One microphone, everything at once

`VoiceInput` listens to a microphone and runs a recorder, an envelope follower and a formant tracker on it, in that order:

```cpp
auto voice = Buffers::VoiceInput::listen(*get_buffer_manager(), {
    .sample_rate = static_cast<double>(Config::get_sample_rate()),
    .ring_seconds = 20.0,
});

auto reading = voice->formants()->latest();
if (reading.voiced)
    std::printf("F1 %.0f  F2 %.0f  F3 %.0f\n", reading.f1, reading.f2, reading.f3);

double loud = voice->envelope()->latest();
```

Stay quiet for about a second at the start. The voice gate learns the room's noise floor during that time; `reading.activity.warming` is true until it has.

The microphone does not reach the speakers: the formant tracker clears each block after reading it.

---

## 4. Talk-box: speak through another sound

`FormantTransfer` takes your formants and imposes them on any other sound, called the carrier. A glottal pulse sounds like a voice; anything rich works.

```cpp
auto buzz = std::make_shared<Nodes::Generator::GlottalSource>(110.0F);
auto talk = std::make_shared<Nodes::Network::FormantTransfer>(voice->formants(), buzz,
    Nodes::Network::FormantTransferConfig { .sample_rate = 48000.0 });

talk->relation() | Audio[0];        // decides; makes no sound itself
talk->voice() | Audio[{ 0, 1 }];    // the resonators, which you hear
```

Inside it are two networks:

- A `RelationNetwork` that decides. Each block it reads the latest formants, glides toward them, smooths the loudness, and switches a "voiced" slot on and off.
- A `ResonatorNetwork` that sounds. Three resonators shape the carrier, retuned from the relation's first three slots every block.

Useful controls:

```cpp
talk->set_shift(0.8);                            // all formants x 0.8: a bigger voice
talk->set_glide(0.05);                           // seconds to move between vowels
talk->set_manual({ 300.0, 2300.0, 3000.0 });     // say /i/ with no voice at all
talk->clear_manual();
talk->on_voice([] { /* a voiced sound began */ });
talk->on_voice([] { /* it ended */ }, true);
```

The voiced events run on the audio thread. Keep them short: set a flag or store a number, and act on it elsewhere.

---

## 5. Recording and looping

### Takes

The recorder keeps the last seconds of the microphone in a ring. A take is a copy of part of it.

```cpp
auto recorder = voice->recorder();

recorder->request_take(96000);        // ask for the last two seconds at 48 kHz

// later, for example every frame:
while (auto take = recorder->collect()) {
    // take->stream is a Kakshya::DynamicSoundStream
}
```

The take is cut on the audio thread right after the next block, so asking and collecting are both safe from any thread.

To take exactly one word, mark where it starts and cut when it ends:

```cpp
talk->on_voice([&] { start = recorder->frames(); });
talk->on_voice([&] { word_ended = true; }, true);

// later:
if (word_ended)
    recorder->request_take_since(start, static_cast<uint64_t>(0.12 * 48000));   // with a little before the word
```

### Loops

```cpp
auto looper = std::make_shared<Kriya::Looper>(*get_buffer_manager(), *get_scheduler(), Config::get_buffer_size());

auto slot = looper->add(take->stream, { .speed = 1.0, .level = 0.8 });
looper->speed(*slot, -1.0);         // backwards, carrying on from where it is
looper->level(*slot, 0.5);
looper->remove(*slot);

looper->echo(recorder->ring(), 0.35, 0.4);   // the live voice, 0.35 s late
```

Each loop is a `Kriya::TapSet`. An echo is a tap with a lag: it plays what the ring recorded that long ago.

---

## 6. Grooves: play a word in time

`SliceSequencer` cuts a take into equal chops and plays them on a sixteenth-note clock.

```cpp
auto groove = std::make_shared<Kriya::SliceSequencer>(*get_buffer_manager(), *get_scheduler(),
    Kriya::SliceSequencerConfig { .slices = 4, .steps = 16, .bpm = 100.0, .buffer_size = Config::get_buffer_size() });

groove->set_take(take->stream);
groove->set_hit(0, 0, 1.0);        // chop 1 on step 1, full velocity
groove->set_hit(0, 8, 0.7);        // and on step 9
groove->set_hit(2, 4, 0.8);        // chop 3 on step 5
groove->set_reverse(2, true);      // chop 3 plays backwards
groove->set_pitch(12, 2.0);        // step 13 is an octave up
groove->start();
```

There are two ways to make hits:

- The grid: `set_hit(chop, step, velocity)`. Zero is a rest.
- The queue: `queue({ .slice = 1, .velocity = 0.9, .speed = 1.5 })` from any thread. It plays on the next step, so anything you trigger lands in time.

The clock is a `Kriya::pattern` whose step lengths are worked out again at every bar, so `set_tempo` and `set_swing` take effect at the next bar line. You can call `set_take` while it plays; the new word takes over at once.

### Triggering hits from space

The `perform` example uses `Nexus` to make hits from movement. Each chop is a `Nexus::Emitter` whose position Fabric updates every frame. A `Nexus::Expanse` is a region; when a body enters it, the region's callback queues that chop:

```cpp
auto fabric = std::make_shared<Nexus::Fabric>(*get_scheduler(), *get_event_manager(), *get_buffer_manager(), 0.25F);

fabric->wire(body)
    .every(1.0 / 60.0, Vruta::ProcessingToken::FRAME_ACCURATE)
    .position_from([&] { return current_position; })
    .finalise();

fabric->add_expanse(std::make_shared<Nexus::Expanse>(
    [](const glm::vec3& p) { return p.y > 0.6F; },          // the region
    [&](uint32_t id) { groove->queue({ .slice = chop_of(id) }); },
    [](uint32_t) {}));

// every frame:
fabric->commit();
```

---

## 7. Drawing with Forma

The examples use `Portal::Forma` for everything on screen. Three patterns cover most of it. The snippets assume:

```cpp
using namespace Portal::Forma;
using Role = Kakshya::DataDimension::Role;
using Kakshya::DataModality;
```

A live plot in its own window:

```cpp
auto [plot, surface] = Portal::Forma::plot("Level", 900, 400,
    Plot::source().as("level", 300, Role::SPATIAL_Y, DataModality::TENSOR_ND)
        .from([&](std::vector<double>& s) { /* fill s */ }).build(),
    Plot::series().y(Role::SPATIAL_Y, Plot::AxisRange {}.range(-60.F, 0.F), { glm::vec3(0.3F, 0.7F, 1.F) })
        .as_waveform().done());

schedule_metro(1.0 / 60.0, [plot = std::move(plot)]() mutable { plot.sync(); },
    "plot", Vruta::ProcessingToken::FRAME_ACCURATE);
```

A fader whose value goes to your code:

```cpp
Portal::Forma::Tend<float> fader {
    .form = Portal::Forma::Geometry::horizontal_fader({ { -0.8F, -0.05F }, { 0.8F, 0.05F } }, 0.04F),
    .initial = 0.5F,
};
fader.place(surface).bridge().write([](std::span<const float> v) { /* v[0] is 0..1 */ });
```

A curve you draw, for example 16 steps of velocity:

```cpp
Portal::Forma::Tend<std::vector<float>> lane {
    .form = Portal::Forma::Geometry::drawable_canvas(bounds),
    .initial = std::vector<float>(16, 0.F),
};
lane.place(surface).bridge().write([](std::span<const float> v) { /* each value 0..1 */ });
```

Canvas values run from 0 at the bottom to 1 at the top. For a signed value, treat 0.5 as zero.

---

## 8. Things that cost time

| Symptom | Cause | What to do |
| ------- | ----- | ---------- |
| A processor on an input listener never runs | An input listener's chain only runs while the buffer is on an output channel | Use `VoiceInput::listen` or the `listen()` helpers; they put it there and silence it |
| Recording captures silence | The formant tracker clears the block, and the recorder ran after it | Record first. `VoiceInput` already orders them |
| Takes have gaps | The engine hands an input listener an empty block between microphone deliveries | Skip all-zero blocks (`set_skip_silent_blocks(true)`), on by default in `VoiceInput` |
| Crash in `WindowManager::process()` at start | Windows created from `compose()` race the graphics thread | Build your windows from a frame-clock task with a small delay: `schedule_sequence({ { 0.05, build_ui } }, "ui", Vruta::ProcessingToken::FRAME_ACCURATE)`. A delay of zero runs at once on the calling thread and does not help |
| Crash on quit | Objects that own taps or networks are destroyed after the engine | Keep them alive until the process exits, or destroy them before `MayaFlux::End()` |
| Crash after replacing a take | A sampler was freed while the audio thread still used it | `SliceSequencer` keeps replaced samplers; do the same in your own code |
| Takes many seconds long, or the groove records itself | The microphone hears the speakers, so the voice gate stays open | Use headphones |
| Formants read 15 to 20 percent high on /i/ | F1 of a close vowel falls between harmonics of a low voice | Known limit of the tracker |
| The groove stutters and the CPU is high | Plots redrawn every frame on an integrated GPU | Fewer windows, or sync plots at 30 Hz |

---

## 9. The examples

The `FollowExamples` project has one example per idea. Choose with `MF_EXAMPLE`:

```sh
MF_EXAMPLE=swell   ./build/FollowExamples   # node follower bending a voice's pitch
MF_EXAMPLE=duck    ./build/FollowExamples   # buffer processor ducking music under a voice
MF_EXAMPLE=filter  ./build/FollowExamples   # drawn FIR taps, metered
MF_EXAMPLE=meter   ./build/FollowExamples   # microphone peak and RMS
MF_EXAMPLE=talkbox ./build/FollowExamples   # your formants voice a buzz, Praat-style views
MF_EXAMPLE=draw    ./build/FollowExamples   # draw formant, pitch and loudness tiers
MF_EXAMPLE=loop    ./build/FollowExamples   # say a word, it loops
MF_EXAMPLE=perform ./build/FollowExamples   # a word as a groove: orbits, a strike zone, a grid
```

Read them in that order. Each adds one idea to the ones before.
