// Offline checks for formant following and formant transfer in mf_follow. No audio engine and no
// microphone: every signal is made here. Exit code 0 means every check passed.

#include "MayaFlux/MayaFlux.hpp"

#include "FormantProcessor.hpp"
#include "FormantTracker.hpp"
#include "FormantTransfer.hpp"
#include "GlottalSource.hpp"
#include "Lpc.hpp"
#include "VocalTract.hpp"

#include <MayaFlux/Kinesis/Discrete/Coefficients.hpp>
#include <MayaFlux/Kinesis/Discrete/Convolution.hpp>
#include <MayaFlux/Kinesis/Discrete/Taper.hpp>
#include <MayaFlux/Kinesis/Discrete/Word.hpp>

using namespace MayaFlux;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    g_failures += ok ? 0 : 1;
}

struct Vowel {
    const char* name;
    std::array<double, 3> formants;
    std::array<double, 3> bandwidths;
};

constexpr double k_rate = 48000.0;

/** @brief A GlottalSource through three parallel bandpass sections, one per formant, scaled to rms 0.1. */
std::vector<double> synthesise(const Vowel& vowel, double f0, size_t samples)
{
    Nodes::Generator::GlottalSource source(static_cast<float>(f0));

    std::array<Kinesis::Discrete::Biquad, 3> sections;
    for (size_t i = 0; i < 3; ++i) {
        sections[i] = Kinesis::Discrete::biquad_bandpass(vowel.formants[i], vowel.formants[i] / vowel.bandwidths[i], k_rate);
    }
    constexpr std::array<double, 3> k_tilt { 1.0, 0.7, 0.5 };

    std::array<std::array<double, 4>, 3> state {}; // x1, x2, y1, y2 per section
    std::vector<double> out(samples);
    for (size_t n = 0; n < samples; ++n) {
        const double x = source.process_sample();
        double sum = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            const auto& s = sections[i];
            auto& st = state[i];
            const double y = s.b0 * x + s.b1 * st[0] + s.b2 * st[1] - s.a1 * st[2] - s.a2 * st[3];
            st = { x, st[0], y, st[2] };
            sum += k_tilt[i] * y;
        }
        out[n] = sum;
    }

    double energy = 0.0;
    for (double v : out) {
        energy += v * v;
    }
    const double scale = 0.1 / std::sqrt(energy / static_cast<double>(samples));
    for (double& v : out) {
        v *= scale;
    }
    return out;
}

/** @brief White noise at a steady level, from a fixed seed. */
std::vector<double> fan(size_t samples, double level)
{
    std::vector<double> noise(samples);
    uint32_t state = 0x1234ABCDU;
    for (double& v : noise) {
        state = Kinesis::Discrete::xorshift32(state);
        v = level * (static_cast<double>(state) / 2147483648.0 - 1.0);
    }
    return noise;
}

void test_glottal_source()
{
    std::printf("GlottalSource\n");

    constexpr double f0 = 120.0;
    Nodes::Generator::GlottalSource flat(static_cast<float>(f0), 0.0);
    const auto samples = flat.process_batch(48000);

    size_t pulses = 0;
    for (double v : samples) {
        pulses += v > 0.5 ? 1 : 0;
    }
    check(pulses >= 119 && pulses <= 121, "120 Hz with no tilt gives about 120 pulses a second");

    Nodes::Generator::GlottalSource tilted(static_cast<float>(f0), 0.97);
    const auto voiced = tilted.process_batch(4800);
    const double peak = *std::ranges::max_element(voiced);
    check(peak > 0.99 && peak < 1.001, "tilted pulses peak at the amplitude (a pulse decays before the next)");

    tilted.save_state();
    const double before = tilted.process_sample();
    tilted.restore_state();
    check(tilted.process_sample() == before, "restore_state replays the same sample");
}

