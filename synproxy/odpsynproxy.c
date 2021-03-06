#define _GNU_SOURCE
#define NETMAP_WITH_LIBS
#include <pthread.h>
#include "llalloc.h"
#include "synproxy.h"
#include "iphdr.h"
#include "ipcksum.h"
#include "packet.h"
#include <odp_api.h>
#include "hashseed.h"
#include "yyutils.h"
#include "mypcapng.h"
#include "odpports.h"
#include <unistd.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <signal.h>
#include "time64.h"
#include "databuf.h"
#include "read.h"
#include "ctrl.h"

atomic_int exit_threads = 0;

static void *signal_handler_thr(void *arg)
{
  sigset_t set;
  int sig;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGUSR1);
  sigaddset(&set, SIGUSR2);
  sigaddset(&set, SIGALRM);
  sigwait(&set, &sig);
  atomic_store(&exit_threads, 1);
  return NULL;
}

struct rx_args {
  struct synproxy *synproxy;
  struct worker_local *local;
  int idx;
};


struct periodic_userdata {
  struct rx_args *args;
  uint64_t dlbytes, ulbytes;
  uint64_t dlpkts, ulpkts;
  uint64_t last_dlbytes, last_ulbytes;
  uint64_t last_dlpkts, last_ulpkts;
  uint64_t last_time64;
  uint64_t next_time64;
};

static void periodic_fn(
  struct periodic_userdata *ud)
{
  uint64_t time64 = gettime64();
  double diff = (time64 - ud->last_time64)/1000.0/1000.0;
  uint64_t ulbdiff = ud->ulbytes - ud->last_ulbytes;
  uint64_t dlbdiff = ud->dlbytes - ud->last_dlbytes;
  uint64_t ulpdiff = ud->ulpkts - ud->last_ulpkts;
  uint64_t dlpdiff = ud->dlpkts - ud->last_dlpkts;
  ud->last_ulbytes = ud->ulbytes;
  ud->last_dlbytes = ud->dlbytes;
  ud->last_ulpkts = ud->ulpkts;
  ud->last_dlpkts = ud->dlpkts;
  worker_local_rdlock(ud->args->local);
  log_log(LOG_LEVEL_INFO, "NMPROXY",
         "worker/%d %g MPPS %g Gbps ul %g MPPS %g Gbps dl"
         " %u conns synproxied %u conns not",
         ud->args->idx,
         ulpdiff/diff/1e6, 8*ulbdiff/diff/1e9,
         dlpdiff/diff/1e6, 8*dlbdiff/diff/1e9,
         ud->args->local->synproxied_connections,
         ud->args->local->direct_connections);
  worker_local_rdunlock(ud->args->local);
  ud->last_time64 = time64;
  ud->next_time64 += 2*1000*1000;
}

#define MAX_WORKERS 64
#define MAX_RX_TX 64
#define MAX_TX 64
#define MAX_RX 64

odp_pktin_queue_t dlinq[MAX_RX_TX];
odp_pktin_queue_t ulinq[MAX_RX_TX];
odp_pktout_queue_t dloutq[MAX_RX_TX];
odp_pktout_queue_t uloutq[MAX_RX_TX];
odp_pool_t pool;
odp_instance_t instance;

int in = 0;
struct pcapng_out_ctx inctx;
int out = 0;
struct pcapng_out_ctx outctx;
int lan = 0;
struct pcapng_out_ctx lanctx;
int wan = 0;
struct pcapng_out_ctx wanctx;

#define POOL_SIZE 300
#define CACHE_SIZE 100
#define QUEUE_SIZE 512
#define BLOCK_SIZE 10240

struct tx_args {
  struct queue *txq;
  int idx;
};

#define PKTCNT 64

