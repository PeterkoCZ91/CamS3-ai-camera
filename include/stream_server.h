#ifndef STREAM_SERVER_H
#define STREAM_SERVER_H

bool streamServerInit();
void streamServerStop();
// False when the viewer port failed to start; stream_url then points at STREAM_PORT.
bool viewerStreamServerRunning();

#endif // STREAM_SERVER_H
