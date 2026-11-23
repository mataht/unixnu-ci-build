#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void drain_mig_port(mach_port_t port)
{
    for (;;) {
        union {
            mach_msg_header_t hdr;
            uint8_t buf[4096];
        } msg;
        memset(&msg, 0, sizeof(msg.hdr));
        kern_return_t kr = mach_msg(&msg.hdr,
                                    MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE,
                                    0, (mach_msg_size_t)sizeof(msg), port, 0, MACH_PORT_NULL);
        if (kr != MACH_MSG_SUCCESS) break;
        mach_msg_destroy(&msg.hdr);
    }
}

struct client_ctx {
    mach_port_t server;
    kern_return_t sync_kr;
};

static void *client_thread(void *arg)
{
    struct client_ctx *ctx = (struct client_ctx *)arg;
    /* 1. One-way notification (like libquit LQNotifyStuckApp) */
    mach_msg_header_t oneway;
    memset(&oneway, 0, sizeof(oneway));
    oneway.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    oneway.msgh_size = sizeof(oneway);
    oneway.msgh_remote_port = ctx->server;
    oneway.msgh_id = 40001;
    assert(mach_msg(&oneway, MACH_SEND_MSG, sizeof(oneway), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS);

    /* 2. Two-way synchronous MIG RPC waiting for SEND_ONCE reply */
    mach_port_t reply = MACH_PORT_NULL;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) == KERN_SUCCESS);
    union {
        mach_msg_header_t hdr;
        uint8_t raw[128];
    } rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    rpc.hdr.msgh_size = sizeof(mach_msg_header_t);
    rpc.hdr.msgh_remote_port = ctx->server;
    rpc.hdr.msgh_local_port = reply;
    rpc.hdr.msgh_id = 40002;
    ctx->sync_kr = mach_msg(&rpc.hdr, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                            sizeof(mach_msg_header_t), sizeof(rpc), reply, 2000, MACH_PORT_NULL);
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    return NULL;
}

int main(void)
{
    mach_port_t srv = MACH_PORT_NULL;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &srv) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), srv, srv, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);

    dispatch_queue_t q = dispatch_queue_create("test.absentd.mig", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, srv, 0, q);
    dispatch_source_set_event_handler(src, ^{
        drain_mig_port(srv);
    });
    dispatch_resume(src);

    struct client_ctx ctx = { .server = srv, .sync_kr = KERN_FAILURE };
    pthread_t th;
    assert(pthread_create(&th, NULL, client_thread, &ctx) == 0);
    assert(pthread_join(th, NULL) == 0);
    /* mach_msg_destroy releases SEND_ONCE -> kernel delivers MACH_NOTIFY_SEND_ONCE (msgh_id=71, MACH_MSG_SUCCESS) immediately without hanging! */
    assert(ctx.sync_kr == MACH_MSG_SUCCESS);

    dispatch_source_cancel(src);
    printf("absentd_mig_host: ok
");
    return 0;
}
