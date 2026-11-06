#ifndef FOCUS_SCORE_H
#define FOCUS_SCORE_H

#include <stdint.h>

// On-demand focus/sharpness score (variance of the 4-neighbour Laplacian over the
// centre crop of the grayscale frame). Costs nothing until focusScoreTouch() is
// called; it then keeps computing every ~500 ms for 10 s after the last touch.
struct FocusScore {
    float    score;   // latest Laplacian variance
    float    peak;    // max since the session's first request (or last reset)
    uint32_t age_ms;  // time since `score` was computed
    bool     valid;   // false until the first sample of the session exists
};

// Mark the score as wanted now (starts the worker lazily). Never blocks.
void focusScoreTouch(bool resetPeak);
// Copy the latest result. Safe from async handlers (no decode, no waiting).
FocusScore focusScoreGet();

#endif
