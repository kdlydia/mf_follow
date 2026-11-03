# Log

Working notes for mf_follow and the projects built on it (FollowExamples, DrawnVoice).
Newest at the bottom.

## 2026-11-01

Started mf_follow as a MayaFlux community module. First piece is an envelope follower: one Kinesis kernel (`envelope_follow`) used by a buffer processor (`EnvelopeProcessor`) and a node (`EnvelopeFollower`). Both give the same numbers on the same input.

Upstream review: no `API/` includes in a module, pass the buffer manager in instead. Moved per-buffer state to `SnapshotCell<FlatMap>`, no mutex, no `atomic<weak_ptr>` (missing on macOS).

FollowExamples: swell, duck, filter, meter. Too many windows and faders, but it showed each piece working.

## 2026-11-02

Registry PR merged (MayaFlux/community-sources-registry#3). mf_follow is installable through Weave now.

Merged the formant tracker from the mf_formant prototype: LPC, voice gate, F1 to F3. `FormantTransfer` is a talk-box: a RelationNetwork glides the tracked formants and a ResonatorNetwork shapes a buzz with them.

Recording and looping: `RingRecorder` keeps the last seconds of the mic, takes are cut on the audio thread. `Looper` plays takes with TapSet. `SliceSequencer` chops a take and plays it on a Kriya pattern clock; the perform example triggers chops from a Nexus strike zone.

Things that broke:
- a processor on an input listener never ran. The listener has to sit on an output channel.
- multi-window apps crashed at start. `create_window` races the graphics thread. Building the UI from a frame-clock task with a 0.05 s delay avoids it (a 0 delay runs immediately and does not).
- replacing a take crashed. Samplers were freed while the audio thread still used them. Now kept until the end.
- `Nexus::Fabric` needs the buffer manager in 1.0.

perform had far too many controls. Started DrawnVoice: one window, a few controls, add only what I miss.

## 2026-11-03

`VocalTract`: Kelly-Lochbaum waveguide, 44 sections at twice the sample rate, about 16 cm. `WaveguideNetwork` only does one uniform tube, and a vowel is the change of width along the tract, so it needed its own model.

Vowel shapes from three numbers (front, height, rounding), the seven Italian vowels. `vowels_in` reads the vowels of a sentence ("Nel mezzo del cammin di nostra vita" gives e e o e a i i o a i a).

Formants measured through the waveguide:

    i  241 1745    e  392 1851    E  478 1979    a  619 1397
    O  465 1127    o  340  908    u  177  727

Right shape of vowel space. /i/ F2 is too low (should be near 2300), /u/ F1 too low. First try had /u/ and /o/ wrong, the tongue was too far back.

DrawnVoice now: type a sentence, it speaks the vowels. One plot shows the tract walls and the pressure wave inside, one shows the spectrum with formants.

Next:
- F0 tracking. Vowel relay, overtone work and pitch feedback all need it.
- snap a narrow resonator to the harmonic nearest each formant, to try overtone singing.
- vowel relay with the relation network.
