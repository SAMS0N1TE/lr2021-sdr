#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* A byte transport for one bridge session. send returns 0 when all n bytes
 * went out; recv returns bytes read (0 after a short wait) or <0 when the
 * transport closed. */
typedef struct {
    void *context;
    int (*send)(void *, const uint8_t *, size_t);
    int (*recv)(void *, uint8_t *, size_t);
} bridge_transport;
int bridge_init(void);
/* Serves the protocol on t until BYE or a closed transport; -2 while another
 * session holds the radio. */
int bridge_session(const bridge_transport *t);
bool bridge_busy(void);
/* `bridge [baud]`: serves the bridge protocol on the console UART until BYE. */
int bridge_command(int argc, char **argv);