void test_lpc()
{
    std::printf("Lpc\n");

    // A 1 kHz sine at a 12 kHz rate: the envelope must peak at 1 kHz.
    constexpr double rate = 12000.0;
    std::vector<double> sine(256);
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / rate);
    }
    Kinesis::Discrete::apply_hann(sine);

    auto r = Kinesis::Discrete::auto_correlate(sine, false);
    r[0] *= 1.0001;
    const auto a = Kinesis::Discrete::levinson_durbin(r, 4);
    check(a.size() == 5 && a[0] == 1.0, "levinson_durbin returns order + 1 coefficients with a[0] = 1");

    const auto envelope = Kinesis::Discrete::lpc_envelope_db(a, 256);
    const auto peaks = Kinesis::Discrete::spectral_peaks(envelope, 0.5 * rate);
    check(!peaks.empty() && std::abs(peaks.front() - 1000.0) < 50.0, "envelope of a 1 kHz sine peaks within 50 Hz of 1 kHz");

    const std::vector<double> silent(8, 0.0);
    const auto none = Kinesis::Discrete::levinson_durbin(silent, 4);
    check(none[0] == 1.0 && none[1] == 0.0, "zero energy gives the trivial predictor");
}

void test_tracker()
{
    std::printf("FormantTracker, tilted pulse train at 120 Hz over a noise bed\n");

    constexpr std::array<Vowel, 3> k_vowels { {
        { "a", { 800.0, 1200.0, 2500.0 }, { 90.0, 110.0, 150.0 } },
        { "i", { 270.0, 2300.0, 3000.0 }, { 60.0, 100.0, 130.0 } },
        { "e", { 400.0, 2000.0, 2600.0 }, { 70.0, 100.0, 140.0 } },
    } };

    const Kinesis::FormantConfig config;
    constexpr size_t k_bed = 48000 * 3;

    for (const Vowel& vowel : k_vowels) {
        Kinesis::FormantTracker tracker(config);

        std::vector<double> signal = fan(k_bed, 0.02);
        const auto voice = synthesise(vowel, 120.0, 48000);
        const auto bed = fan(voice.size(), 0.02);
        for (size_t i = 0; i < voice.size(); ++i) {
            signal.push_back(voice[i] + bed[i]);
        }

        Kinesis::FormantReading reading;
        for (size_t start = 0; start + config.frame_size <= signal.size(); start += 512) {
            reading = tracker.analyse(std::span<const double>(signal).subspan(start, config.frame_size));
        }

        const std::array<double, 3> found { reading.f1, reading.f2, reading.f3 };
        // F1 of a high vowel sits between two harmonics of a 120 Hz voice, so it is allowed more slack.
        const std::array<double, 3> tolerance { 0.20, 0.12, 0.12 };

        char label[96];
        std::snprintf(label, sizeof(label), "/%s/ is voiced", vowel.name);
        check(reading.voiced, label);
        for (size_t i = 0; i < 3; ++i) {
            const double error = std::abs(found[i] - vowel.formants[i]) / vowel.formants[i];
            std::snprintf(label, sizeof(label), "/%s/ F%zu expected %4.0f found %4.0f (%.1f%%)", vowel.name, i + 1, vowel.formants[i], found[i], error * 100.0);
            check(error <= tolerance[i], label);
        }
    }

    // The fan alone, for long enough to learn it: no frame after the warm-up may count as voice.
    Kinesis::FormantTracker tracker(config);
    const auto noise = fan(48000 * 8, 0.02);
    size_t voiced = 0;
    size_t counted = 0;
    for (size_t start = 0; start + config.frame_size <= noise.size(); start += 512) {
        const auto reading = tracker.analyse(std::span<const double>(noise).subspan(start, config.frame_size));
        if (!reading.activity.warming) {
            ++counted;
            voiced += reading.voiced ? 1 : 0;
        }
    }
    char label[96];
    std::snprintf(label, sizeof(label), "fan alone: %zu of %zu frames counted as voice", voiced, counted);
    check(voiced * 50 <= counted, label);
}

/** @brief /a/ after a quiet bed, the way a microphone would hand it over: one buffer block at a time. */
std::vector<double> vowel_after_bed()
{
    constexpr Vowel k_a { "a", { 800.0, 1200.0, 2500.0 }, { 90.0, 110.0, 150.0 } };
    std::vector<double> signal = fan(48000 * 3, 0.02);
    const auto voice = synthesise(k_a, 120.0, 48000);
    const auto bed = fan(voice.size(), 0.02);
    for (size_t i = 0; i < voice.size(); ++i) {
        signal.push_back(voice[i] + bed[i]);
    }
    return signal;
}

