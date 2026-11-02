#include "SliceSequencer.hpp"

#include <MayaFlux/Kinesis/Tendency/TimeMap.hpp>
#include <MayaFlux/Kriya/Pattern.hpp>

namespace MayaFlux::Kriya {

SliceSequencer::SliceSequencer(Buffers::BufferManager& manager, Vruta::TaskScheduler& scheduler, SliceSequencerConfig config)
    : m_manager(manager)
    , m_scheduler(scheduler)
    , m_config(std::move(config))
    , m_grid(std::max<size_t>(m_config.slices, 1) * std::max<size_t>(m_config.steps, 1))
    , m_pitch(std::max<size_t>(m_config.steps, 1))
    , m_reverse(std::max<size_t>(m_config.slices, 1))
    , m_hits(std::max<size_t>(m_config.slices, 1))
    , m_velocity(std::max<size_t>(m_config.slices, 1))
    , m_bpm(std::clamp(m_config.bpm, 20.0, 400.0))
    , m_swing(std::clamp(m_config.swing, 0.0, 0.5))
    , m_gate(std::clamp(m_config.gate, 0.05, 1.0))
{
    m_config.slices = std::max<size_t>(m_config.slices, 1);
    m_config.steps = std::max<size_t>(m_config.steps, 1);
    m_config.voices = std::max<size_t>(m_config.voices, 1);
    for (auto& p : m_pitch)
        p.store(1.0);
}

SliceSequencer::~SliceSequencer()
{
    stop();
}

void SliceSequencer::set_take(std::shared_ptr<Kakshya::DynamicSoundStream> take)
{
    auto material = take ? build(take) : nullptr;
    m_take_frames.store(material ? material->frames : 0, std::memory_order_relaxed);
    m_take_seconds.store(material ? static_cast<double>(material->frames) / material->rate : 0.0, std::memory_order_relaxed);
    m_material.publish(std::move(material));
}

std::shared_ptr<SliceSequencer::Material> SliceSequencer::build(const std::shared_ptr<Kakshya::DynamicSoundStream>& take) const
{
    const uint64_t frames = take->get_num_frames();
    if (frames < m_config.slices)
        return nullptr;

    auto material = std::make_shared<Material>();
    material->take = take;
    material->frames = frames;
    material->rate = static_cast<double>(take->get_sample_rate());
    for (const uint32_t channel : m_config.channels) {
        auto sampler = std::make_shared<SamplingPipeline>(take, m_manager, m_scheduler, channel, m_config.buffer_size);
        sampler->build();
        material->samplers.push_back(std::move(sampler));
    }
    return material;
}

void SliceSequencer::set_hit(size_t slice, size_t step, double velocity)
{
    if (slice < m_config.slices && step < m_config.steps)
        m_grid[slice * m_config.steps + step].store(std::clamp(velocity, 0.0, 1.0), std::memory_order_relaxed);
}

double SliceSequencer::hit(size_t slice, size_t step) const
{
    return slice < m_config.slices && step < m_config.steps
        ? m_grid[slice * m_config.steps + step].load(std::memory_order_relaxed)
        : 0.0;
}

void SliceSequencer::set_pitch(size_t step, double ratio)
{
    if (step < m_config.steps)
        m_pitch[step].store(std::clamp(ratio, 0.125, 8.0), std::memory_order_relaxed);
}

double SliceSequencer::pitch(size_t step) const
{
    return step < m_config.steps ? m_pitch[step].load(std::memory_order_relaxed) : 1.0;
}

void SliceSequencer::set_reverse(size_t slice, bool reverse)
{
    if (slice < m_config.slices)
        m_reverse[slice].store(reverse, std::memory_order_relaxed);
}

bool SliceSequencer::reverse(size_t slice) const
{
    return slice < m_config.slices && m_reverse[slice].load(std::memory_order_relaxed);
}

uint64_t SliceSequencer::hits(size_t slice) const
{
    return slice < m_config.slices ? m_hits[slice].load(std::memory_order_relaxed) : 0;
}

double SliceSequencer::last_velocity(size_t slice) const
{
    return slice < m_config.slices ? m_velocity[slice].load(std::memory_order_relaxed) : 0.0;
}

std::pair<uint64_t, uint64_t> SliceSequencer::slice_frames(size_t slice) const
{
    const uint64_t frames = m_take_frames.load(std::memory_order_relaxed);
    const uint64_t n = m_config.slices;
    if (slice >= n || frames == 0)
        return { 0, 0 };
    return { frames * slice / n, frames * (slice + 1) / n };
}

std::vector<double> SliceSequencer::bar_durations() const
{
    const double step = step_seconds();
    const double swing = m_swing.load(std::memory_order_relaxed);
    std::vector<double> durations(m_config.steps);
    for (size_t i = 0; i < durations.size(); ++i) {
        // Swing moves every second step later; the pair keeps its total length.
        durations[i] = i % 2 == 0 ? step * (1.0 + swing) : step * (1.0 - swing);
    }
    return durations;
}

void SliceSequencer::start()
{
    if (m_routine)
        return;

    m_routine = Kriya::pattern(
        [steps = m_config.steps](uint64_t index) { return static_cast<size_t>(index % steps); },
        [this](size_t step) { play_step(step); },
        [this](uint64_t) { return bar_durations(); },
        Vruta::ProcessingToken::SAMPLE_ACCURATE);

    if (m_routine)
        m_scheduler.add_task(m_routine);
}

void SliceSequencer::stop()
{
    if (!m_routine)
        return;
    m_scheduler.cancel_task(m_routine);
    m_routine.reset();
}

void SliceSequencer::play_step(size_t step)
{
    m_step.store(step, std::memory_order_relaxed);

    const auto guard = m_material.read();
    const Material* material = guard && *guard ? guard->get() : nullptr;

    if (material) {
        const double ratio = pitch(step);
        for (size_t slice = 0; slice < m_config.slices; ++slice) {
            const double velocity = hit(slice, step);
            if (velocity > 0.0)
                play(*material, { .slice = slice, .velocity = velocity, .speed = ratio, .reverse = reverse(slice) });
        }
    }

    // Queued hits play on this step whether or not there is a take, so the queue never grows.
    m_queue.sweep(
        [](SliceHit&) { return true; },
        [this, material](SliceHit& hit) {
            if (material)
                play(*material, hit);
        });
}

void SliceSequencer::play(const Material& material, const SliceHit& hit)
{
    if (hit.slice >= m_config.slices || hit.velocity <= 0.0 || material.samplers.empty())
        return;

    const uint64_t first = material.frames * hit.slice / m_config.slices;
    const uint64_t last = material.frames * (hit.slice + 1) / m_config.slices;
    const auto length = static_cast<uint64_t>(static_cast<double>(last - first) * m_gate.load(std::memory_order_relaxed));
    if (length < 2)
        return;

    // A reversed hit keeps the end of its chop, so a short gate still sounds the end of the word.
    const uint64_t lo = hit.reverse ? last - length : first;
    const uint64_t hi = lo + length - 1;
    const double speed = std::clamp(hit.speed, 0.125, 8.0);
    const auto voice = static_cast<uint8_t>(m_next_voice);
    m_next_voice = (m_next_voice + 1) % m_config.voices;

    for (const auto& sampler : material.samplers) {
        auto slice = Kakshya::StreamSlice::from_frame_range(material.take, lo, hi, voice);
        slice.scale = std::clamp(hit.velocity, 0.0, 1.0) * m_level.load(std::memory_order_relaxed);
        if (hit.reverse) {
            slice.with_time_map(Kinesis::TimeMaps::integrated(static_cast<double>(hi), std::make_shared<const double>(-speed * material.rate)));
        } else {
            slice.speed = speed;
        }
        sampler->play(voice, std::move(slice));
    }

    m_hits[hit.slice].fetch_add(1, std::memory_order_relaxed);
    m_velocity[hit.slice].store(hit.velocity, std::memory_order_relaxed);
}

} // namespace MayaFlux::Kriya
