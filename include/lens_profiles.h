#ifndef LENS_PROFILES_H
#define LENS_PROFILES_H

#include <Arduino.h>

// Named snapshots of the settings that depend on the fitted lens, so swapping a
// lens is one action. Only user-saved snapshots exist; no presets are shipped.
#define LENS_PROFILE_MAX      4
#define LENS_PROFILE_NAME_LEN 17   // 16 chars + NUL

struct LensProfile {
    char name[LENS_PROFILE_NAME_LEN];
    int  fov;         // appConfig.lens_fov_deg
    bool lenc;        // appConfig.camera.lenc
    int  sharpness;   // appConfig.camera.sharpness
};

enum LensProfileResult { LENS_PROFILE_OK, LENS_PROFILE_BAD_NAME, LENS_PROFILE_FULL, LENS_PROFILE_IO };

// Strips to [A-Za-z0-9 _.-], trims, truncates to 16. Returns false if nothing is left.
bool lensProfileSanitizeName(const char* in, char* out);

int  lensProfileCount();
bool lensProfileGet(int index, LensProfile& out);
bool lensProfileFind(const char* name, LensProfile& out);
LensProfileResult lensProfileSave(const LensProfile& p);   // same name overwrites
bool lensProfileDelete(const char* name);

// "Active" = last applied profile. RAM only: it is a hint for the UI and must not
// outlive a manual edit of any of the three values (caller clears it then).
const char* lensProfileActive();
void lensProfileSetActive(const char* name);
void lensProfileClearActive();

#endif
