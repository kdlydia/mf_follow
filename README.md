# mf_follow

A MayaFlux community module for following a voice or any signal: how loud it is, which formants it has, and what another sound does with them.

It measures loudness sample by sample or block by block, with separate attack and release times, in peak or RMS mode. One Kinesis kernel does the measuring; a buffer processor and a node run it for the two ways MayaFlux processes audio, and they produce exactly the same numbers on the same samples. It tracks the first three formants of a voice with linear prediction and tells speech from steady noise. And it passes those formants through a `RelationNetwork` into a `ResonatorNetwork`, so any carrier speaks with your vowels.

New here? Start with [TUTORIAL.md](TUTORIAL.md).

Needs MayaFlux 1.0.0 or newer. Does not need Lila. Nothing in it reaches the engine through `API/`: what needs a buffer manager takes one.

---

## What is in it

| Piece | Namespace | Kind | What it does |
| ----- | --------- | ---- | ------------ |
| `envelope_follow`, `envelope_block`, `envelope_follow_state` | `Kinesis::Discrete` | kernels | Attack/release follower in the usual Kinesis shape: a `coefs` array holds the parameters and the level. Built on `decay_per_sample`. |
| `amplitude_to_db`, `db_to_amplitude` | `Kinesis::Discrete` | free functions | Decibel conversion. MayaFlux has none. |
| `EnvelopeProcessor` | `Buffers` | `BufferProcessor` | Follows whole audio blocks. Publishes the envelope, replaces the block with it, or applies it as a gain, measured on the buffer itself or on a sidechain buffer. |
| `EnvelopeFollower` | `Nodes::Filters` | `Node` | Follows one sample at a time, in a chain (`source >> follower`) or with an input node, like `IIR`. |
| `levinson_durbin`, `lpc_envelope_db`, `spectral_peaks` | `Kinesis::Discrete` | free functions | Linear prediction from an autocorrelation, the envelope of the resulting filter in dB, and refined peaks of any sampled curve. |
| `VoiceGate` | `Kinesis` | stateful class | Decides frame by frame whether a voice is present or only steady noise, from `Stochastic::Estimate`, `MotionChannel` and `SymbolicTrajectory`. |
| `FormantTracker` | `Kinesis` | stateful class | Frame in, `FormantReading` (F1..F3, voiced, rms) out. No buffers, no threads. |
| `FormantProcessor` | `Buffers` | `BufferProcessor` | Runs a `FormantTracker` on the audio passing through a buffer and publishes readings behind a `Memory::Seqlock`. `listen(manager)` follows a microphone. |
| `GlottalSource` | `Nodes::Generator` | `Generator` | A pulse train with the spectral tilt of a voice: the natural carrier for formants. |
| `RingRecorder` | `Buffers` | `BufferProcessor` | Keeps the last seconds of a buffer in a circular `DynamicSoundStream` (with `append_frames`, so lag taps and `snapshot` see its head) and cuts takes. Requests and takes travel on `Memory::SweepList`; the cut happens on the processing thread. |
| `VoiceInput` | `Buffers` | listener bundle | One microphone through one buffer, in a fixed order: `RingRecorder`, `EnvelopeProcessor`, `FormantProcessor` (which silences last). Takes the `BufferManager`. |
| `Looper` | `Kriya` | `TapSet` owner | Loops takes in numbered slots with per-slot speed (negative is backwards), level and stutter, and echoes a live ring with lag taps. Takes the `BufferManager`, `TaskScheduler` and buffer size. |
| `SliceSequencer` | `Kriya` | `pattern` + `SamplingPipeline` | Cuts a take into chops and plays them on a 16th-note clock from a step grid (velocity per chop and step, pitch per step, reverse per chop) and from a queue any thread can push hits onto, quantised to the next step. Tempo and swing per bar. Takes are swapped live. |
| `FormantTransfer` | `Nodes::Network` | `RelationNetwork` + `ResonatorNetwork` | A talk-box. The relation glides the tracked formants (with `envelope_follow` in `Derive`), smooths the loudness and gates on voicing; the resonators shape any carrier with them. Shift the formants, or draw a vowel by hand. |

Nodes are the per-sample path and buffers are the per-block path. Neither is built on the other: both call the same kernel.

