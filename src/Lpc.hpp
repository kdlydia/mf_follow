#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace MayaFlux::Kinesis::Discrete {

/**
 * @brief Linear prediction coefficients from an autocorrelation, by the Levinson-Durbin recursion.
 *
 * Grows the predictor one order at a time, so the cost is order squared. The result is the denominator
 * of an all-pole filter whose spectrum follows the spectral envelope of the signal the autocorrelation
 * came from.
 *
 * @param autocorrelation Lags 0, 1, 2 ... Lag zero is the signal energy and must be positive. A short
 *        input is padded with zeros up to order + 1 lags.
 * @param order Number of poles.
 * @return Coefficients a[0..order] with a[0] = 1, in the convention A(z) = 1 + a1 z^-1 + ... + ap z^-p.
 *         All zeros past a[0] if the energy is not positive.
 */
[[nodiscard]] std::vector<double> levinson_durbin(std::span<const double> autocorrelation, size_t order);

/**
 * @brief Spectrum of an all-pole filter in dB, from 0 Hz up to Nyquist.
 *
 * Evaluates -10 log10 |A(e^jw)|^2 at evenly spaced frequencies, so the peaks are where the filter
 * resonates.
 *
 * @param a Coefficients from levinson_durbin().
 * @param points Number of frequencies, at least 2. Point 0 is 0 Hz and the last point is Nyquist.
 */
[[nodiscard]] std::vector<double> lpc_envelope_db(std::span<const double> a, size_t points);

/**
 * @brief Local maxima of a sampled curve, refined to between the samples.
 *
 * Each interior sample higher than its left neighbour and at least as high as its right one is a peak.
 * A parabola through the peak and its two neighbours gives the position to a fraction of a sample.
 *
 * @param curve Sampled values, evenly spaced.
 * @param span_hz Frequency covered from the first sample to the last, so the result is in Hz.
 * @return Peak positions in Hz, ascending.
 */
[[nodiscard]] std::vector<double> spectral_peaks(std::span<const double> curve, double span_hz);

} // namespace MayaFlux::Kinesis::Discrete
