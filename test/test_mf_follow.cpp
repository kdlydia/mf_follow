#include "MayaFlux/MayaFlux.hpp"

#include <MayaFlux/Nodes/Conduit/Constant.hpp>

#include "Envelope.hpp"
#include "EnvelopeFollower.hpp"
#include "EnvelopeProcessor.hpp"

#include <cstdio>
#include <numbers>

namespace {

namespace D = MayaFlux::Kinesis::Discrete;
using MayaFlux::Buffers::AudioBuffer;
using MayaFlux::Buffers::EnvelopeOutput;
using MayaFlux::Buffers::EnvelopeProcessor;
using MayaFlux::Nodes::Filters::EnvelopeFollower;

constexpr double k_rate = 48000.0;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, double got = 0.0, double want = 0.0)
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    std::printf("%s  %s  (got %.6f, want %.6f)\n", ok ? "PASS" : "FAIL", what, got, want);
}

void check_near(double got, double want, double tol, const char* what)
{
    check(std::abs(got - want) <= tol, what, got, want);
}

/// Deterministic test signal: two sines and a burst, so attack and release both get exercised.
std::vector<double> test_signal(size_t n)
{
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / k_rate;
        const double burst = (i / 2000) % 3 == 0 ? 1.0 : 0.15;
        x[i] = burst * (0.6 * std::sin(2.0 * std::numbers::pi * 220.0 * t) + 0.3 * std::sin(2.0 * std::numbers::pi * 1730.0 * t));
    }
    return x;
}

void kernel_tests()
{
    std::printf("\n-- kernel --\n");

    auto coefs = D::envelope_follow_state(0.01, 0.1, k_rate, D::EnvelopeMode::PEAK);
    const std::vector<double> ones(480, 1.0);
    const double after_attack = D::envelope_block(ones, {}, coefs);
    check_near(after_attack, 1.0 - std::exp(-1.0), 1e-9, "peak attack reaches 1 - 1/e after the attack time");

    for (int i = 0; i < 48000; ++i)
        (void)D::envelope_follow(ones, coefs);
    const std::vector<double> zeros(4800, 0.0);
    const double before = D::envelope_value(coefs);
    const double after_release = D::envelope_block(zeros, {}, coefs);
    check_near(after_release, before * std::exp(-1.0), 1e-9, "peak release falls by 1/e after the release time");

    auto instant = D::envelope_follow_state(0.0, 0.0, k_rate);
    const std::vector<double> neg { -0.7 };
    check_near(D::envelope_follow(neg, instant), 0.7, 1e-12, "zero times follow |x| instantly");

    auto rms = D::envelope_follow_state(0.1, 0.1, k_rate, D::EnvelopeMode::RMS);
    std::vector<double> sine(48000);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.8 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / k_rate);
    const double settled = D::envelope_block(sine, {}, rms);
    check_near(settled, 0.8 / std::sqrt(2.0), 0.8 / std::sqrt(2.0) * 0.02, "RMS of a sine settles at A / sqrt(2)");

    const double held = D::envelope_value(rms);
    D::envelope_set_mode(rms, D::EnvelopeMode::PEAK);
    check_near(D::envelope_value(rms), held, 1e-12, "changing mode keeps the output continuous");

    std::vector<double> block = test_signal(1024);
    auto a = D::envelope_follow_state(0.005, 0.05, k_rate);
    auto b = a;
    std::vector<double> separate(block.size());
    for (size_t i = 0; i < block.size(); ++i)
        separate[i] = D::envelope_follow(std::span<const double>(&block[i], 1), a);
    (void)D::envelope_block(block, block, b);
    check(block == separate, "envelope_block in place equals envelope_follow per sample");

    check_near(D::amplitude_to_db(1.0), 0.0, 1e-12, "0 dB at unity");
    check_near(D::amplitude_to_db(0.5), -6.0206, 1e-4, "-6.02 dB at half");
    check_near(D::amplitude_to_db(0.0), -120.0, 1e-12, "silence hits the floor");
    check_near(D::db_to_amplitude(D::amplitude_to_db(0.3)), 0.3, 1e-12, "dB round trip");
}

