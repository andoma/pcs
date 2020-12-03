#pragma once

#include <sys/uio.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct pcs pcs_t;


// Return 0 to accept, any other to terminate connection
int pcs_accept(pcs_t *pcs, uint8_t channel);

void pcs_input(const uint8_t *data, size_t len, int64_t clock);

int pcs_send(pcs_t *pcs, const void *data, size_t len, int flush);

pcs_t *pcs_connect(uint8_t channel, int64_t clock);

int pcs_read(pcs_t *pcs, void *data, size_t len, int wait);

void pcs_close(pcs_t *pcs);

void pcs_shutdown(pcs_t *pcs);

size_t pcs_poll(uint8_t *buf, size_t max_bytes, int64_t clock);
