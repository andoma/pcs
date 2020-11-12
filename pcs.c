#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <pthread.h>

#include "pcs.h"


// Packetized Character Stream

TAILQ_HEAD(pcs_queue, pcs);

static pthread_mutex_t pcs_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pcs_queue pcss = { NULL, &pcss.tqh_first };

#define PCS_STATE_EST    0
#define PCS_STATE_FIN    1   // Remote have shutdown (We've seen PCS_F_EOS)
#define PCS_STATE_CLOSED 2   // We've closed
#define PCS_STATE_ERR    3   // Remote have timed out
#define PCS_STATE_LINGER 4
#define PCS_STATE_SYN    5
#define PCS_STATE_SYNACK 6

// Flags < bit 8 are propagated to the remote via the hdr[0] byte
#define PCS_F_SYN   0x1
#define PCS_F_LOSS  0x2
#define PCS_F_EOS   0x4
#define PCS_F_ACK   0x8

struct pcs {
  int64_t last_output;
  int64_t last_input;
  void *arg;
  TAILQ_ENTRY(pcs) link;

  pthread_cond_t txfifo_cond;
  pthread_cond_t rxfifo_cond;

  int rtx;

  uint8_t state;
  uint8_t channel;
  uint8_t flow;
  uint8_t pending_send_flags;

  uint16_t txfifo_size;

  uint16_t txfifo_acked;
  uint16_t txfifo_sent;
  uint16_t txfifo_wrptr;
  uint16_t txfifo_eos;

  uint16_t rxfifo_size;
  uint16_t rxfifo_rdptr;
  uint16_t rxfifo_wrptr;

  uint8_t buffers[0];
};


#define MAX_HEADER_LEN 7


static size_t
pcs_xmit(pcs_t *pcs, uint8_t *buf, size_t max_bytes)
{
  // max_bytes is guaranteed to be at least MAX_HEADER_LEN
  // this is checked in pcs_poll()

  uint8_t flags = pcs->pending_send_flags;

  pcs->pending_send_flags &= ~(PCS_F_LOSS | PCS_F_ACK);

  if(pcs->state == PCS_STATE_SYN) {
    pcs->flow = rand();
    flags |= PCS_F_SYN;
  } else if(pcs->state == PCS_STATE_SYNACK) {
    flags |= PCS_F_SYN | PCS_F_ACK;
  }

  buf[0] = pcs->channel;
  buf[2] = pcs->flow;
  // ACK
  buf[3] = pcs->rxfifo_wrptr >> 8;
  buf[4] = pcs->rxfifo_wrptr;

  int size = 5;
  buf[size++] = pcs->txfifo_sent >> 8;
  buf[size++] = pcs->txfifo_sent;

  const int max_payload = max_bytes - size;
  if(max_payload < 0)
    return size;

  int payload_len = 0;

  if(pcs->state <= PCS_STATE_CLOSED) {
    const uint16_t tx_avail = pcs->txfifo_wrptr - pcs->txfifo_sent;
    payload_len = MIN(tx_avail, max_payload);
    if(payload_len) {
      const uint16_t mask = pcs->txfifo_size - 1;

      uint8_t *txfifo = pcs->buffers + pcs->rxfifo_size;
      uint16_t rdptr = pcs->txfifo_sent;

      for(int i = 0; i < payload_len; i++) {
        buf[size++] = txfifo[rdptr++ & mask];
      }
    }
  }

  if(pcs->txfifo_sent != pcs->txfifo_eos) {
    flags &= ~PCS_F_EOS;
  }

  buf[1] = flags;

  pcs->txfifo_sent += payload_len;
  return size;
}



static pcs_t *
pcs_create(uint8_t channel, int state, int64_t now)
{
  size_t fifo_size = 256;

  pcs_t *pcs = malloc(sizeof(pcs_t) + fifo_size * 2);
  memset(pcs, 0, sizeof(pcs_t));
  pcs->channel = channel;
  pcs->txfifo_size = fifo_size;
  pcs->rxfifo_size = fifo_size;

  uint16_t seq = rand();
  pcs->txfifo_acked = seq;
  pcs->txfifo_sent  = seq;
  pcs->txfifo_wrptr = seq;

  pthread_cond_init(&pcs->txfifo_cond, NULL);
  pthread_cond_init(&pcs->rxfifo_cond, NULL);

  pcs->state = state;
  TAILQ_INSERT_HEAD(&pcss, pcs, link);
  pcs->last_input = now;
  pcs->rtx = 50000;
  return pcs;
}


