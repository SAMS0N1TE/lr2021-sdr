#include "lr2021_iq_queue.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct { lr2021_iq_queue *q; atomic_bool failed; } stress_context;
static void yield_cpu(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
#ifdef _WIN32
static DWORD WINAPI consume(LPVOID ptr)
#else
static void *consume(void *ptr)
#endif
{
    stress_context *c=ptr;
    for(unsigned k=0;k<10000;k++) {
        const lr2021_iq_block *b;
        while(!(b=lr2021_iq_queue_peek(c->q)))yield_cpu();
        bool valid=b->bytes==1280 && b->first_pair==(uint64_t)k*320 && b->generation==17;
        for(unsigned j=0;j<1280;j++)if(b->data[j]!=(uint8_t)(k+j*17))valid=false;
        if(!valid){atomic_store(&c->failed,true);break;}
        lr2021_iq_queue_release(c->q);
    }
    return 0;
}
int main(void)
{
    lr2021_iq_queue q;uint8_t data[1280];memset(data,0x5a,sizeof(data));
    CHECK(lr2021_iq_queue_init(&q));CHECK(!lr2021_iq_queue_peek(&q));
    CHECK(!lr2021_iq_queue_push(&q,data,3,0,0));
    CHECK(!lr2021_iq_queue_push(&q,data,1284,0,0));
    /* Cross unsigned sequence wrap, filling and draining all slots repeatedly. */
    atomic_store(&q.read_index,UINT_MAX-3);atomic_store(&q.write_index,UINT_MAX-3);
    for(unsigned cycle=0;cycle<1000;cycle++) {
        for(unsigned i=0;i<8;i++) {
            data[0]=(uint8_t)i;
            CHECK(lr2021_iq_queue_push(&q,data,1280,(uint64_t)cycle*2560+i*320,cycle));
        }
        CHECK(!lr2021_iq_queue_push(&q,data,4,0,9999));
        for(unsigned i=0;i<8;i++) {
            const lr2021_iq_block *b=lr2021_iq_queue_peek(&q);
            CHECK(b && b->bytes==1280 && b->generation==cycle && b->data[0]==i && b->data[1279]==0x5a);
            CHECK(b->first_pair==(uint64_t)cycle*2560+i*320);
            lr2021_iq_queue_release(&q);
        }
        CHECK(!lr2021_iq_queue_peek(&q));lr2021_iq_queue_release(&q);
    }
    CHECK(lr2021_iq_queue_init(&q));
    stress_context context;context.q=&q;atomic_init(&context.failed,false);
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,consume,&context,0,NULL);CHECK(thread!=NULL);
#else
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,consume,&context));
#endif
    for(unsigned k=0;k<10000 && !atomic_load(&context.failed);k++) {
        for(unsigned j=0;j<1280;j++)data[j]=(uint8_t)(k+j*17);
        while(!lr2021_iq_queue_push(&q,data,sizeof(data),(uint64_t)k*320,17)) {
            if(atomic_load(&context.failed))break;
            yield_cpu();
        }
    }
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);CloseHandle(thread);
#else
    CHECK(!pthread_join(thread,NULL));
#endif
    CHECK(!atomic_load(&context.failed));CHECK(!lr2021_iq_queue_peek(&q));
    puts("FIFO queue: overflow/wrap and 10000 concurrent blocks passed");return 0;
}
