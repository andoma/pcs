#pragma once

#include <sys/uio.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct pcs pcs_t;

typedef struct pcs_iface pcs_iface_t;

void pcs_input(pcs_iface_t *pi, const uint8_t *data, size_t len, int64_t clock,
               uint16_t addr);

int pcs_send(pcs_t *pcs, const void *data, size_t len);

void pcs_flush(pcs_t *pcs);

pcs_t *pcs_connect(pcs_iface_t *pi, uint8_t channel, int64_t clock,
                   uint16_t addr);

int pcs_read(pcs_t *pcs, void *data, size_t len, size_t req_len);

void pcs_close(pcs_t *pcs);

void pcs_shutdown(pcs_t *pcs);

void pcs_callback(pcs_t *pcs, void *arg,
                  void (*fill)(void *arg, size_t avail,
                               void (*write)(pcs_t *pcs,
                                            const void *buf,
                                            size_t len)));

typedef struct {
  uint16_t len;
  uint16_t addr;
} pcs_poll_result_t;

pcs_poll_result_t pcs_poll(pcs_iface_t *pi, uint8_t *buf,
                           size_t max_bytes, int64_t clock);

pcs_poll_result_t pcs_wait(pcs_iface_t *pi, uint8_t *buf,
                           size_t max_bytes, int64_t clock,
                           int64_t (*wait)(pthread_cond_t *c,
                                           pthread_mutex_t *m,
                                           int64_t deadline));

typedef struct {
  uint16_t tx;
  uint16_t rx;
} pcs_fifo_sizes_t;

pcs_iface_t *pcs_iface_create(void *opaque,
                              pcs_fifo_sizes_t (*fifosize)(void *opaque,
                                                           uint8_t channel),
                              int (*accept)(void *opaque, pcs_t *pcs,
                                            uint8_t channel),
                              const pthread_condattr_t *pca);



void pcs_iface_wakeup(pcs_iface_t *pi);

void *pcs_malloc(size_t size);

pcs_iface_t *pcs_get_iface(pcs_t *pcs);
