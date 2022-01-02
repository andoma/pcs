#pragma once

#include <sys/uio.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct pcs pcs_t;

typedef struct pcs_iface pcs_iface_t;

void pcs_input(pcs_iface_t *pi, const uint8_t *data, size_t len, int64_t clock);

int pcs_send(pcs_t *pcs, const void *data, size_t len,
             int flush);

pcs_t *pcs_connect(pcs_iface_t *pi, uint8_t channel, int64_t clock);

int pcs_read(pcs_t *pcs, void *data, size_t len, int wait);

void pcs_close(pcs_t *pcs);

void pcs_shutdown(pcs_t *pcs);

size_t pcs_poll(pcs_iface_t *pi, uint8_t *buf, size_t max_bytes, int64_t clock);

pcs_iface_t *pcs_iface_create(void *opaque,
                              int (*accept)(void *opaque, pcs_t *pcs,
                                            uint8_t channel),
                              void (*wakeup)(void *opaque));