The formant pieces began as the [mf_formant](https://github.com/kdlydia/mf_formant) prototype and are reworked here to the same conventions as the rest of the module.

---

## Structure

```
mf_follow/
    mf_follow.cmake         # CMake module definition
    community.json          # manifest (Weave reads this)
    src/
        Envelope.hpp / .cpp             # envelope kernels, dB
        EnvelopeProcessor.hpp / .cpp    # block envelope follower
        EnvelopeFollower.hpp / .cpp     # per-sample envelope node
        Lpc.hpp / .cpp                  # linear prediction
        VoiceGate.hpp / .cpp            # voice versus steady noise
        FormantTracker.hpp / .cpp       # F1..F3 from a frame
        FormantProcessor.hpp / .cpp     # formants of a buffer or microphone
        GlottalSource.hpp / .cpp        # voice-like carrier
        FormantTransfer.hpp / .cpp      # talk-box: relation into resonators
        RingRecorder.hpp / .cpp         # ring of the recent past, takes
        VoiceInput.hpp / .cpp           # microphone: record, follow, track
        Looper.hpp / .cpp               # TapSet loops and echoes
        SliceSequencer.hpp / .cpp       # chops of a take, played in time
    test/
        CMakeLists.txt
        test_mf_follow.cpp              # envelope
        test_formant.cpp                # formants and talk-box
```

---

## Using it

### Follow a microphone

```cpp
void settings()
{
    auto& stream = MayaFlux::Config::get_global_stream_info();
    stream.input.enabled = true;
    stream.input.channels = 1;
}

void compose()
{
    auto level = Buffers::EnvelopeProcessor::listen(*get_buffer_manager(), 0, 0.005, 0.2);

    schedule_metro(0.1, [level] {
        std::printf("%.1f dB\n", level->latest_db());
    });
}
```

`listen()` takes the `BufferManager` to use, so it never reaches for the engine itself. It makes an input listener buffer on the output channel, because a buffer's processing chain only runs there, and attaches the processor to that buffer's own chain. The processor zeroes each block after measuring it (`set_silence_after`), so the microphone does not reach the speakers. It also skips the empty blocks the engine hands an input listener between microphone deliveries; following those would halve the level.

### Read the level of a buffer that keeps playing

```cpp
auto follow = std::make_shared<Buffers::EnvelopeProcessor>(0.01, 0.1, Kinesis::Discrete::EnvelopeMode::RMS);
add_processor(follow, my_buffer);
// follow->feature(my_buffer), follow->latest(), or follow->set_observer(...)
```

`EnvelopeOutput::FEATURE` (the default) leaves the audio alone. One processor can be attached to several buffers; each keeps its own level.

### Turn a buffer into its envelope

```cpp
auto env = std::make_shared<Buffers::EnvelopeProcessor>(
    0.002, 0.05, Kinesis::Discrete::EnvelopeMode::PEAK, Buffers::EnvelopeOutput::SIGNAL);
add_processor(env, control_buffer);
```

### Duck one buffer under another

```cpp
auto duck = std::make_shared<Buffers::EnvelopeProcessor>(
    0.005, 0.3, Kinesis::Discrete::EnvelopeMode::RMS, Buffers::EnvelopeOutput::GAIN);
duck->set_sidechain(voice_buffer);
duck->set_gain_law([](double env) {
    return Kinesis::map_clamped(env, 0.02, 0.2, 1.0, 0.25);
});
add_processor(duck, music_buffer);
```

The sidechain is read as it stands when the processor runs: this cycle's data if it was processed earlier in the cycle, last cycle's otherwise. Without a gain law the envelope itself is the gain. Set the gain law and the observer before the processor starts running; attack, release, mode, output and sidechain can be changed from any thread at any time.

### Follow a node

```cpp
auto follower = std::make_shared<Nodes::Filters::EnvelopeFollower>(0.01, 0.2);
auto chain = voice >> follower;
chain | Audio[0];
follower->exclude_output_from_graph();

follower->on_tick([](Nodes::Filters::EnvelopeContext& ctx) {
    // ctx.value (envelope), ctx.db, ctx.input
});
```

Or give it an input node: `std::make_shared<Nodes::Filters::EnvelopeFollower>(voice, 0.01, 0.2)`.

### Follow formants and speak through another sound

```cpp
void compose()
{
    auto formants = Buffers::FormantProcessor::listen(*get_buffer_manager());
    auto buzz = std::make_shared<Nodes::Generator::GlottalSource>(110.0F);

    auto talk = std::make_shared<Nodes::Network::FormantTransfer>(formants, buzz,
        Nodes::Network::FormantTransferConfig { .sample_rate = static_cast<double>(Config::get_sample_rate()) });

    talk->relation() | Audio[0];        // decides; publishes no audio
    talk->voice() | Audio[{ 0, 1 }];    // the resonators, speaking

    talk->set_shift(0.8);               // bigger: every formant x 0.8
    talk->on_voice([] { /* a voiced sound began */ });
}
```

`formants->latest()` gives F1, F2, F3, the RMS and whether the frame is voiced, from any thread. Stay quiet for about a second at the start so the voice gate can learn the room.

`FormantTransfer` registers nothing: route its two networks wherever the application runs networks. Its relation keeps a fixed slot layout (F1..F3 heard, F1..F3 tracked, loudness raw and smoothed, voiced), so other networks can map from it too. `set_manual({f1, f2, f3})` speaks a vowel chosen by hand, with no voice at all, until `clear_manual()`.

### Record and loop

```cpp
void compose()
{
    auto voice = Buffers::VoiceInput::listen(*get_buffer_manager(), { .sample_rate = 48000.0, .ring_seconds = 30.0 });
    auto looper = std::make_shared<Kriya::Looper>(*get_buffer_manager(), *get_scheduler(), Config::get_buffer_size());

    // Take the last two seconds; it is cut on the processing thread and collected here.
    voice->recorder()->request_take(96000);
    schedule_metro(1.0 / 60.0, [voice, looper] {
        while (auto take = voice->recorder()->collect())
            looper->add(take->stream, { .speed = 1.0, .level = 0.8 });
    });

    looper->echo(voice->recorder()->ring(), 0.35, 0.4);    // the live voice, 0.35 s late
}
```

Mark a moment with `recorder()->frames()` and cut from it with `request_take_since(mark, preroll)`: with `FormantTransfer::on_voice` start and stop, every word you say becomes a take. `looper->speed(slot, -1.0)` plays a loop backwards from where it is. The managers are the caller's; nothing here reaches the engine itself.

### Play a take as a groove

```cpp
auto groove = std::make_shared<Kriya::SliceSequencer>(*get_buffer_manager(), *get_scheduler(),
    Kriya::SliceSequencerConfig { .slices = 4, .steps = 16, .bpm = 100.0, .buffer_size = Config::get_buffer_size() });
groove->set_take(take);                     // for example from RingRecorder::collect()
groove->set_hit(0, 0, 1.0);                 // chop 1 on the downbeat
groove->set_hit(2, 4, 0.8);
groove->set_reverse(2, true);               // chop 3 backwards
groove->set_pitch(12, 2.0);                 // step 13 an octave up
groove->start();

groove->queue({ .slice = 1, .velocity = 0.9, .speed = 1.5 });   // from anywhere: plays on the next step
```

The clock is a `Kriya::pattern` with a duration source called once per bar, so `set_tempo` and `set_swing` land on the next bar line. Queued hits are how gestures, `Nexus` events or other patterns play in time with the grid. Reversed hits read the chop through a negative-velocity time map, as `TapSet` plays backwards.

### Use the kernel directly

```cpp
namespace D = Kinesis::Discrete;

auto coefs = D::envelope_follow_state(0.01, 0.1, 48000.0, D::EnvelopeMode::RMS);
double env = D::envelope_block(samples, {}, coefs);       // final value only
D::envelope_block(samples, samples, coefs);               // in place, per sample
```

`envelope_follow(window, coefs)` has the `CoefFunction` shape `RelationNetwork`'s `DeriveOperator` takes, so it can follow a slot there too.

---

## How the follower works

Each sample moves a level toward a target:

```
target = |x|          (PEAK)
target = x^2          (RMS; the output is sqrt(level))

g      = attack_decay   if target > level
         release_decay  otherwise
level  = target + g * (level - target)
```

The decays are `Kinesis::Discrete::decay_per_sample(seconds, rate)`, that is `exp(-1 / (seconds * rate))`, so after the attack time a step has covered 1 - 1/e (about 63%) and after the release time a fall has dropped to 1/e. A time of zero follows instantly. This is `Kinesis::damp_asymmetric` with the exponentials computed once per parameter change instead of once per sample.

Layout of the `coefs` array: `[attack_decay, release_decay, mode, level]`.

---

## Tests

```
cmake -G Ninja -B test/build -S test -DCMAKE_BUILD_TYPE=Release
cmake --build test/build --parallel
./test/build/test_mf_follow
./test/build/test_formant
```

No audio device needed. `test_mf_follow` checks the attack and release times against their definition, that RMS of a sine settles at A / sqrt(2), the decibel functions, every output mode of the processor (including sidechain, gain law, several buffers, silent-block skipping and silencing, parameter changes between blocks), that the node and the processor agree sample for sample, input nodes, save and restore, tick hooks, and the `RingRecorder`: frame counting, takes cut on the processing thread with exactly the last samples, takes since a mark with pre-roll, capping at the ring and skipping empty blocks.

`test_formant` checks `GlottalSource`, the LPC functions on a known sine, the tracker on three synthetic vowels over a noise bed, that a steady fan alone is never voice, `FormantProcessor` fed block by block, and `FormantTransfer`: formants glide from a neutral vowel to the tracked one and the resonators follow, the voiced slot and its event, loudness, shift, manual vowels and silence. Each exits with zero when everything passes.

---

## CMake

`mf_follow.cmake` defines the `mf_follow` target and links `MayaFlux::MayaFluxLib`. In the consuming project, put the module in `community/mf_follow/` and list it in `community.cmake`:

```
mf_follow
```

Weave can also add it to a project once it is in the community registry.

---

## Ideas that fit but are not built

- Hold time, so a gate stays open across short gaps (`Kinesis::Discrete::one_shot`).
- A sidechain filter before detection, so bass does not dominate (`biquad_highpass`).
- Gate, compressor and expander processors: a gain computer on top of `EnvelopeOutput::GAIN`.
- An adaptive threshold from a learned noise floor (`Kinesis::Stochastic::Estimate`).

---

## Licence

GPLv3. See [LICENSE](LICENSE).
