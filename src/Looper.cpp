#include "Looper.hpp"

namespace MayaFlux::Kriya {

Looper::Looper(Buffers::BufferManager& manager, Vruta::TaskScheduler& scheduler, uint32_t buffer_size)
    : m_manager(manager)
    , m_scheduler(scheduler)
    , m_buffer_size(buffer_size)
{
}

Looper::~Looper()
{
    clear();
}

std::optional<size_t> Looper::add(std::shared_ptr<Kakshya::DynamicSoundStream> take, LoopSpec spec)
{
    if (!take || take->get_num_frames() == 0)
        return std::nullopt;

    TapSetBuilder builder(take, m_manager, m_scheduler, m_buffer_size);
    builder.tap()
        .speed(std::abs(spec.speed))
        .backward(spec.speed < 0.0)
        .level(spec.level)
        .on_channels(spec.channels)
        .loop();
    if (spec.repeat > 0.0)
        builder.repeat_every(spec.repeat);

    Loop loop {
        .take = take,
        .taps = builder.start(),
        .seconds = static_cast<double>(take->get_num_frames()) / static_cast<double>(take->get_sample_rate()),
    };

    std::lock_guard lock(m_mutex);
    for (size_t slot = 0; slot < m_loops.size(); ++slot) {
        if (!m_loops[slot]) {
            m_loops[slot] = std::move(loop);
            return slot;
        }
    }
    m_loops.emplace_back(std::move(loop));
    return m_loops.size() - 1;
}

void Looper::remove(size_t slot)
{
    std::lock_guard lock(m_mutex);
    if (slot < m_loops.size() && m_loops[slot]) {
        m_loops[slot]->taps.stop();
        m_loops[slot].reset();
    }
}

void Looper::clear()
{
    std::lock_guard lock(m_mutex);
    for (auto& loop : m_loops) {
        if (loop) {
            loop->taps.stop();
            loop.reset();
        }
    }
    for (auto& echo : m_echoes)
        echo.stop();
    m_echoes.clear();
}

void Looper::speed(size_t slot, double ratio)
{
    std::lock_guard lock(m_mutex);
    if (slot < m_loops.size() && m_loops[slot])
        m_loops[slot]->taps.speed(0, ratio);
}

void Looper::level(size_t slot, double gain)
{
    std::lock_guard lock(m_mutex);
    if (slot < m_loops.size() && m_loops[slot])
        m_loops[slot]->taps.level(0, gain);
}

bool Looper::playing(size_t slot) const
{
    std::lock_guard lock(m_mutex);
    return slot < m_loops.size() && m_loops[slot].has_value();
}

double Looper::length(size_t slot) const
{
    std::lock_guard lock(m_mutex);
    return slot < m_loops.size() && m_loops[slot] ? m_loops[slot]->seconds : 0.0;
}

TapSet* Looper::taps(size_t slot)
{
    std::lock_guard lock(m_mutex);
    return slot < m_loops.size() && m_loops[slot] ? &m_loops[slot]->taps : nullptr;
}

size_t Looper::echo(const std::shared_ptr<Kakshya::DynamicSoundStream>& ring, Kinesis::TimeMap seconds,
    double level, std::vector<uint32_t> channels)
{
    TapSetBuilder builder(ring, m_manager, m_scheduler, m_buffer_size);
    builder.tap().lag(std::move(seconds)).level(level).on_channels(channels);
    auto taps = builder.start();

    std::lock_guard lock(m_mutex);
    m_echoes.push_back(std::move(taps));
    return m_echoes.size() - 1;
}

size_t Looper::echo(const std::shared_ptr<Kakshya::DynamicSoundStream>& ring, double seconds,
    double level, std::vector<uint32_t> channels)
{
    TapSetBuilder builder(ring, m_manager, m_scheduler, m_buffer_size);
    builder.tap().lag(seconds).level(level).on_channels(channels);
    auto taps = builder.start();

    std::lock_guard lock(m_mutex);
    m_echoes.push_back(std::move(taps));
    return m_echoes.size() - 1;
}

void Looper::echo_level(size_t index, double gain)
{
    std::lock_guard lock(m_mutex);
    if (index < m_echoes.size())
        m_echoes[index].level(0, gain);
}

} // namespace MayaFlux::Kriya