std::shared_ptr<AudioBuffer> make_buffer(const std::vector<double>& signal, size_t offset)
{
    auto buffer = std::make_shared<AudioBuffer>(0);
    auto& data = buffer->get_data();
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = offset + i < signal.size() ? signal[offset + i] : 0.0;
    return buffer;
}

void processor_tests()
{
    std::printf("\n-- EnvelopeProcessor --\n");

    const size_t block = std::make_shared<AudioBuffer>(0)->get_data().size();
    const auto signal = test_signal(block * 8);

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.005, 0.05);
        auto buffer = make_buffer(signal, 0);
        const auto original = buffer->get_data();
        proc->on_attach(buffer);
        proc->processing_function(buffer);

        auto coefs = D::envelope_follow_state(0.005, 0.05, k_rate);
        const double want = D::envelope_block(original, {}, coefs);
        check(buffer->get_data() == original, "FEATURE leaves the buffer untouched");
        check_near(proc->feature(buffer), want, 1e-12, "FEATURE publishes the block's final envelope");
        check_near(proc->latest(), want, 1e-12, "latest() matches feature()");
    }

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.005, 0.05, D::EnvelopeMode::RMS, EnvelopeOutput::SIGNAL);
        auto coefs = D::envelope_follow_state(0.005, 0.05, k_rate, D::EnvelopeMode::RMS);
        auto buffer = std::make_shared<AudioBuffer>(0);
        proc->on_attach(buffer);
        bool same = true;
        for (size_t b = 0; b < 8; ++b) {
            auto& data = buffer->get_data();
            std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(b * block), block, data.begin());
            std::vector<double> want(data.begin(), data.end());
            (void)D::envelope_block(want, want, coefs);
            proc->processing_function(buffer);
            same = same && data == want;
        }
        check(same, "SIGNAL writes the envelope and carries the level across blocks");
    }

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.0, 0.0, D::EnvelopeMode::PEAK, EnvelopeOutput::GAIN);
        auto target = std::make_shared<AudioBuffer>(0);
        auto side = std::make_shared<AudioBuffer>(1);
        std::ranges::fill(target->get_data(), 0.5);
        std::ranges::fill(side->get_data(), -0.25);
        proc->set_sidechain(side);
        proc->on_attach(target);
        proc->processing_function(target);
        check_near(target->get_data().back(), 0.125, 1e-12, "GAIN applies the sidechain's envelope to the target");
        check(side->get_data().front() == -0.25, "the sidechain itself is not modified");

        std::ranges::fill(target->get_data(), 0.5);
        proc->set_gain_law([](double env) { return env > 0.1 ? 0.0 : 1.0; });
        proc->processing_function(target);
        check_near(target->get_data().back(), 0.0, 1e-12, "a gain law turns GAIN into a ducker");
    }

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.0, 0.01);
        auto loud = std::make_shared<AudioBuffer>(0);
        auto quiet = std::make_shared<AudioBuffer>(1);
        std::ranges::fill(loud->get_data(), 0.9);
        std::ranges::fill(quiet->get_data(), 0.1);
        proc->on_attach(loud);
        proc->on_attach(quiet);
        proc->processing_function(loud);
        proc->processing_function(quiet);
        check_near(proc->feature(loud), 0.9, 1e-12, "each attached buffer keeps its own level (loud)");
        check_near(proc->feature(quiet), 0.1, 1e-12, "each attached buffer keeps its own level (quiet)");

        proc->set_skip_silent_blocks(true);
        std::ranges::fill(loud->get_data(), 0.0);
        proc->processing_function(loud);
        check_near(proc->feature(loud), 0.9, 1e-12, "a silent block is skipped when asked");
        proc->set_skip_silent_blocks(false);
        proc->processing_function(loud);
        check(proc->feature(loud) < 0.9, "a silent block releases otherwise", proc->feature(loud), 0.9);

        proc->on_detach(quiet);
        check_near(proc->feature(quiet), 0.0, 1e-12, "detaching forgets that buffer's level");
        check(proc->feature(loud) > 0.0, "detaching one buffer keeps the others", proc->feature(loud), 0.0);
    }

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.0, 0.1);
        auto buffer = std::make_shared<AudioBuffer>(0);
        std::ranges::fill(buffer->get_data(), 0.4);
        proc->set_silence_after(true);
        proc->on_attach(buffer);
        proc->processing_function(buffer);
        check_near(proc->feature(buffer), 0.4, 1e-12, "silence_after still measures the block");
        check(std::ranges::all_of(buffer->get_data(), [](double v) { return v == 0.0; }), "silence_after zeroes the block");
    }

    {
        auto proc = std::make_shared<EnvelopeProcessor>(0.0, 10.0);
        auto buffer = std::make_shared<AudioBuffer>(0);
        std::ranges::fill(buffer->get_data(), 0.8);
        proc->on_attach(buffer);
        proc->processing_function(buffer);
        proc->set_times(0.01, 0.0);
        std::ranges::fill(buffer->get_data(), 0.2);
        proc->processing_function(buffer);
        check_near(proc->feature(buffer), 0.2, 1e-12, "set_times takes effect at the next block");

        double observed = -1.0;
        proc->set_observer([&observed](double env) { observed = env; });
        proc->processing_function(buffer);
        check_near(observed, proc->latest(), 1e-12, "the observer receives the block's envelope");
    }
}