static void
accept_data(pcs_t *pcs, const uint8_t *data, int len, uint16_t seq)
{
  if(seq != pcs->rxfifo_wrptr) {
    // Got data which does not point to our fifo buffers start,
    pcs->pending_send_flags |= (PCS_F_LOSS | PCS_F_ACK);
    return;
  }

  if(len == 0)
    return;

  uint16_t avail = pcs->rxfifo_size - (pcs->rxfifo_wrptr - pcs->rxfifo_rdptr);
  uint8_t *rxfifo = pcs->buffers;

  const int to_copy = MIN(avail, len);
  const int mask = pcs->rxfifo_size - 1;

  uint16_t wrptr = pcs->rxfifo_wrptr;
  for(int i = 0; i < to_copy; i++) {
    rxfifo[wrptr++ & mask] = data[i];
  }
  pcs->rxfifo_wrptr = wrptr;
  pthread_cond_signal(&pcs->rxfifo_cond);

  pcs->pending_send_flags |= PCS_F_ACK;
}


static void
pcs_input_locked(const uint8_t *data, size_t len, int64_t now)
{
  if(len < 5)
    return;

  const uint8_t channel = data[0];
  const uint8_t in_flags = data[1];
  const uint8_t flow = data[2];
  const uint16_t ack = (data[3] << 8) | data[4];

  data += 5;
  len  -= 5;

  pcs_t *pcs;
  TAILQ_FOREACH(pcs, &pcss, link) {
    if(pcs->channel == channel && pcs->flow == flow) {
      break;
    }
  }

  if((in_flags & (PCS_F_SYN | PCS_F_ACK)) == PCS_F_SYN) {
    // Got SYN
    if(pcs != NULL) {
      return; // But we already have a connection here, drop this packet
    }
    if(len != 2)
      return;
    pcs = pcs_create(channel, PCS_STATE_SYNACK, now);
    pcs->flow = flow;
    pcs->rxfifo_wrptr = pcs->rxfifo_rdptr = (data[0] << 8) | data[1];
    return;
  }

  if(pcs == NULL) {
    return;
  }

  switch(pcs->state) {

  case PCS_STATE_SYN:
    // We have sent SYN and are waiting for an ACK
    if(len != 2)
      return;

    pcs->rxfifo_wrptr = pcs->rxfifo_rdptr = (data[0] << 8) | data[1];
    pcs->state = PCS_STATE_EST;
    pcs->last_output = 0;
    break;

  case PCS_STATE_SYNACK:
    pcs->state = PCS_STATE_EST;
    pcs_accept(pcs, pcs->channel);
    pcs->last_output = 0;
    break;

  case PCS_STATE_CLOSED:
    if(in_flags & PCS_F_EOS) {
      pcs->state = PCS_STATE_LINGER;
      return;
    }
    break;

  case PCS_STATE_EST:
  case PCS_STATE_FIN:
    break;

  case PCS_STATE_LINGER:
    pcs->last_output = 0;
    return;

  case PCS_STATE_ERR:
    return;
  }

  int16_t ackd_delta = ack - pcs->txfifo_acked;
  int16_t   wr_delta = ack - pcs->txfifo_wrptr;

  if(ackd_delta >= 0 && wr_delta <= 0) {
    pcs->txfifo_acked = ack;
    pthread_cond_signal(&pcs->txfifo_cond);

    if(in_flags & PCS_F_LOSS) {
      pcs->txfifo_sent = ack;
    }
  } else {
    return;
  }

  pcs->last_input = now;
  pcs->rtx = 20000;

  if(len >= 2) {
    uint16_t seq = (data[0] << 8) | data[1];
    data += 2;
    len -= 2;
    accept_data(pcs, data, len, seq);

    if(in_flags & PCS_F_EOS) {
      uint16_t the_end = seq + len;
      if(pcs->rxfifo_wrptr == the_end) {
        assert(pcs->state == PCS_STATE_EST);
        pcs->state = PCS_STATE_FIN;
        pthread_cond_signal(&pcs->rxfifo_cond);
      }
    }
  }
}


void
pcs_input(const uint8_t *data, size_t len, int64_t now)
{
  pthread_mutex_lock(&pcs_mutex);
  pcs_input_locked(data, len, now);
  pthread_mutex_unlock(&pcs_mutex);
}


int
pcs_send(pcs_t *pcs, const void *data, size_t len, int flush)
{
  uint8_t *txfifo = pcs->buffers + pcs->rxfifo_size;

  pthread_mutex_lock(&pcs_mutex);

  while(len > 0) {

    if(pcs->pending_send_flags & PCS_F_EOS) {
      pthread_mutex_unlock(&pcs_mutex);
      return -1;
    }
    uint16_t avail = pcs->txfifo_size - (pcs->txfifo_wrptr - pcs->txfifo_acked);
    if(avail == 0) {
      pthread_cond_wait(&pcs->txfifo_cond, &pcs_mutex);
      continue;
    }

    const uint16_t mask = pcs->txfifo_size - 1;
    const int to_copy = MIN(avail, len);
    uint16_t wrptr = pcs->txfifo_wrptr;
    const uint8_t *d = data;
    for(int i = 0; i < to_copy; i++) {
      txfifo[wrptr++ & mask] = d[i];
    }

    pcs->txfifo_wrptr = wrptr;

    data += to_copy;
    len -= to_copy;
  }
  pthread_mutex_unlock(&pcs_mutex);
  return 0;
}


