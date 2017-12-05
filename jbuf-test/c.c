#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "eloop.h"
#include "jtbuf.h"

#define PACKET_BUF_SIZE 640
#define PACKET_PTIME 20

eloop_t *loop;
event_t *node;
event_t *gtimer;
jbuf_t *g_jt;
char *str[] = {"JB_MISSING_FRAME","JB_NORMAL_FRAME","JB_ZERO_PREFETCH_FRAME","JB_ZERO_EMPTY_FRAME"};

void r_callback(eloop_t *loop,event_t *evt,long fd,void *arg)
{
  static int seq = 0;
  char buf[PACKET_BUF_SIZE] = {0};
  read(fd,buf,sizeof(buf));
  jbuf_put_frame(g_jt,buf,PACKET_BUF_SIZE,++seq);
}

void g_callback(eloop_t *loop,event_t *evt,long fd,void *arg)
{
  char type;
  char buf[PACKET_BUF_SIZE] = {0};
  jbuf_get_frame(g_jt, buf, &type);
  printf("get %s\n",str[type]);
}

int main()
{
  int fd,error = 0;
  loop = e_loop_new();

  //create jitter buffer
  error = jbuf_create(PACKET_BUF_SIZE, PACKET_PTIME, 40, &g_jt);
  if(error){
    return NULL;
  }
  jbuf_set_adaptive(g_jt, 20, 10, 30);
  jbuf_set_discard(g_jt, JB_DISCARD_NONE);

  fd = open("/tmp/pc_fifo",O_RDONLY);
  node = e_event_new(E_READ,fd,r_callback,NULL);
  gtimer = e_event_new(E_TIMER,10,g_callback,NULL);

  e_event_add(loop,node);
  e_event_add(loop,gtimer);
  e_loop_run(loop);
  return 0;
}