void feed(Buffers::FormantProcessor& processor, const std::shared_ptr<Buffers::AudioBuffer>& buffer, std::span<const double> signal, size_t offset)
{
    auto& data = buffer->get_data();
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = offset + i < signal.size() ? signal[offset + i] : 0.0;
    }
    processor.processing_function(buffer);
}

void test_processor()
{
    std::printf("FormantProcessor, /a/ fed block by block\n");

    auto processor = std::make_shared<Buffers::FormantProcessor>();
    auto buffer = std::make_shared<Buffers::AudioBuffer>(0);
    const size_t block = buffer->get_data().size();
    const auto signal = vowel_after_bed();

    for (size_t offset = 0; offset < signal.size(); offset += block) {
        feed(*processor, buffer, signal, offset);
    }

    const auto reading = processor->latest();
    check(reading.voiced, "the vowel is voiced at the end");
    check(std::abs(reading.f2 - 1200.0) / 1200.0 <= 0.12, "F2 near 1200 Hz through the buffer path");
    check(processor->frames() == (signal.size() + block - 1) / block, "every block with sound was analysed");
    check(std::ranges::all_of(buffer->get_data(), [](double v) { return v == 0.0; }), "the buffer is silenced after analysis");
    check(processor->envelope().size() == processor->config().grid_points, "the envelope of a voiced frame is published");
    check(processor->frame().size() == processor->config().frame_size, "the analysed frame is published");

    const auto before = processor->frames();
    std::ranges::fill(buffer->get_data(), 0.0);
    processor->processing_function(buffer);
    check(processor->frames() == before, "an all-zero block is skipped");
}

void test_transfer()
{
    std::printf("FormantTransfer, /a/ through the relation into the resonators\n");

    auto processor = std::make_shared<Buffers::FormantProcessor>();
    auto buffer = std::make_shared<Buffers::AudioBuffer>(0);
    const size_t block = buffer->get_data().size();
    const auto signal = vowel_after_bed();

    auto carrier = std::make_shared<Nodes::Generator::GlottalSource>(110.0F);
    Nodes::Network::FormantTransfer talk(processor, carrier, { .sample_rate = k_rate, .glide = 0.02 });

    int starts = 0;
    talk.on_voice([&starts] { ++starts; });

    const auto run_block = [&] {
        talk.relation()->process_batch(static_cast<unsigned int>(block));
        talk.voice()->process_batch(static_cast<unsigned int>(block));
    };

    check(std::abs(talk.formant(0) - 500.0) < 1e-9, "the resonators start on the neutral vowel");

    double peak_out = 0.0;
    for (size_t offset = 0; offset < signal.size(); offset += block) {
        feed(*processor, buffer, signal, offset);
        run_block();
        if (const auto out = talk.voice()->get_audio_buffer(); out && offset > signal.size() - 48000 / 2) {
            for (double v : *out) {
                peak_out = std::max(peak_out, std::abs(v));
            }
        }
    }

    constexpr std::array<double, 3> k_a { 800.0, 1200.0, 2500.0 };
    const std::array<double, 3> tolerance { 0.20, 0.12, 0.12 };
    for (size_t i = 0; i < 3; ++i) {
        char label[96];
        const double heard = talk.formant(i);
        const double tuned = talk.voice()->get_resonators()[i].frequency;
        std::snprintf(label, sizeof(label), "F%zu glides to the vowel: %4.0f Hz, resonator at %4.0f Hz", i + 1, heard, tuned);
        check(std::abs(heard - k_a[i]) / k_a[i] <= tolerance[i] && std::abs(tuned - heard) < 1.0, label);
    }

    check(talk.voiced(), "the relation's voiced slot is active");
    check(starts >= 1, "on_voice fired when the vowel began");
    check(talk.loudness() > 0.3, "loudness follows the voice");
    check(peak_out > 1e-3, "the carrier speaks through the resonators");

    talk.set_shift(0.8);
    for (size_t n = 0; n < 40; ++n) {
        feed(*processor, buffer, signal, signal.size() - 48000 + n * block);
        run_block();
    }
    check(talk.formant(1) < 1200.0 * 0.9, "a shift below 1 lowers the formants");

    const std::vector<double> silence(block * 200, 1e-7);
    for (size_t offset = 0; offset < silence.size(); offset += block) {
        feed(*processor, buffer, silence, offset);
        run_block();
    }
    check(talk.loudness() < 0.05, "loudness falls away in silence");

    talk.set_shift(1.0);
    talk.set_manual({ 300.0, 2300.0, 3000.0 }, 0.8);
    for (size_t offset = 0; offset < silence.size(); offset += block) {
        feed(*processor, buffer, silence, offset);
        run_block();
    }
    check(std::abs(talk.formant(1) - 2300.0) < 5.0, "a manual vowel overrides the tracker");
    check(std::abs(talk.loudness() - 0.8) < 0.02 && talk.voiced(), "a manual vowel sounds without a voice");

    talk.clear_manual();
    for (size_t offset = 0; offset < silence.size(); offset += block) {
        feed(*processor, buffer, silence, offset);
        run_block();
    }
    check(talk.loudness() < 0.05 && !talk.is_manual(), "clearing manual returns to the tracker");
}

