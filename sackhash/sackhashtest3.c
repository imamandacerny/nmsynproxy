#include "hashtable.h"
#include "siphash.h"
#include "linkedlist.h"
#include "containerof.h"
#include <pthread.h>

struct sack_ip_port_hash_entry {
  struct hash_list_node node;
  struct linked_list_node llnode;
  uint64_t ipport; // for fast comparisons
};

char sack_hash_key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

static inline uint64_t ip_port(uint32_t ip, uint16_t port)
{
  return ip | (((uint64_t)port)<<32);
}

static inline uint32_t sack_ipport_hash_value(uint64_t ipport)
{
  return siphash64(sack_hash_key, ipport);
}

static inline uint32_t sack_ip_port_hash_value(uint32_t ip, uint16_t port)
{
  return sack_ipport_hash_value(ip_port(ip, port));
}

static inline uint32_t sack_ip_port_hash_fn(
  struct hash_list_node *node, void *ud)
{
  struct sack_ip_port_hash_entry *e;
  e = CONTAINER_OF(node, struct sack_ip_port_hash_entry, node);
  return sack_ipport_hash_value(e->ipport);
}

#define READ_MTX_CNT 128

struct sack_ip_port_hash {
  // Lock order, mtx first, then read_mtx.
  pthread_mutex_t read_mtx[READ_MTX_CNT];
  pthread_mutex_t mtx;
  struct hash_table hash;
  struct linked_list_head list;
};

static int sack_ip_port_hash_init(
  struct sack_ip_port_hash *hash, size_t capacity)
{
  int i;
  if (capacity < READ_MTX_CNT)
  {
    capacity = READ_MTX_CNT;
  }
  if (pthread_mutex_init(&hash->mtx, NULL) != 0)
  {
    return -ENOMEM;
  }
  for (i = 0; i < READ_MTX_CNT; i++)
  {
    if (pthread_mutex_init(&hash->read_mtx[i], NULL) != 0)
    {
      while (i >= 0)
      {
        pthread_mutex_destroy(&hash->read_mtx[i]);
        i--;
      }
      pthread_mutex_destroy(&hash->mtx);
      return -ENOMEM;
    }
  }
  if (hash_table_init(&hash->hash, capacity, sack_ip_port_hash_fn, NULL))
  {
    for (i = 0; i < READ_MTX_CNT; i++)
    {
      pthread_mutex_destroy(&hash->read_mtx[i]);
    }
    pthread_mutex_destroy(&hash->mtx);
    return -ENOMEM;
  }
  linked_list_head_init(&hash->list);
  return 0;
}

static void sack_ip_port_hash_free(struct sack_ip_port_hash *hash)
{
  int i;
  while (!linked_list_is_empty(&hash->list))
  {
    struct linked_list_node *llnode = hash->list.node.next;
    struct sack_ip_port_hash_entry *old;
    uint32_t hashval2;
    old = CONTAINER_OF(llnode, struct sack_ip_port_hash_entry, llnode);
    hashval2 = sack_ipport_hash_value(old->ipport);
    linked_list_delete(llnode);
    hash_table_delete(&hash->hash, &old->node, hashval2);
    free(old);
  }
  for (i = 0; i < READ_MTX_CNT; i++)
  {
    pthread_mutex_destroy(&hash->read_mtx[i]);
  }
  pthread_mutex_destroy(&hash->mtx);
  hash_table_free(&hash->hash);
}