static void *rx_func(void *userdata)
{
  struct rx_args *args = userdata;
  struct ll_alloc_st st;
  int i, j, k;
  struct port outport;
  struct odpfunc3_userdata ud;
  struct timeval tv1;
  struct periodic_userdata periodic = {};
  struct allocif intf = {.ops = &ll_allocif_ops_st, .userdata = &st};
  odp_pktin_queue_t inqs[3] =
    {dlinq[args->idx], ulinq[args->idx], dlinq[args->idx]};
  int inqidx = 0;

  gettimeofday(&tv1, NULL);

  if (odp_init_local(instance, ODP_THREAD_WORKER))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't init ODP local");
    exit(1);
  }

  ud.intf = &intf;
  ud.pool = pool;
  ud.dloutq = dloutq[args->idx];
  ud.uloutq = uloutq[args->idx];
  ud.lan = lan;
  ud.wan = wan;
  ud.out = out;
  ud.lanctx = &lanctx;
  ud.wanctx = &wanctx;
  ud.outctx = &outctx;
  ud.dloutcnt = 0;
  ud.uloutcnt = 0;
  outport.portfunc = odpfunc3;
  outport.userdata = &ud;

  if (ll_alloc_st_init(&st, POOL_SIZE, BLOCK_SIZE) != 0)
  {
    abort();
  }

  periodic.last_time64 = gettime64();
  periodic.next_time64 = periodic.last_time64 + 2*1000*1000;
  periodic.args = args;

  while (!atomic_load(&exit_threads))
  {
    uint64_t time64;
    uint64_t expiry;
    int try;
    unsigned from;
    odp_packet_t packets[PKTCNT];
    odp_packet_t pkts2[PKTCNT];
    odp_packet_t pkts3[PKTCNT];
    uint64_t wait;
    int num_rcvd;

    inqidx++;
    if (inqidx > 1)
    {
      inqidx = 0;
    }

    odpfunc3flushdl(&ud);
    odpfunc3flushul(&ud);

    worker_local_rdlock(args->local);
    expiry = timer_linkheap_next_expiry_time(&args->local->timers);
    time64 = gettime64();
    if (expiry > time64 + 1000*1000)
    {
      expiry = time64 + 1000*1000;
    }
    worker_local_rdunlock(args->local);

    if (expiry < time64)
    {
      wait = 0;
    }
    else
    {
      wait = odp_pktin_wait_time((expiry - time64)*1000);
    }
    num_rcvd = odp_pktin_recv_mq_tmo(&inqs[inqidx], 2, &from, packets, PKTCNT, wait);

    time64 = gettime64();
    worker_local_rdlock(args->local);
    try = (timer_linkheap_next_expiry_time(&args->local->timers) < time64);
    worker_local_rdunlock(args->local);

    if (time64 >= periodic.next_time64)
    {
      periodic_fn(&periodic);
    }

    if (try)
    {
      worker_local_wrlock(args->local);
      while (timer_linkheap_next_expiry_time(&args->local->timers) < time64)
      {
        struct timer_link *timer = timer_linkheap_next_expiry_timer(&args->local->timers);
        timer_linkheap_remove(&args->local->timers, timer);
        worker_local_wrunlock(args->local);
        timer->fn(timer, &args->local->timers, timer->userdata);
        worker_local_wrlock(args->local);
      }
      worker_local_wrunlock(args->local);
    }
    j = 0;
    k = 0;
    for (i = 0; i < num_rcvd; i++)
    {
      struct packet pktstruct;
      char *pkt = odp_packet_data(packets[i]);
      size_t sz = odp_packet_len(packets[i]);

      //pktstruct = ll_alloc_st(&st, packet_size(sz));
      pktstruct.data = pkt;
      pktstruct.sz = sz;
      //memcpy(pktstruct.data, pkt, sz);

      if (from + inqidx != 1)
      {
        pktstruct.direction = PACKET_DIRECTION_UPLINK;
        if (uplink(args->synproxy, args->local, &pktstruct, &outport, time64, &st))
        {
          //ll_free_st(&st, pktstruct);
          pkts3[k] = packets[i];
          k++;
        }
        else
        {
          pkts2[j] = packets[i];
          j++;
        }
        periodic.ulpkts++;
        periodic.ulbytes += sz;
        if (in)
        {
          if (pcapng_out_ctx_write(&inctx, pkt, sz, gettime64(), "out"))
          {
            log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't record packet");
            exit(1);
          }
        }
        if (lan)
        {
          if (pcapng_out_ctx_write(&lanctx, pkt, sz, gettime64(), "in"))
          {
            log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't record packet");
            exit(1);
          }
        }
      }
      else
      {
        pktstruct.direction = PACKET_DIRECTION_DOWNLINK;
        if (downlink(args->synproxy, args->local, &pktstruct, &outport, time64, &st))
        {
          //ll_free_st(&st, pktstruct);
          pkts3[k] = packets[i];
          k++;
        }
        else
        {
          pkts2[j] = packets[i];
          j++;
        }
        periodic.dlpkts++;
        periodic.dlbytes += sz;
        if (in)
        {
          if (pcapng_out_ctx_write(&inctx, pkt, sz, gettime64(), "in"))
          {
            log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't record packet");
            exit(1);
          }
        }
        if (wan)
        {
          if (pcapng_out_ctx_write(&wanctx, pkt, sz, gettime64(), "in"))
          {
            log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't record packet");
            exit(1);
          }
        }
      }
    }
    if (from + inqidx != 1)
    {
      int num_sent = odp_pktout_send(uloutq[args->idx], pkts2, j);
      if (num_sent < 0)
      {
        num_sent = 0;
      }
      odp_packet_free_multi(pkts2 + num_sent, j - num_sent);
    }
    else
    {
      int num_sent = odp_pktout_send(dloutq[args->idx], pkts2, j);
      if (num_sent < 0)
      {
        num_sent = 0;
      }
      odp_packet_free_multi(pkts2 + num_sent, j - num_sent);
    }
    odp_packet_free_multi(pkts3, k);
  }
  ll_alloc_st_free(&st);
  odp_term_local();
  log_log(LOG_LEVEL_NOTICE, "RX", "exiting RX thread");
  return NULL;
}

