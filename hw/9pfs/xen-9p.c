/*
 * Xen 9p backend interface to server
 *
 * Copyright Linda Jacobson 2015
 *based on code from virtio-9p.c
 * Copyright IBM, Corp. 2010
 * * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "virtio-9p.h"
#include "hw/i386/pc.h"
#include "qemu/sockets.h"
#include "fsdev/qemu-fsdev.h"
#include "virtio-9p-xattr.h"
#include "virtio-9p-coth.h"
#include "trace.h"
#include "migration/migration.h"
#include "hw/xen/xen_backend.h"
#include "xen_p9.h"
#include "v9fs_server.h"
#include "xen-9p.h"

/*
 * xen_complete_pdu appears to prepare the response to a request.
 *
 * We don't do error checking for pdu_marshal/unmarshal here
 * because we always expect to have enough space to encode
 * error details
 */
void xen_complete_pdu(V9fsState *s, V9fsPDU *pdu, ssize_t len)
{
    int8_t id = pdu->id + 1; /* Response */
    V9GenericFsState *gs;

    if (len < 0) {
        int err = -len;
	/*
         *  FYI this magic # is the length of the header
         *  It should be changed - it took me awhile to figure it out
         */ 
        len = 7;

        if (s->proto_version != V9FS_PROTO_2000L) {
            V9fsString str;

            str.data = strerror(err);
            str.size = strlen(str.data);

            len += pdu_marshal(pdu, len, "s", &str);
            id = P9_RERROR;
        }

        len += pdu_marshal(pdu, len, "d", err);

        if (s->proto_version == V9FS_PROTO_2000L) {
            id = P9_RLERROR;
        }
        trace_v9fs_rerror(pdu->tag, pdu->id, err); /* Trace ERROR */
    }

    /* fill out the header */
    pdu_marshal(pdu, 0, "dbw", (int32_t)len, id, pdu->tag);

    /* keep these in sync */
    pdu->size = len;
    pdu->id = id;
    gs = (V9GenericFsState *)s;
    p9_send_response(gs->devptr, pdu->tag);

    /* Now wakeup anybody waiting in flush for this request */
    qemu_co_queue_next(&pdu->complete);
    free_pdu(s, pdu);
}

/*
 *
 * set up pointers to output data  - this is being saved to use as a template
 * will not be in real version - FIX ME
 * I think this is what I call handle_requests.
 */
bool xen_pdu_handle (V9GenericFsState *gs, struct iovec *in_sg,
		     struct iovec *out_sg)
{
    V9fsPDU *pdu;
    uint8_t *ptr;
    V9fsState *s = (V9fsState *) gs;
    
    if (!(pdu = alloc_pdu(s))) 
        return false;
    pdu->s = s;
    memcpy (&(pdu->elem.in_sg[0]), in_sg, sizeof (struct iovec));
    memcpy (&(pdu->elem.out_sg[0]), out_sg, sizeof (struct iovec));
    g_free (in_sg);
    g_free (out_sg);
    
    pdu->elem.out_num = pdu->elem.in_num = 1;
    BUG_ON(pdu->elem.out_sg[0].iov_len < 7);
    ptr = pdu->elem.out_sg[0].iov_base;

    pdu->size = le32_to_cpu(*(uint32_t *)ptr);
    pdu->id = ptr[4];
    pdu->tag = le16_to_cpu(*(uint16_t *)(ptr + 5));
    qemu_co_queue_init(&pdu->complete);
    submit_pdu(s, pdu);
    free_pdu(s, pdu);
    return true;
}

/*
 * replace a virtio "constructor
 */
void set_9p_fd_limit(void)
{
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
        fprintf(stderr, "Failed to get the resource limit\n");
        exit(1);
    }
    open_fd_hw = rlim.rlim_cur - MIN(400, rlim.rlim_cur/3);
    open_fd_rc = rlim.rlim_cur/2;
}

