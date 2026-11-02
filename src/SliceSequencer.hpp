#pragma once

#include <MayaFlux/Buffers/BufferManager.hpp>
#include <MayaFlux/Kakshya/Source/DynamicSoundStream.hpp>
#include <MayaFlux/Kriya/SamplingPipeline.hpp>
#include <MayaFlux/Transitive/Memory/SnapshotCell.hpp>
#include <MayaFlux/Transitive/Memory/SweepList.hpp>
#include <MayaFlux/Vruta/Routine.hpp>
#include <MayaFlux/Vruta/Scheduler.hpp>

namespace MayaFlux::Kriya {

/**
 * @struct SliceSequencerConfig
 * @brief The shape of a SliceSequencer's grid and where it plays.
 */
struct SliceSequencerConfig {
    size_t slices { 4 }; ///< Equal chops of the take, one grid lane each.
    size_t steps { 16 }; ///< Steps per bar.
    double bpm { 110.0 }; ///< Quarter notes per minute; a step is a sixteenth.
    double swing { 0.0 }; ///< 0..0.5: how much later every second step lands.
    double gate { 1.0 }; ///< 0.05..1: how much of its chop a hit plays.
    std::vector<uint32_t> channels { 0, 1 }; ///< Output channels.
    uint32_t buffer_size { 512 }; ///< Engine buffer size in frames.
    size_t voices { 8 }; ///< Hits that can ring over each other.
};

/**
 * @struct SliceHit
 * @brief One hit of one chop.
 */
struct SliceHit {
    size_t slice {};
    double velocity { 1.0 }; ///< 0..1.
    double speed { 1.0 }; ///< Playback rate: pitch. Above zero.
    bool reverse { false }; ///< Play the chop from its end to its start.
};

/**
 * @class SliceSequencer
 * @brief Cuts a take into chops and plays them in time: a groove made of a voice.
 *
 * The take is split into equal chops. Hits come from two places, both landing on the step clock:
 * a step grid that gives every chop a velocity per step (zero is a rest) with a pitch per step, and
 * a queue that anything can push hits onto from any thread, played on the next step, so gestures,
 * spatial events or other patterns are quantised into the groove.
 *
 * A Kriya::pattern runs the clock on the scheduler. Its durations come from a source called once
 * per bar, so tempo and swing changes land at the next bar line, and grid edits at the next step.
 * Hits play on Kriya::SamplingPipeline voices, rotated so a hit does not cut off the one before.
 * A reversed hit reads its chop through a Kinesis::TimeMaps::integrated map with a negative velocity,
 * the way TapSet plays backwards.
 *
 * The take can be replaced while it plays: the material lives in a Memory::SnapshotCell and the
 * pattern reads it under a guard. The queue is a Memory::SweepList. Grid, pitch, tempo and the rest
 * are atomics. The buffer manager and scheduler are the caller's.
 */
class SliceSequencer {
public:
    SliceSequencer(Buffers::BufferManager& manager, Vruta::TaskScheduler& scheduler, SliceSequencerConfig config = {});
    ~SliceSequencer();

    SliceSequencer(const SliceSequencer&) = delete;
    SliceSequencer& operator=(const SliceSequencer&) = delete;

    /// Play from @p take from now on. Any thread but the processing thread. Null clears it.
    void set_take(std::shared_ptr<Kakshya::DynamicSoundStream> take);

    [[nodiscard]] bool has_take() const { return m_take_frames.load(std::memory_order_relaxed) > 0; }
    [[nodiscard]] double take_seconds() const { return m_take_seconds.load(std::memory_order_relaxed); }

    /// Velocity 0..1 for @p slice at @p step. Zero is a rest.
    void set_hit(size_t slice, size_t step, double velocity);
    [[nodiscard]] double hit(size_t slice, size_t step) const;

    /// Pitch ratio for the grid's hits on @p step: 1 is the take as recorded, 2 an octave up.
    void set_pitch(size_t step, double ratio);
    [[nodiscard]] double pitch(size_t step) const;

    /// Whether the grid plays @p slice backwards.
    void set_reverse(size_t slice, bool reverse);
    [[nodiscard]] bool reverse(size_t slice) const;

    /// Play @p hit on the next step. Any thread.
    void queue(SliceHit hit) { m_queue.push(hit); }

    void set_tempo(double bpm) { m_bpm.store(std::clamp(bpm, 20.0, 400.0), std::memory_order_relaxed); }
    void set_swing(double swing) { m_swing.store(std::clamp(swing, 0.0, 0.5), std::memory_order_relaxed); }
    void set_gate(double gate) { m_gate.store(std::clamp(gate, 0.05, 1.0), std::memory_order_relaxed); }
    void set_level(double level) { m_level.store(std::max(level, 0.0), std::memory_order_relaxed); }

    [[nodiscard]] double get_tempo() const { return m_bpm.load(std::memory_order_relaxed); }

    /// Seconds per step at the current tempo, without swing.
    [[nodiscard]] double step_seconds() const { return 15.0 / get_tempo(); }

    /// Start the clock from step 0. Does nothing if it is already running.
    void start();
    void stop();
    [[nodiscard]] bool running() const { return m_routine != nullptr; }

    /// The step that played last.
    [[nodiscard]] size_t current_step() const { return m_step.load(std::memory_order_relaxed); }

    /// Hits @p slice has played so far; compare with an earlier value to see a new one.
    [[nodiscard]] uint64_t hits(size_t slice) const;

    /// Velocity of the last hit of @p slice.
    [[nodiscard]] double last_velocity(size_t slice) const;

    [[nodiscard]] size_t slices() const { return m_config.slices; }
    [[nodiscard]] size_t steps() const { return m_config.steps; }

    /// Frame range [first, last) of @p slice within the take, for drawing the cuts.
    [[nodiscard]] std::pair<uint64_t, uint64_t> slice_frames(size_t slice) const;

private:
    struct Material {
        std::shared_ptr<Kakshya::DynamicSoundStream> take;
        uint64_t frames {};
        double rate {};
        std::vector<std::shared_ptr<SamplingPipeline>> samplers;
    };

    Buffers::BufferManager& m_manager;
    Vruta::TaskScheduler& m_scheduler;
    SliceSequencerConfig m_config;

    Memory::SnapshotCell<std::shared_ptr<Material>> m_material;
    std::atomic<double> m_take_seconds { 0.0 };
    std::atomic<uint64_t> m_take_frames { 0 };

    std::vector<std::atomic<double>> m_grid;
    std::vector<std::atomic<double>> m_pitch;
    std::vector<std::atomic<bool>> m_reverse;
    std::vector<std::atomic<uint64_t>> m_hits;
    std::vector<std::atomic<double>> m_velocity;
    Memory::SweepList<SliceHit> m_queue;

    std::atomic<double> m_bpm;
    std::atomic<double> m_swing;
    std::atomic<double> m_gate;
    std::atomic<double> m_level { 1.0 };
    std::atomic<size_t> m_step { 0 };
    size_t m_next_voice { 0 };

    std::shared_ptr<Vruta::Routine> m_routine;

    void play_step(size_t step);
    void play(const Material& material, const SliceHit& hit);
    [[nodiscard]] std::vector<double> bar_durations() const;
    [[nodiscard]] std::shared_ptr<Material> build(const std::shared_ptr<Kakshya::DynamicSoundStream>& take) const;
};

} // namespace MayaFlux::Kriya
