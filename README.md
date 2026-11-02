# mf_follow

A MayaFlux community module for following the envelope of a signal.

It measures how loud a signal is, sample by sample or block by block, with separate attack and release times, in peak or RMS mode. One Kinesis kernel does the measuring; a buffer processor and a node run it for the two ways MayaFlux processes audio, and they produce exactly the same numbers on the same samples.

Needs MayaFlux 1.0.0 or newer. Does not need Lila.

---

## What is in it

| Piece | Namespace | Kind | What it does |
| ----- | --------- | ---- | ------------ |
| `envelope_follow`, `envelope_block`, `envelope_follow_state` | `Kinesis::Discrete` | kernels | Attack/release follower in the usual Kinesis shape: a `coefs` array holds the parameters and the level. Built on `decay_per_sample`. |
| `amplitude_to_db`, `db_to_amplitude` | `Kinesis::Discrete` | free functions | Decibel conversion. MayaFlux has none. |
| `EnvelopeProcessor` | `Buffers` | `BufferProcessor` | Follows whole audio blocks. Publishes the envelope, replaces the block with it, or applies it as a gain, measured on the buffer itself or on a sidechain buffer. |
| `EnvelopeFollower` | `Nodes::Filters` | `Node` | Follows one sample at a time, in a chain (`source >> follower`) or with an input node, like `IIR`. |

Nodes are the per-sample path and buffers are the per-block path. Neither is built on the other: both call the same kernel.

---

## Structure

```
mf_follow/
    mf_follow.cmake         # CMake module definition
    community.json          # manifest (Weave reads this)
    src/
        Envelope.hpp / .cpp
        EnvelopeProcessor.hpp / .cpp
        EnvelopeFollower.hpp / .cpp
    test/
        CMakeLists.txt
        test_mf_follow.cpp
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
    auto level = Buffers::EnvelopeProcessor::listen(0, 0.005, 0.2);

    schedule_metro(0.1, [level] {
        std::printf("%.1f dB\n", level->latest_db());
    });
}
```

`listen()` makes an input listener buffer on the output channel, because a buffer's processing chain only runs there, and attaches the processor to that buffer's own chain. The processor zeroes each block after measuring it (`set_silence_after`), so the microphone does not reach the speakers. It also skips the empty blocks the engine hands an input listener between microphone deliveries; following those would halve the level.

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
```

No audio device needed. The test checks the attack and release times against their definition, that RMS of a sine settles at A / sqrt(2), the decibel functions, every output mode of the processor (including sidechain, gain law, several buffers, silent-block skipping and parameter changes between blocks), that the node and the processor agree sample for sample, input nodes, save and restore, and tick hooks. The exit code is zero when everything passes.

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
