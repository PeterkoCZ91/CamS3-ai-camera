#ifndef TRACKER_H
#define TRACKER_H

#include <Arduino.h>

// Tracker configuration
#define TRACKER_MAX_TRACKS      16
#define TRACKER_MAX_DETECTIONS  16
#define TRACKER_CONFIRM_HITS    3    // Hits to confirm a track
#define TRACKER_MAX_MISSES      5    // Misses before deletion
#define TRACKER_MATCH_DIST      40   // Max pixel distance for matching (64x64 space)

enum TrackState {
    TRACK_TENTATIVE = 0,
    TRACK_CONFIRMED,
    TRACK_LOST,
    TRACK_DELETED
};

struct Detection {
    int x;          // Centroid X (in FOMO grid coords)
    int y;          // Centroid Y
    int w;          // Bounding box width
    int h;          // Bounding box height
    float score;    // Confidence
    int label;      // Class label (0 = person)
};

struct Track {
    int id;
    int x, y;               // Last known centroid
    int w, h;                // Last known bbox size
    float score;
    TrackState state;
    int age;                 // Total frames since creation
    int hits;                // Consecutive matched frames
    int misses;              // Consecutive unmatched frames
    unsigned long lastSeen;  // millis() of last match
    bool notified;           // Telegram notification sent for this track
};

// Initialize tracker (call once)
void trackerInit();

// Reset all tracks
void trackerReset();

// Update tracker with new detections. Returns number of confirmed tracks.
int trackerUpdate(const Detection* detections, int numDetections);

// Get all tracks (includes all states)
const Track* trackerGetTracks(int* count);

// Get count of currently confirmed tracks
int trackerGetConfirmedCount();

// Get count of newly confirmed tracks (transitioned this frame, not yet notified)
int trackerGetNewlyConfirmed(const Track** outTracks, int maxOut);

// Mark a track as notified (to prevent duplicate Telegram messages)
void trackerMarkNotified(int trackId);

#endif // TRACKER_H
