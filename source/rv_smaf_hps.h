#pragma once
#include <stddef.h>
#include <stdint.h>

// Yamaha SMAF Handy Phone Standard (FormatType 0) score track support.
// Each MTR track contains four independent logical channels. This unit has no
// NDS dependencies so its parser can be regression-tested on the host.
struct RvSmafHpsNote {
    uint32_t start_ms;
    uint32_t gate_ms;
    uint8_t note;           // MIDI note number after octaves and octave-shift
    uint8_t channel;        // 0..3 within the score track
    uint8_t velocity;
    uint8_t program;
    uint8_t volume;
    uint8_t expression;
    uint8_t rhythm;
};

typedef void (*RvSmafHpsNoteFn)(void *ctx, const RvSmafHpsNote *note);

// Pass a NULL callback to scan duration before PCM allocation. Each track
// begins at t=0, allowing all of an MMF's MTR chunks to mix in sync.
// Returns false for malformed or unsupported score-track data.
bool rv_smaf_hps_parse_track(const uint8_t *mtr, size_t len,
                             RvSmafHpsNoteFn callback, void *ctx,
                             uint32_t *out_end_ms, uint32_t *out_notes);
