#pragma once

#include "FormantProcessor.hpp"

#include <MayaFlux/Nodes/Conduit/Constant.hpp>
#include <MayaFlux/Nodes/Network/RelationNetwork.hpp>
#include <MayaFlux/Nodes/Network/ResonatorNetwork.hpp>

namespace MayaFlux::Nodes::Network {

/**
 * @struct FormantTransferConfig
 * @brief Starting values for a FormantTransfer. All but the Q values can be changed while it runs.
 */
struct FormantTransferConfig {
    double sample_rate { 48000.0 }; ///< Rate the networks run at.
    double glide { 0.04 }; ///< Seconds for the resonators to cover 1 - 1/e of a jump between formants.
    double shift { 1.0 }; ///< Ratio applied to every tracked formant: below 1 sounds bigger, above 1 smaller.
    double sensitivity { 12.0 }; ///< Output scale per unit of microphone RMS, before clamping to 1.
    double unvoiced { 0.25 }; ///< Fraction of the loudness kept while the frame is not voiced.
    std::array<double, 3> start_hz { 500.0, 1500.0, 2500.0 }; ///< Formants before the first voiced frame: a neutral vowel.
    std::array<double, 3> q { 8.0, 10.0, 14.0 }; ///< Resonator Q per formant.
};

/**
 * @class FormantTransfer
 * @brief Speaks with your vowels through another sound: a talk-box.
 *
 * A FormantProcessor follows the formants of a voice. A RelationNetwork turns each reading into a
 * decision: a Step writes the shifted formants and the loudness into its slots and switches a voiced
 * slot, Derive operators running Kinesis::Discrete::envelope_follow glide the formants and smooth the
 * loudness, and on_enter / on_leave of the voiced slot are events. A ResonatorNetwork, excited by any
 * carrier node, is retuned from the relation's first three slots every block ("retune" keeps the ring)
 * and its output scale follows the smoothed loudness.
 *
 * Nothing is registered: route relation() and voice() wherever the application runs its networks,
 * for example `relation() | Audio[0]` and `voice() | Audio[{0, 1}]` with the engine's graph. The
 * relation publishes no audio of its own.
 *
 * Slot layout of relation(), kept stable so it can be mapped elsewhere:
 *   0..2  F1..F3 as the resonators hear them (Hz, glided)
 *   3..5  F1..F3 as tracked and shifted (Hz)
 *   6     loudness, raw (RMS times sensitivity, clamped to 1)
 *   7     loudness, smoothed
 *   8     voiced (active while the voice gate says voice)
 */
class FormantTransfer {
public:
    enum Slot : uint32_t {
        F1 = 0,
        F2,
        F3,
        RAW_F1,
        RAW_F2,
        RAW_F3,
        LOUDNESS_RAW,
        LOUDNESS,
        VOICED,
    };

    /**
     * @param formants The tracker to follow, typically from FormantProcessor::listen().
     * @param carrier What the resonators shape. A GlottalSource sounds like a voice; anything rich works.
     * @param config Starting values.
     */
    FormantTransfer(std::shared_ptr<Buffers::FormantProcessor> formants,
        std::shared_ptr<Node> carrier,
        FormantTransferConfig config = {});

    [[nodiscard]] const std::shared_ptr<RelationNetwork>& relation() const { return m_relation; }
    [[nodiscard]] const std::shared_ptr<ResonatorNetwork>& voice() const { return m_voice; }

    void set_glide(double seconds) { m_controls->glide.store(std::max(seconds, 0.0), std::memory_order_relaxed); }
    void set_shift(double ratio) { m_controls->shift.store(std::max(ratio, 0.01), std::memory_order_relaxed); }
    void set_sensitivity(double per_rms) { m_controls->sensitivity.store(std::max(per_rms, 0.0), std::memory_order_relaxed); }
    void set_unvoiced(double fraction) { m_controls->unvoiced.store(std::clamp(fraction, 0.0, 1.0), std::memory_order_relaxed); }

    [[nodiscard]] double get_glide() const { return m_controls->glide.load(std::memory_order_relaxed); }
    [[nodiscard]] double get_shift() const { return m_controls->shift.load(std::memory_order_relaxed); }

    /**
     * @brief Speak a vowel chosen by hand instead of the tracked one.
     *
     * While manual, the relation takes F1..F3 from here (still shifted and glided), counts as voiced
     * and holds the loudness at @p loudness, so a vowel can be drawn with no voice at all. Any thread.
     */
    void set_manual(std::array<double, 3> formants_hz, double loudness = 0.8);

    /** @brief Return to following the tracker. */
    void clear_manual() { m_controls->manual.store(false, std::memory_order_release); }

    [[nodiscard]] bool is_manual() const { return m_controls->manual.load(std::memory_order_acquire); }

    /** @brief Formant @p index (0..2) as the resonators currently hear it, in Hz. */
    [[nodiscard]] double formant(size_t index) const;

    /** @brief Smoothed loudness, 0..1. */
    [[nodiscard]] double loudness() const;

    /** @brief Whether the voice gate currently says voice. */
    [[nodiscard]] bool voiced() const;

    /**
     * @brief Run @p callback when the voice starts (or stops, with @p on_stop).
     *
     * Runs on the processing thread, inside the relation's block. Add callbacks before the
     * relation starts running.
     */
    void on_voice(std::function<void()> callback, bool on_stop = false);

private:
    struct Controls {
        std::atomic<double> glide;
        std::atomic<double> shift;
        std::atomic<double> sensitivity;
        std::atomic<double> unvoiced;
        std::atomic<bool> manual { false };
        std::array<std::atomic<double>, 3> manual_hz {};
        std::atomic<double> manual_loudness { 0.8 };
    };

    std::shared_ptr<Buffers::FormantProcessor> m_formants;
    std::shared_ptr<Controls> m_controls;
    std::shared_ptr<Constant> m_scale;
    std::shared_ptr<RelationNetwork> m_relation;
    std::shared_ptr<ResonatorNetwork> m_voice;
};

} // namespace MayaFlux::Nodes::Network