static void
pcs_shutdown_locked(pcs_t *pcs)
{
  pcs->txfifo_eos = pcs->txfifo_wrptr;
  pcs->pending_send_flags |= PCS_F_EOS;
}



void
pcs_shutdown(pcs_t *pcs)
{
  pthread_mutex_lock(&pcs_mutex);
  pcs_shutdown_locked(pcs);
  pthread_mutex_unlock(&pcs_mutex);
}

void
pcs_close(pcs_t *pcs)
{
  pthread_mutex_lock(&pcs_mutex);
  pcs_shutdown_locked(pcs);
  pcs->state = PCS_STATE_CLOSED;
  pthread_mutex_unlock(&pcs_mutex);
}


pcs_t *
pcs_connect(uint8_t channel, int64_t now)
{
  pthread_mutex_lock(&pcs_mutex);
  pcs_t *pcs = pcs_create(channel, PCS_STATE_SYN, now);
  pthread_mutex_unlock(&pcs_mutex);
  return pcs;
}


int
pcs_read(pcs_t *pcs, void *data, size_t len, int all)
{
  uint8_t *d = data;
  int total = 0;
  uint8_t *rxfifo = pcs->buffers;

  pthread_mutex_lock(&pcs_mutex);

  while(1) {

    assert(pcs->state != PCS_STATE_CLOSED &&
           pcs->state != PCS_STATE_LINGER);

    if(all) {
      if(len == 0)
        break;
    } else {
      if(total > 0)
        break;
    }

    const uint16_t avail = pcs->rxfifo_wrptr - pcs->rxfifo_rdptr;
    if(!avail) {

      if(pcs->state == PCS_STATE_FIN) {
        total = 0;
        break;
      }
      if(pcs->state == PCS_STATE_ERR) {
        total = -1;
        break;
      }
      pthread_cond_wait(&pcs->rxfifo_cond, &pcs_mutex);
      continue;
    }

    const int to_copy = MIN(avail, len);
    const uint16_t mask = pcs->rxfifo_size - 1;

    uint16_t rdptr = pcs->rxfifo_rdptr;
    for(int i = 0; i < to_copy; i++) {
      *d++ = rxfifo[rdptr++ & mask];
    }
    len -= to_copy;
    total += to_copy;
    pcs->rxfifo_rdptr = rdptr;
  }
  pthread_mutex_unlock(&pcs_mutex);
  return total;
}


static void
pcs_backoff(pcs_t *pcs)
{
  pcs->rtx = MIN(pcs->rtx + 10000, 500000);
}


static void
pcs_destroy(pcs_t *pcs)
{
  TAILQ_REMOVE(&pcss, pcs, link);
  free(pcs);
}

size_t
pcs_poll(uint8_t *buf, size_t max_bytes, int64_t clock)
{
  if(max_bytes < MAX_HEADER_LEN)
    return 0;

  if(pthread_mutex_trylock(&pcs_mutex))
    return 0;

  pcs_t *pcs, *n;
  for(pcs = TAILQ_FIRST(&pcss); pcs != NULL; pcs = n) {
    n = TAILQ_NEXT(pcs, link);

    switch(pcs->state) {
    case PCS_STATE_SYN:
    case PCS_STATE_SYNACK:
      if(clock > pcs->last_output + pcs->rtx) {
        pcs_backoff(pcs);
        break;
      }
      continue;

    case PCS_STATE_EST:
    case PCS_STATE_FIN:
    case PCS_STATE_CLOSED:
      if(clock > pcs->last_output + 1500000)
        break;

      if(pcs->txfifo_sent != pcs->txfifo_wrptr)
        break;

      if(pcs->pending_send_flags & PCS_F_ACK)
        break;

      if((pcs->txfifo_acked != pcs->txfifo_wrptr ||
          pcs->pending_send_flags & PCS_F_EOS) &&
         clock > pcs->last_output + pcs->rtx) {
        pcs_backoff(pcs);
        break;
      }
      continue;

    case PCS_STATE_ERR:
      continue;

    case PCS_STATE_LINGER:
      if(clock > pcs->last_input + 5000000) {
        pcs_destroy(pcs);
        continue;
      }
      if(pcs->last_output)
        continue;
      break;
    }

    if(clock > pcs->last_input + 5000000) {
      pcs->state = PCS_STATE_ERR;
      pthread_cond_signal(&pcs->rxfifo_cond);
      pthread_cond_signal(&pcs->txfifo_cond);
      continue;
    }

    size_t r = pcs_xmit(pcs, buf, max_bytes);
    if(r) {
      pcs->last_output = clock;
      TAILQ_REMOVE(&pcss, pcs, link);
      TAILQ_INSERT_TAIL(&pcss, pcs, link);
      pthread_mutex_unlock(&pcs_mutex);
      return r;
    }
  }
  pthread_mutex_unlock(&pcs_mutex);
  return 0;
}


void * __attribute__((weak))
pcs_accept(pcs_t *pcs, uint8_t channel)
{
  return NULL;
}
