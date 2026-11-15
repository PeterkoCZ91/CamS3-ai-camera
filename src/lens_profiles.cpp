#include "lens_profiles.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

#define LENS_PROFILES_FILE "/lens_profiles.json"

// Loaded lazily and cached: the file is tiny and only changes through this module.
// All callers are async-web handlers running on one task, so no locking is needed.
static LensProfile profiles[LENS_PROFILE_MAX];
static int  profileCount = 0;
static bool loaded = false;
static char activeName[LENS_PROFILE_NAME_LEN] = {0};

static void ensureLoaded() {
    if (loaded) return;
    loaded = true;
    profileCount = 0;
    File f = LittleFS.open(LENS_PROFILES_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;   // corrupt file = start empty rather than refuse to boot features
    for (JsonObject o : doc.as<JsonArray>()) {
        if (profileCount >= LENS_PROFILE_MAX) break;
        char clean[LENS_PROFILE_NAME_LEN];
        // Re-sanitize: the file is plain text on flash and could be hand-edited.
        if (!lensProfileSanitizeName(o["name"] | "", clean)) continue;
        LensProfile& p = profiles[profileCount++];
        strlcpy(p.name, clean, sizeof(p.name));
        p.fov       = constrain((int)(o["fov"] | 0), 0, 220);
        p.lenc      = o["lenc"] | true;
        p.sharpness = constrain((int)(o["sharpness"] | 0), -3, 3);
    }
}

static bool persist() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < profileCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = profiles[i].name;
        o["fov"] = profiles[i].fov;
        o["lenc"] = profiles[i].lenc;
        o["sharpness"] = profiles[i].sharpness;
    }
    // Atomic replace: write a .tmp, then rename over the real file, so a power cut
    // mid-write cannot leave a truncated profiles file.
    const char* tmpPath = LENS_PROFILES_FILE ".tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) return false;
    bool ok = serializeJson(doc, f) > 0;
    f.close();
    if (!ok) { LittleFS.remove(tmpPath); return false; }
    // lfs_rename replaces an existing destination atomically.
    if (!LittleFS.rename(tmpPath, LENS_PROFILES_FILE)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return true;
}

static int indexOf(const char* name) {
    for (int i = 0; i < profileCount; i++)
        if (strcmp(profiles[i].name, name) == 0) return i;
    return -1;
}

bool lensProfileSanitizeName(const char* in, char* out) {
    // Restricted charset keeps names safe to echo into JSON/HTML and to compare with strcmp.
    size_t n = 0;
    for (const char* c = in; c && *c && n < LENS_PROFILE_NAME_LEN - 1; c++) {
        char ch = *c;
        bool okc = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                   ch == ' ' || ch == '_' || ch == '-' || ch == '.';
        if (!okc) continue;
        if (ch == ' ' && n == 0) continue;   // no leading space
        out[n++] = ch;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return n > 0;
}

int lensProfileCount() { ensureLoaded(); return profileCount; }

bool lensProfileGet(int index, LensProfile& out) {
    ensureLoaded();
    if (index < 0 || index >= profileCount) return false;
    out = profiles[index];
    return true;
}

bool lensProfileFind(const char* name, LensProfile& out) {
    ensureLoaded();
    char clean[LENS_PROFILE_NAME_LEN];
    if (!lensProfileSanitizeName(name, clean)) return false;
    int i = indexOf(clean);
    if (i < 0) return false;
    out = profiles[i];
    return true;
}

LensProfileResult lensProfileSave(const LensProfile& in) {
    ensureLoaded();
    LensProfile p = in;
    if (!lensProfileSanitizeName(in.name, p.name)) return LENS_PROFILE_BAD_NAME;
    int i = indexOf(p.name);
    LensProfile backup;
    bool existed = (i >= 0);
    if (!existed) {
        if (profileCount >= LENS_PROFILE_MAX) return LENS_PROFILE_FULL;
        i = profileCount++;
    } else {
        backup = profiles[i];
    }
    profiles[i] = p;
    if (!persist()) {
        // Keep RAM and flash consistent if the write failed.
        if (existed) profiles[i] = backup; else profileCount--;
        return LENS_PROFILE_IO;
    }
    return LENS_PROFILE_OK;
}

bool lensProfileDelete(const char* name) {
    ensureLoaded();
    char clean[LENS_PROFILE_NAME_LEN];
    if (!lensProfileSanitizeName(name, clean)) return false;
    int i = indexOf(clean);
    if (i < 0) return false;
    LensProfile removed = profiles[i];
    for (int j = i; j < profileCount - 1; j++) profiles[j] = profiles[j + 1];
    profileCount--;
    if (!persist()) {
        for (int j = profileCount; j > i; j--) profiles[j] = profiles[j - 1];
        profiles[i] = removed;
        profileCount++;
        return false;
    }
    if (strcmp(activeName, clean) == 0) activeName[0] = '\0';
    return true;
}

const char* lensProfileActive() { return activeName; }
void lensProfileSetActive(const char* name) { strlcpy(activeName, name ? name : "", sizeof(activeName)); }
void lensProfileClearActive() { activeName[0] = '\0'; }
