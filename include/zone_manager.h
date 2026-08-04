#ifndef ZONE_MANAGER_H
#define ZONE_MANAGER_H

#include <Arduino.h>

#define ZONE_MAX_COUNT  8
#define ZONE_MAX_RECTS  4
#define ZONE_NAME_LEN  24

struct ZoneRect { int x, y, w, h; };

struct Zone {
    char name[ZONE_NAME_LEN];
    char color[8];   // "#rrggbb"
    bool alert;      // fire event/Telegram when motion hits this zone
    int  rect_count;
    ZoneRect rects[ZONE_MAX_RECTS];
};

void    initZoneManager();
bool    saveZones();

int     getZoneCount();
Zone*   getZone(int index);
bool    addOrUpdateZone(const Zone& z);
bool    deleteZone(const char* name);
void    clearZones();

// Fills out_buf with comma-separated names of alert zones that contain motion.
// motion_grid is the MOTION_GRID_SIZE uint8_t diffMask; gw/gh are the grid dims.
void    getActiveZones(const uint8_t* motion_grid, int gw, int gh,
                       char* out_buf, int buf_len);

String  getZonesJSON();

void    saveROIMask(const char* mask_str, int len);
bool    loadROIMask(char* out_buf, int buf_len);

#endif // ZONE_MANAGER_H