static __attribute__((noinline)) int sack_ip_port_hash_del(
  struct sack_ip_port_hash *hash, uint32_t ip, uint16_t port)
{
  uint64_t ipport = ip_port(ip, port);
  uint32_t hashval = sack_ipport_hash_value(ipport);
  struct hash_list_node *node;
  struct sack_ip_port_hash_entry *e;
  int status = 0;
  if (pthread_mutex_lock(&hash->mtx) != 0)
  {
    abort();
  }
  HASH_TABLE_FOR_EACH_POSSIBLE(&hash->hash, node, hashval)
  {
    e = CONTAINER_OF(node, struct sack_ip_port_hash_entry, node);
    if (e->ipport == ipport)
    {
      if (pthread_mutex_lock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
      {
        abort();
      }
      hash_table_delete(&hash->hash, node, hashval);
      if (pthread_mutex_unlock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
      {
        abort();
      }
      linked_list_delete(&e->llnode);
      free(e);
      status = 1;
      break;
    }
  }
  if (pthread_mutex_unlock(&hash->mtx) != 0)
  {
    abort();
  }
  return status;
}

static __attribute__((noinline)) int sack_ip_port_hash_add(
  struct sack_ip_port_hash *hash, uint32_t ip, uint16_t port)
{
  int result = 0, status = 0;
  uint64_t ipport = ip_port(ip, port);
  uint32_t hashval = sack_ipport_hash_value(ipport);
  struct hash_list_node *node;
  struct sack_ip_port_hash_entry *e;
  if (pthread_mutex_lock(&hash->mtx) != 0)
  {
    abort();
  }
  HASH_TABLE_FOR_EACH_POSSIBLE(&hash->hash, node, hashval)
  {
    e = CONTAINER_OF(node, struct sack_ip_port_hash_entry, node);
    if (e->ipport == ipport)
    {
      result = 1;
      break;
    }
  }
  if (!result)
  {
    e = malloc(sizeof(*e));
    if (e == NULL)
    {
      status = -ENOMEM;
      goto out;
    }
    e->ipport = ipport;
    if (hash->hash.itemcnt >= hash->hash.bucketcnt)
    {
      struct linked_list_node *llnode = hash->list.node.next;
      struct sack_ip_port_hash_entry *old;
      uint32_t hashval2;
      old = CONTAINER_OF(llnode, struct sack_ip_port_hash_entry, llnode);
      hashval2 = sack_ipport_hash_value(old->ipport);
      linked_list_delete(llnode);
      if (pthread_mutex_lock(&hash->read_mtx[hashval2%READ_MTX_CNT]) != 0)
      {
        abort();
      }
      hash_table_delete(&hash->hash, &old->node, hashval2);
      if (pthread_mutex_unlock(&hash->read_mtx[hashval2%READ_MTX_CNT]) != 0)
      {
        abort();
      }
      free(old);
    }
    linked_list_add_tail(&e->llnode, &hash->list);
    if (pthread_mutex_lock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
    {
      abort();
    }
    hash_table_add_nogrow(&hash->hash, &e->node, hashval);
    if (pthread_mutex_unlock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
    {
      abort();
    }
  }
out:
  if (pthread_mutex_unlock(&hash->mtx) != 0)
  {
    abort();
  }
  return status;
}

static int __attribute__((noinline)) sack_ip_port_hash_has(
  struct sack_ip_port_hash *hash, uint32_t ip, uint16_t port)
{
  int result = 0;
  uint64_t ipport = ip_port(ip, port);
  uint32_t hashval = sack_ipport_hash_value(ipport);
  struct hash_list_node *node;
  struct sack_ip_port_hash_entry *e;
  if (pthread_mutex_lock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
  {
    abort();
  }
  HASH_TABLE_FOR_EACH_POSSIBLE(&hash->hash, node, hashval)
  {
    e = CONTAINER_OF(node, struct sack_ip_port_hash_entry, node);
    if (e->ipport == ipport)
    {
      result = 1;
      break;
    }
  }
  if (pthread_mutex_unlock(&hash->read_mtx[hashval%READ_MTX_CNT]) != 0)
  {
    abort();
  }
  return result;
}

int main(int argc, char **argv)
{
  struct sack_ip_port_hash hash;
  size_t i;
  if (sack_ip_port_hash_init(&hash, 128*1024) != 0)
  {
    abort();
  }
  if (sack_ip_port_hash_has(&hash, 0, 0))
  {
    abort();
  }
  for (i = 0; i < 10*1000*1000; i++)
  {
    uint32_t randval = rand();
    uint32_t ip = randval&0xFFF;
    uint16_t port = 128 | ((randval>>16)&0xF);
    int oper = (randval>>24)%2;
    if (oper || !oper)
    {
      if (sack_ip_port_hash_add(&hash, ip, port) != 0)
      {
        abort();
      }
#if 0
      if (!sack_ip_port_hash_has(&hash, ip, port))
      {
        abort();
      }
#endif
    }
    else
    {
      if (sack_ip_port_hash_del(&hash, ip, port) != 0)
      {
        //printf("really deleted %u\n", ip, port);
      }
#if 0
      if (sack_ip_port_hash_has(&hash, ip, port))
      {
        abort();
      }
#endif
    }
  }
  sack_ip_port_hash_free(&hash);
  return 0;
}