void test_tract()
{
    std::printf("VocalTract, vowel shapes through the waveguide\n");

    std::map<std::string, std::vector<double>> found;
    for (const auto& v : Kinesis::vowel_targets()) {
        const auto d = Kinesis::tract_diameters(v.shape, 44);
        const auto db = Kinesis::tract_response_db(d, 2.0 * k_rate, 600, 5000.0);
        auto f = Kinesis::tract_formants(db, 5000.0, 3);
        f.resize(3, 0.0);
        found[v.symbol] = f;
        std::printf("  /%s/  F1 %5.0f  F2 %5.0f  F3 %5.0f\n", v.symbol, f[0], f[1], f[2]);
        if (std::getenv("TRACT_DEBUG")) {
            std::printf("      diameters:");
            for (size_t i = 0; i < d.size(); i += 4)
                std::printf(" %.2f", d[i]);
            std::printf("\n      peaks:");
            for (size_t i = 1; i + 1 < db.size(); ++i)
                if (db[i] > db[i - 1] && db[i] >= db[i + 1])
                    std::printf(" %.0f(%.0f)", static_cast<double>(i) * 5000.0 / 599.0, db[i]);
            std::printf("\n");
        }
    }

    check(found["a"][0] > found["i"][0] && found["a"][0] > found["u"][0], "/a/ has the highest F1 of a, i, u");
    check(found["i"][1] > found["a"][1] && found["a"][1] > found["u"][1], "F2 falls from /i/ through /a/ to /u/");
    check(found["i"][0] < found["e"][0] && found["e"][0] < found["a"][0], "F1 rises from /i/ through /e/ to /a/");
    check(found["u"][1] < found["o"][1] + 300.0, "/u/ is at least as back (low F2) as /o/");

    const auto seq = Kinesis::vowels_in("Nel mezzo del cammin di nostra vita");
    std::string skeleton;
    for (const auto& v : seq)
        skeleton += v.symbol;
    std::printf("  vowels of \"Nel mezzo del cammin di nostra vita\": %s\n", skeleton.c_str());
    check(skeleton == "eeoeaiioaia", "the sentence's vocalic skeleton is read in order");
    check(Kinesis::vowels_in("perch\xC3\xA8 cos\xC3\xAC").back().symbol == std::string("i")
            && Kinesis::vowels_in("perch\xC3\xA8")[1].symbol == std::string("E"),
        "grave accents give open vowels, other accents the plain vowel");

    Nodes::Network::VocalTract tract(44, k_rate);
    tract.set_exciter(std::make_shared<Nodes::Generator::GlottalSource>(120.0F));
    tract.set_shape(Kinesis::vowel_target("a")->shape);
    for (int b = 0; b < 40; ++b)
        tract.process_batch(512);
    const auto out = tract.get_audio_buffer();
    double peak = 0.0;
    if (out)
        for (double v : *out)
            peak = std::max(peak, std::abs(v));
    check(peak > 1e-3 && peak < 50.0, "the tract sounds and stays stable");
    const auto d = tract.diameters();
    const auto target = Kinesis::tract_diameters(Kinesis::vowel_target("a")->shape, 44);
    check(std::abs(d[30] - target[30]) < 0.05, "the shape reaches its target");
    check(tract.get_node_count() == 44 && tract.get_node_output(20).has_value(), "each section is a node with its pressure");
}

} // namespace

int main()
{
    test_glottal_source();
    test_lpc();
    test_tracker();
    test_processor();
    test_transfer();
    test_tract();

    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
