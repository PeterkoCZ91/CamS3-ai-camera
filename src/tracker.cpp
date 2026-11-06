#include "tracker.h"
#include "ws_log.h"
#include "config.h"
#include <string.h>
#include <math.h>

static const char* TAG = "Tracker";

static Track tracks[TRACKER_MAX_TRACKS];
static int trackCount = 0;
static int nextTrackId = 1;

void trackerInit() {
    trackerReset();
    logCapture("[%s] Tracker initialized (max %d tracks)\n", TAG, TRACKER_MAX_TRACKS);
}

void trackerReset() {
    trackCount = 0;
    nextTrackId = 1;
    memset(tracks, 0, sizeof(tracks));
}

// Squared Euclidean distance between track and detection centroids
static int distSq(const Track& t, const Detection& d) {
    int dx = t.x - d.x;
    int dy = t.y - d.y;
    return dx * dx + dy * dy;
}

// Remove deleted tracks by compacting the array
static void compactTracks() {
    int write = 0;
    for (int read = 0; read < trackCount; read++) {
        if (tracks[read].state != TRACK_DELETED) {
            if (write != read) {
                tracks[write] = tracks[read];
            }
            write++;
        }
    }
    trackCount = write;
}

// Pixel thresholds were tuned on a reference lens. A wider lens shrinks objects
// on the sensor (a narrower one enlarges them) roughly in proportion to
// tan(fov/2), so the same physical motion spans fewer/more pixels. Opt-in:
// either FOV being 0 (unknown/off) returns exactly 1.0, keeping old behaviour.
float lensScale() {
    int fov = appConfig.lens_fov_deg;
    int ref = appConfig.lens_ref_fov_deg;
    if (fov <= 0 || ref <= 0) return 1.0f;
    // tan() blows up at 180 deg (and the settings accept up to 220), so cap the
    // angles where a rectilinear projection still means something.
    if (fov > 170) fov = 170;
    if (ref > 170) ref = 170;
    const float k = 0.01745329252f * 0.5f;  // deg -> rad, halved
    float s = tanf(ref * k) / tanf(fov * k);
    if (!(s >= 0.5f)) s = 0.5f;             // also catches NaN
    if (s > 2.0f) s = 2.0f;
    return s;
}

int trackerUpdate(const Detection* detections, int numDetections) {
    if (numDetections > TRACKER_MAX_DETECTIONS) {
        numDetections = TRACKER_MAX_DETECTIONS;
    }

    unsigned long now = millis();
    int matchDist   = appConfig.tracker.match_dist   > 0 ? appConfig.tracker.match_dist   : TRACKER_MATCH_DIST_DEFAULT;
    int confirmHits = appConfig.tracker.confirm_hits > 0 ? appConfig.tracker.confirm_hits : TRACKER_CONFIRM_HITS_DEFAULT;
    int maxMisses   = appConfig.tracker.max_misses   > 0 ? appConfig.tracker.max_misses   : TRACKER_MAX_MISSES_DEFAULT;
    // Scale only when opted in; the guard keeps the default path integer-exact.
    float ls = lensScale();
    if (ls != 1.0f) matchDist = (int)(matchDist * ls + 0.5f);
    int maxDistSq = matchDist * matchDist;

    // --- Greedy nearest-neighbor matching ---
    bool trackMatched[TRACKER_MAX_TRACKS] = {};
    bool detMatched[TRACKER_MAX_DETECTIONS] = {};

    // Build cost matrix and match greedily (closest pairs first)
    // Simple O(N*M) approach — fine for small N, M
    for (int pass = 0; pass < min(trackCount, numDetections); pass++) {
        int bestT = -1, bestD = -1;
        int bestDist = maxDistSq + 1;

        for (int t = 0; t < trackCount; t++) {
            if (trackMatched[t] || tracks[t].state == TRACK_DELETED) continue;
            for (int d = 0; d < numDetections; d++) {
                if (detMatched[d]) continue;
                int dist = distSq(tracks[t], detections[d]);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestT = t;
                    bestD = d;
                }
            }
        }

        if (bestT < 0 || bestDist > maxDistSq) break;

        // Match found
        trackMatched[bestT] = true;
        detMatched[bestD] = true;

        Track& tr = tracks[bestT];
        const Detection& det = detections[bestD];
        tr.x = det.x;
        tr.y = det.y;
        tr.w = det.w;
        tr.h = det.h;
        tr.score = det.score;
        tr.age++;
        tr.hits++;
        tr.misses = 0;
        tr.lastSeen = now;

        // State transitions
        if (tr.state == TRACK_TENTATIVE && tr.hits >= confirmHits) {
            tr.state = TRACK_CONFIRMED;
        } else if (tr.state == TRACK_LOST) {
            tr.state = TRACK_CONFIRMED;
            tr.hits = 1;
        }
    }

    // --- Handle unmatched tracks ---
    for (int t = 0; t < trackCount; t++) {
        if (trackMatched[t] || tracks[t].state == TRACK_DELETED) continue;

        Track& tr = tracks[t];
        tr.age++;
        tr.misses++;
        tr.hits = 0;

        if (tr.misses >= maxMisses) {
            tr.state = TRACK_DELETED;
        } else if (tr.state == TRACK_CONFIRMED) {
            tr.state = TRACK_LOST;
        } else if (tr.state == TRACK_TENTATIVE && tr.misses >= 2) {
            tr.state = TRACK_DELETED;
        }
    }

    // --- Create new tracks for unmatched detections ---
    for (int d = 0; d < numDetections; d++) {
        if (detMatched[d]) continue;
        if (trackCount >= TRACKER_MAX_TRACKS) break;

        Track& tr = tracks[trackCount++];
        memset(&tr, 0, sizeof(Track));
        tr.id = nextTrackId++;
        tr.x = detections[d].x;
        tr.y = detections[d].y;
        tr.w = detections[d].w;
        tr.h = detections[d].h;
        tr.score = detections[d].score;
        tr.state = TRACK_TENTATIVE;
        tr.age = 1;
        tr.hits = 1;
        tr.misses = 0;
        tr.lastSeen = now;
        tr.notified = false;
    }

    // --- Compact deleted tracks ---
    compactTracks();

    // Count confirmed
    int confirmed = 0;
    for (int t = 0; t < trackCount; t++) {
        if (tracks[t].state == TRACK_CONFIRMED) confirmed++;
    }
    return confirmed;
}

const Track* trackerGetTracks(int* count) {
    if (count) *count = trackCount;
    return tracks;
}

int trackerGetConfirmedCount() {
    int count = 0;
    for (int t = 0; t < trackCount; t++) {
        if (tracks[t].state == TRACK_CONFIRMED) count++;
    }
    return count;
}

int trackerGetNewlyConfirmed(const Track** outTracks, int maxOut) {
    int count = 0;
    for (int t = 0; t < trackCount && count < maxOut; t++) {
        if (tracks[t].state == TRACK_CONFIRMED && !tracks[t].notified) {
            outTracks[count++] = &tracks[t];
        }
    }
    return count;
}

void trackerMarkNotified(int trackId) {
    for (int t = 0; t < trackCount; t++) {
        if (tracks[t].id == trackId) {
            tracks[t].notified = true;
            return;
        }
    }
}
