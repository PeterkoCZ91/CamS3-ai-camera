#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <Arduino.h>

void timelapseInit();
void timelapseTask(void* param);
uint32_t getTimelapseCount();

#endif // TIMELAPSE_H
