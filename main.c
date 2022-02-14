#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/time.h>

#include "pcs.h"

static int g_fd;

static int g_do_echo;
static int g_do_hexdump;
static int g_do_timings;
static float g_drop;

static void
hexdump(const char *pfx, const void *data_, int len)
{
  int i, j, k;
  const uint8_t *data = data_;
  char buf[100];

  for(i = 0; i < len; i+= 16) {
    int p = snprintf(buf, sizeof(buf), "0x%06x: ", i);

    for(j = 0; j + i < len && j < 16; j++) {
      p += snprintf(buf + p, sizeof(buf) - p, "%s%02x ",
                    j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++)
      buf[p + k] = ' ';
    p += cnt;

    for(j = 0; j + i < len && j < 16; j++)
      buf[p++] = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
    buf[p] = 0;
    fprintf(stderr, "%s: %s\n", pfx, buf);
  }
}


static void __attribute__((unused))
hexdumpv(const char *pfx, const struct iovec *iov, size_t count)
{
  size_t total_size = 0;
  for(size_t i = 0; i < count; i++) {
    total_size += iov[i].iov_len;
  }

  char *buf = alloca(total_size);
  size_t offset = 0;
  for(size_t i = 0; i < count; i++) {
    memcpy(buf + offset, iov[i].iov_base, iov[i].iov_len);
    offset += iov[i].iov_len;
  }
  hexdump(pfx, buf, total_size);
}


static int64_t
get_ts(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}





static void *
send_to_pcs_thread(void *arg)
{
  pcs_t *pcs = arg;

  char buf[128];
  while(1) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    int r = read(0, buf, sizeof(buf));
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if(r < 1)
      break;

    int64_t ts = get_ts();
    pcs_send(pcs, buf, r);

    if(r > 0) {
      for(int i = 0; i < r; i++) {
        if(buf[i] == 10) {
          pcs_flush(pcs);
          break;
        }
      }
    }

    if(g_do_timings) {
      ts = get_ts() - ts;
      fprintf(stderr, "XMIT %d took %d\n", r, (int)ts);
    }
  }


  fprintf(stderr, "* Shutdown\n");
  pcs_shutdown(pcs);

  return NULL;
}




static void *
read_from_pcs_thread(void *arg)
{
  pcs_t *pcs = arg;

  pthread_t tid = 0;
  pthread_create(&tid, NULL, send_to_pcs_thread, pcs);

  while(1) {
    char buf[128];

    int len = pcs_read(pcs, buf, sizeof(buf), 1);
    if(len == 0) {
      fprintf(stderr, "* Remote end closed connection\n");
      break;
    } else if(len < 0) {
      fprintf(stderr, "* Connection timeout\n");
      break;
    } else {
      if(write(1, buf, len) != len) {
        break;
      }

      if(g_do_echo) {
        pcs_send(pcs, buf, len);
        pcs_flush(pcs);
      }
    }
  }

  if(tid) {
    pthread_cancel(tid);
    pthread_join(tid, NULL);
  }

  pcs_close(pcs);
  fprintf(stderr, "* Closed\n");
  return NULL;
}


static int
pcs_accept(void *opaque, pcs_t *pcs, uint8_t channel)
{
  pthread_t tid;
  fprintf(stderr, "* Accepted connection on channel %d\n", channel);
  pthread_create(&tid, NULL, read_from_pcs_thread, pcs);
  return 0;
}



static void *
read_from_network_thread(void *arg)
{
  pcs_iface_t *pi = arg;
  uint8_t buf[4096];

  while(1) {
    int r = read(g_fd, buf, sizeof(buf));
    if(r > 0) {

      if(drand48() < g_drop) {
        continue;
      }

      if(g_do_hexdump)
        hexdump(" IN", buf, r);
      pcs_input(pi, buf, r, get_ts(), 100);
    }
  }
  return NULL;
}


