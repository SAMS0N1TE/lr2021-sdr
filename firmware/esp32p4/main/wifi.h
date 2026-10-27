#pragma once
/* Starts the station and the TCP bridge if credentials are saved; 1 if none. */
int wifi_start(void);
/* `wifi`, `wifi set <ssid> <password>`, `wifi clear`. */
int wifi_command(int argc, char **argv);