static odp_pktio_t
create_pktio_multiqueue(const char *name,
                        odp_pktin_queue_t *inq, odp_pktout_queue_t *outq,
                        int numq)
{
  odp_pktio_param_t pktio_param;
  odp_pktin_queue_param_t in_queue_param;
  odp_pktout_queue_param_t out_queue_param;
  odp_pktio_t pktio;
  odp_pktio_config_t config;

  odp_pktio_param_init(&pktio_param);
  pktio = odp_pktio_open(name, pool, &pktio_param);
  if (pktio == ODP_PKTIO_INVALID)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't open pktio %s", name);
    exit(1);
  }

  odp_pktio_config_init(&config);
  config.parser.layer = ODP_PROTO_LAYER_L2;
  odp_pktio_config(pktio, &config);

  odp_pktin_queue_param_init(&in_queue_param);
  odp_pktout_queue_param_init(&out_queue_param);

  in_queue_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;

  if (odp_pktin_queue_config(pktio, &in_queue_param))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't config inq %s", name);
    exit(1);
  }

  out_queue_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;

  if (odp_pktout_queue_config(pktio, &out_queue_param))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't config outq %s", name);
    exit(1);
  }

  if (odp_pktin_queue(pktio, inq, numq) != numq)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't query inq %s", name);
    exit(1);
  }
  if (odp_pktout_queue(pktio, outq, numq) != numq)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't query outq %s", name);
    exit(1);
  }
  return pktio;
}

#define POOL_NUM_PKT 8192
#define POOL_SEG_LEN 1856