void node_tests()
{
    std::printf("\n-- EnvelopeFollower --\n");

    const size_t block = std::make_shared<AudioBuffer>(0)->get_data().size();
    const auto signal = test_signal(block * 6);

    {
        auto node = std::make_shared<EnvelopeFollower>(0.005, 0.05, D::EnvelopeMode::RMS);
        node->set_sample_rate(static_cast<uint32_t>(k_rate));
        auto proc = std::make_shared<EnvelopeProcessor>(0.005, 0.05, D::EnvelopeMode::RMS, EnvelopeOutput::SIGNAL);
        auto buffer = std::make_shared<AudioBuffer>(0);
        proc->on_attach(buffer);

        bool same = true;
        for (size_t b = 0; b < 6; ++b) {
            auto& data = buffer->get_data();
            std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(b * block), block, data.begin());
            std::vector<double> from_node(block);
            for (size_t i = 0; i < block; ++i)
                from_node[i] = node->process_sample(data[i]);
            proc->processing_function(buffer);
            same = same && from_node == data;
        }
        check(same, "node and buffer processor agree sample for sample");
    }

    {
        auto source = std::make_shared<MayaFlux::Nodes::Constant>(-0.6);
        auto node = std::make_shared<EnvelopeFollower>(source, 0.0, 0.1);
        node->set_sample_rate(static_cast<uint32_t>(k_rate));
        const auto out = node->process_batch(16);
        check_near(out.back(), 0.6, 1e-12, "an input node is followed like IIR's input");
        check(node->get_modulators().size() == 1, "the input node is reported as a modulator");
    }

    {
        auto node = std::make_shared<EnvelopeFollower>(0.01, 0.1);
        node->set_sample_rate(static_cast<uint32_t>(k_rate));
        for (size_t i = 0; i < 1000; ++i)
            (void)node->process_sample(signal[i]);
        node->save_state();
        std::vector<double> first(500);
        for (size_t i = 0; i < first.size(); ++i)
            first[i] = node->process_sample(signal[1000 + i]);
        node->restore_state();
        std::vector<double> second(500);
        for (size_t i = 0; i < second.size(); ++i)
            second[i] = node->process_sample(signal[1000 + i]);
        check(first == second, "save_state then restore_state replays the same output");
    }

    {
        auto node = std::make_shared<EnvelopeFollower>(0.0, 0.0);
        node->set_sample_rate(static_cast<uint32_t>(k_rate));
        double seen_db = 0.0;
        double seen_input = 0.0;
        node->on_tick([&](MayaFlux::Nodes::Filters::EnvelopeContext& ctx) {
            seen_db = ctx.db;
            seen_input = ctx.input;
        });
        (void)node->process_sample(-0.5);
        check_near(seen_input, -0.5, 1e-12, "tick context carries the followed sample");
        check_near(seen_db, -6.0206, 1e-4, "tick context carries the envelope in dB");
        check_near(node->get_db(), -6.0206, 1e-4, "get_db matches the context");
    }
}

} // namespace

int main()
{
    kernel_tests();
    processor_tests();
    node_tests();

    std::printf("\n%d of %d checks passed: %s\n", g_checks - g_failures, g_checks, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