static void
dopoll(pcs_iface_t *pi)
{
  uint8_t buf[127];

  pcs_poll_result_t ppr = pcs_poll(pi, buf, sizeof(buf), get_ts());
  if(ppr.len) {
    if(g_do_hexdump)
      hexdump("OUT", buf, ppr.len);
    send(g_fd, buf, ppr.len, 0);
  }
}


static struct timespec
usec_to_timespec(uint64_t ts)
{
  return (struct timespec){.tv_sec = ts / 1000000LL,
                           .tv_nsec = (ts % 1000000LL) * 1000};
}


static int64_t
wait_helper(pthread_cond_t *c, pthread_mutex_t *m, int64_t deadline)
{
  if(deadline == INT64_MAX) {
    pthread_cond_wait(c, m);
  } else {
    struct timespec ts = usec_to_timespec(deadline);
    pthread_cond_timedwait(c, m, &ts);
  }
  return get_ts();
}


static void
dowait(pcs_iface_t *pi)
{
  uint8_t buf[127];

  pcs_poll_result_t ppr = pcs_wait(pi, buf, sizeof(buf), get_ts(),
                                   wait_helper);
  if(ppr.len) {
    if(g_do_hexdump)
      hexdump("OUT", buf, ppr.len);
    send(g_fd, buf, ppr.len, 0);
  }
}


int
main(int argc, char **argv)
{
  pthread_t tid;
  srand(getpid());
  int opt;
  int local_port = 0;
  int remote_port = 0;
  char *remote_host = NULL;
  int fifo_size = 128;
  int poll_interval = 0;
  int do_connect = 0;

  while((opt = getopt(argc, argv, "l:r:cxtd:p:e")) != -1) {
    switch(opt) {
    case 'l':
      local_port = atoi(optarg);
      break;
    case 'r':
      remote_host = strdup(optarg);
      char *x = strchr(remote_host, ':');
      if(x == NULL) {
        fprintf(stderr, "-r is malformed\n");
        exit(1);
      }
      *x++ = 0;
      remote_port = atoi(x);
      break;
    case 'c':
      do_connect = 1;
      break;
    case 'x':
      g_do_hexdump = 1;
      break;
    case 't':
      g_do_timings = 1;
      break;
    case 'd':
      g_drop = atoi(optarg) / 100.0f;
      break;
    case 'p':
      poll_interval = atoi(argv[1]);
      break;
    case 'e':
      g_do_echo = 1;
      break;
    }
  }


  if(remote_port == 0) {
    fprintf(stderr, "remote port not set\n");
    exit(1);
  }
  if(remote_host == NULL) {
    fprintf(stderr, "remote host not set\n");
    exit(1);
  }
  if(local_port == 0) {
    fprintf(stderr, "local port not set\n");
    exit(1);
  }

  struct sockaddr_in localaddr = {
    .sin_family = AF_INET,
    .sin_port = htons(local_port),
  };

  struct sockaddr_in remoteaddr = {
    .sin_family = AF_INET,
    .sin_port = htons(remote_port),
    .sin_addr.s_addr = inet_addr(remote_host),
  };

  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  if(bind(fd, (struct sockaddr *)&localaddr, sizeof(localaddr)) < 0) {
    perror("bind");
    exit(1);
  }

  if(connect(fd, (struct sockaddr *)&remoteaddr, sizeof(remoteaddr)) < 0) {
    perror("connect");
    exit(1);
  }

  g_fd = fd;

  pcs_iface_t *pi = pcs_iface_create(NULL, fifo_size, pcs_accept);

  if(do_connect) {
    pcs_t *pcs = pcs_connect(pi, 0, get_ts(), 11);
    pthread_create(&tid, NULL, read_from_pcs_thread, pcs);
  }

  pthread_create(&tid, NULL, read_from_network_thread, pi);

  if(poll_interval) {
    while(1) {
      usleep(poll_interval);
      dopoll(pi);
    }
  } else {

    while(1) {
      dowait(pi);
    }
  }
}
