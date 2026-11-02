#pragma once

#include <MayaFlux/Buffers/BufferManager.hpp>
#include <MayaFlux/Kakshya/Source/DynamicSoundStream.hpp>
#include <MayaFlux/Kriya/TapSet.hpp>
#include <MayaFlux/Vruta/Scheduler.hpp>

namespace MayaFlux::Kriya {

/**
 * @struct LoopSpec
 * @brief How one loop plays.
 */
struct LoopSpec {
    double speed { 1.0 }; ///< Relative to the take. Negative plays backwards, zero holds.
    double level { 1.0 }; ///< Linear gain.
    double repeat { 0.0 }; ///< When above zero, restart every this many seconds: a stutter shorter than the take.
    std::vector<uint32_t> channels { 0, 1 }; ///< Output channels.
};

/**
 * @class Looper
 * @brief Loops takes with TapSet, and echoes a live ring with lag taps.
 *
 * Each loop is a TapSet over one take (for example from RingRecorder), looping its whole length. Loops
 * live in numbered slots that keep their number until removed, so a UI can hold on to them; a removed
 * slot is reused by the next add(). Echoes are lag taps over a ring that is still being written, which
 * play what was recorded that long ago.
 *
 * The buffer manager and scheduler are the caller's: whatever the application runs its buffers and
 * tasks on. Add and remove loops from any thread except the processing thread.
 */
class Looper {
public:
    /**
     * @param manager Buffer manager the taps are supplied through.
     * @param scheduler Scheduler the taps' pipelines run on.
     * @param buffer_size Engine buffer size in frames.
     */
    Looper(Buffers::BufferManager& manager, Vruta::TaskScheduler& scheduler, uint32_t buffer_size);

    ~Looper();

    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;

    /**
     * @brief Start looping @p take.
     * @return The slot it plays in, or nothing if the take is empty.
     */
    std::optional<size_t> add(std::shared_ptr<Kakshya::DynamicSoundStream> take, LoopSpec spec = {});

    /// Stop the loop in @p slot and free the slot.
    void remove(size_t slot);

    /// Stop every loop and echo.
    void clear();

    /// Change a loop's speed while it plays; the playhead carries on from where it is.
    void speed(size_t slot, double ratio);

    /// Change a loop's level.
    void level(size_t slot, double gain);

    [[nodiscard]] bool playing(size_t slot) const;

    /// Seconds the take in @p slot lasts at normal speed.
    [[nodiscard]] double length(size_t slot) const;

    /// Slots ever used, playing or free.
    [[nodiscard]] size_t slots() const { return m_loops.size(); }

    /// The TapSet behind @p slot, for region edits and routing. Null when the slot is free.
    [[nodiscard]] TapSet* taps(size_t slot);

    /**
     * @brief Play @p ring back @p seconds late, on @p channels, as a lag tap.
     *
     * @p ring must be a circular stream being filled with append_frames, such as RingRecorder::ring().
     * A Kinesis::TimeMap lag glides or wobbles in pitch as it changes.
     * @return Index of the echo, for echo_level().
     */
    size_t echo(const std::shared_ptr<Kakshya::DynamicSoundStream>& ring, Kinesis::TimeMap seconds,
        double level = 0.5, std::vector<uint32_t> channels = { 0, 1 });

    /// echo() with a fixed lag.
    size_t echo(const std::shared_ptr<Kakshya::DynamicSoundStream>& ring, double seconds,
        double level = 0.5, std::vector<uint32_t> channels = { 0, 1 });

    void echo_level(size_t index, double gain);

private:
    struct Loop {
        std::shared_ptr<Kakshya::DynamicSoundStream> take;
        TapSet taps;
        double seconds {};
    };

    Buffers::BufferManager& m_manager;
    Vruta::TaskScheduler& m_scheduler;
    uint32_t m_buffer_size;

    std::vector<std::optional<Loop>> m_loops;
    std::vector<TapSet> m_echoes;
    mutable std::mutex m_mutex;
};

} // namespace MayaFlux::Kriya