int main(int argc, char **argv)
{
  pthread_t rx[MAX_RX], ctrl, sigthr;
  struct rx_args rx_args[MAX_RX];
  struct ctrl_args ctrl_args;
  struct synproxy synproxy;
  struct worker_local local;
  odp_pool_param_t params;
  cpu_set_t cpuset;
  struct conf conf = CONF_INITIALIZER;
  int opt;
  char *inname = NULL;
  char *outname = NULL;
  char *lanname = NULL;
  char *wanname = NULL;
  int i;
  sigset_t set;
  int pipefd[2];
  int sockfd;
  struct timer_link timer;
  odp_pktio_t dlio, ulio;

  log_open("NMSYNPROXY", LOG_LEVEL_DEBUG, LOG_LEVEL_INFO);

  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGUSR1);
  sigaddset(&set, SIGUSR2);
  sigaddset(&set, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  if (odp_init_global(&instance, NULL, NULL))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't init ODP global");
    exit(1);
  }
  if (odp_init_local(instance, ODP_THREAD_CONTROL))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't init ODP local");
    exit(1);
  }

  odp_pool_param_init(&params);
  params.pkt.seg_len = POOL_SEG_LEN;
  params.pkt.len = POOL_SEG_LEN;
  params.pkt.num = POOL_NUM_PKT;
  params.type = ODP_POOL_PACKET;
  pool = odp_pool_create("packet pool", &params);
  if (pool == ODP_POOL_INVALID)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't init ODP pool");
    exit(1);
  }



  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
  {
    abort();
  }

  confyydirparse(argv[0], "conf.txt", &conf, 0);
  synproxy_init(&synproxy, &conf);

  hash_seed_init();
  setlinebuf(stdout);

  while ((opt = getopt(argc, argv, "i:o:l:w:")) != -1)
  {
    switch (opt)
    {
      case 'i':
        inname = optarg;
        break;
      case 'o':
        outname = optarg;
        break;
      case 'l':
        lanname = optarg;
        break;
      case 'w':
        wanname = optarg;
        break;
      default:
        log_log(LOG_LEVEL_CRIT, "NMPROXY", "usage: %s [-i in.pcapng] [-o out.pcapng] [-l lan.pcapng] [-w wan.pcapng] vale0:1 vale1:1", argv[0]);
        exit(1);
        break;
    }
  }

  if (argc != optind + 2)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "usage: %s [-i in.pcapng] [-o out.pcapng] [-l lan.pcapng] [-w wan.pcapng] vale0:1 vale1:1", argv[0]);
    exit(1);
  }
  if (inname != NULL)
  {
    if (pcapng_out_ctx_init(&inctx, inname) != 0)
    {
      log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't open pcap output file");
      exit(1);
    }
    in = 1;
  }
  if (outname != NULL)
  {
    if (pcapng_out_ctx_init(&outctx, outname) != 0)
    {
      log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't open pcap output file");
      exit(1);
    }
    out = 1;
  }
  if (lanname != NULL)
  {
    if (pcapng_out_ctx_init(&lanctx, lanname) != 0)
    {
      log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't open pcap output file");
      exit(1);
    }
    lan = 1;
  }
  if (wanname != NULL)
  {
    if (pcapng_out_ctx_init(&wanctx, wanname) != 0)
    {
      log_log(LOG_LEVEL_CRIT, "NMPROXY", "can't open pcap output file");
      exit(1);
    }
    wan = 1;
  }

  int num_rx;
  int max;
  num_rx = conf.threadcount;
  if (num_rx <= 0 || num_rx > MAX_RX)
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "too many threads: %d", num_rx);
    exit(1);
  }
  max = num_rx;

  dlio = create_pktio_multiqueue(argv[optind+0], dlinq, dloutq, max);
  ulio = create_pktio_multiqueue(argv[optind+1], ulinq, uloutq, max);
  if (odp_pktio_start(dlio))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "unable to start dlio");
    exit(1);
  }
  if (odp_pktio_start(ulio))
  {
    log_log(LOG_LEVEL_CRIT, "NMPROXY", "unable to start ulio");
    exit(1);
  }

  worker_local_init(&local, &synproxy, 0, 1);
  if (conf.test_connections)
  {
    int j;
    for (j = 0; j < 90*6; j++)
    {
      uint32_t src, dst;
      src = htonl((10<<24)|(2*j+2));
      dst = htonl((11<<24)|(2*j+1));
      synproxy_hash_put_connected(
        &local, 4, &src, 12345, &dst, 54321,
        gettime64());
    }
  }

  for (i = 0; i < num_rx; i++)
  {
    rx_args[i].idx = i;
    rx_args[i].synproxy = &synproxy;
    rx_args[i].local = &local;
  }

  char pktdl[14] = {0x02,0,0,0,0,0x04, 0x02,0,0,0,0,0x01, 0, 0};
  char pktul[14] = {0x02,0,0,0,0,0x01, 0x02,0,0,0,0,0x04, 0, 0};

  if (strncmp(argv[optind+0], "vale", 4) == 0)
  {
    odp_packet_t odppkt;
    int sent;
    odppkt = odp_packet_alloc(pool, sizeof(pktdl));
    memcpy(odp_packet_data(odppkt), pktdl, sizeof(pktdl));
    sent = odp_pktout_send(dloutq[0], &odppkt, 1);
    if (sent < 0)
    {
      sent = 0;
    }
    if (sent == 0)
    {
      odp_packet_free(odppkt);
    }
  }
  if (strncmp(argv[optind+1], "vale", 4) == 0)
  {
    odp_packet_t odppkt;
    int sent;
    odppkt = odp_packet_alloc(pool, sizeof(pktul));
    memcpy(odp_packet_data(odppkt), pktul, sizeof(pktul));
    sent = odp_pktout_send(uloutq[0], &odppkt, 1);
    if (sent < 0)
    {
      sent = 0;
    }
    if (sent == 0)
    {
      odp_packet_free(odppkt);
    }
  }

  timer.time64 = gettime64() + 32*1000*1000;
  timer.fn = revolve_secret;
  timer.userdata = &local.info;
  timer_linkheap_add(&local.timers, &timer);

  for (i = 0; i < num_rx; i++)
  {
    pthread_create(&rx[i], NULL, rx_func, &rx_args[i]);
  }
  int cpu = 0;
  if (num_rx <= sysconf(_SC_NPROCESSORS_ONLN))
  {
    for (i = 0; i < num_rx; i++)
    {
      CPU_ZERO(&cpuset);
      CPU_SET(cpu, &cpuset);
      cpu++;
      pthread_setaffinity_np(rx[i], sizeof(cpuset), &cpuset);
    }
  }
  sleep(1);
  odp_pktio_promisc_mode_set(dlio, true);
  odp_pktio_promisc_mode_set(ulio, true);
  //set_promisc_mode(sockfd, argv[optind + 0], 1);
  //set_promisc_mode(sockfd, argv[optind + 1], 1);
  if (getuid() == 0 && conf.gid != 0)
  {
    if (setgid(conf.gid) != 0)
    {
      log_log(LOG_LEVEL_WARNING, "NMPROXY", "setgid failed");
    }
    log_log(LOG_LEVEL_NOTICE, "NMPROXY", "dropped group privileges");
  }
  if (getuid() == 0 && conf.uid != 0)
  {
    if (setuid(conf.uid) != 0)
    {
      log_log(LOG_LEVEL_WARNING, "NMPROXY", "setuid failed");
    }
    log_log(LOG_LEVEL_NOTICE, "NMPROXY", "dropped user privileges");
  }
  if (pipe(pipefd) != 0)
  {
    abort();
  }
  ctrl_args.piperd = pipefd[0];
  ctrl_args.synproxy = &synproxy;
  if (   conf.mssmode == HASHMODE_COMMANDED
      || conf.sackmode == HASHMODE_COMMANDED
      || conf.wscalemode == HASHMODE_COMMANDED)
  {
    pthread_create(&ctrl, NULL, ctrl_func, &ctrl_args);
  }

  pthread_create(&sigthr, NULL, signal_handler_thr, NULL);
  log_log(LOG_LEVEL_NOTICE, "NMPROXY", "fully running");
  for (i = 0; i < num_rx; i++)
  {
    pthread_join(rx[i], NULL);
  }
  pthread_join(sigthr, NULL);
  if (write(pipefd[1], "X", 1) != 1)
  {
    log_log(LOG_LEVEL_WARNING, "NMPROXY", "pipe write failed");
  }
  if (   conf.mssmode == HASHMODE_COMMANDED
      || conf.sackmode == HASHMODE_COMMANDED
      || conf.wscalemode == HASHMODE_COMMANDED)
  {
    pthread_join(ctrl, NULL);
  }

  odp_pktio_promisc_mode_set(dlio, false);
  odp_pktio_promisc_mode_set(ulio, false);
  odp_pool_destroy(pool);
  odp_term_local();
  odp_term_global(instance);

  close(pipefd[0]);
  close(pipefd[1]);
  //set_promisc_mode(sockfd, argv[optind + 0], 0);
  //set_promisc_mode(sockfd, argv[optind + 1], 0);
  close(sockfd);

  timer_linkheap_remove(&local.timers, &timer);
  worker_local_free(&local);
  synproxy_free(&synproxy);
  conf_free(&conf);
  log_log(LOG_LEVEL_NOTICE, "NMPROXY", "closing log");
  log_close();

  return 0;
}
