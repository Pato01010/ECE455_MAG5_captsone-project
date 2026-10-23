/* noise.h - how much denser the noise is near the fitted frequency than white noise would be.
 *
 * fit_record()'s f_sigma_hz assumes white noise: the residual's variance spread evenly from 0 to fs/2.
 * The chain's noise is band-limited (0.8-3 kHz), so near f it is several times denser and the real
 * scatter of f is about 2x f_sigma_hz (tests/fit). noise_density_ratio() measures that density from
 * the residual itself; f_sigma_hz x sqrt(ratio) is then the honest one-sigma error.
 */
#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

#include "fit.h"

/* Noise density of the fit's residual at NOISE_BINS frequencies around f (+/-15 to +/-60 Hz, clear of
 * the decay's own line), over the density white noise of the same variance would have. About 1 for
 * white noise, about (fs/2) / bandwidth for band-limited noise. Returns a negative value if the fit
 * cannot be used. */
double noise_density_ratio(const uint16_t *x, uint32_t n, double fs_hz, const fit_result_t *fit);

#define NOISE_BINS 20

#endif
