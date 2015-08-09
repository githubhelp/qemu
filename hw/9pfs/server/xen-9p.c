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


#include "hw/i386/pc.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "virtio-9p.h"
#include "fsdev/qemu-fsdev.h"
#include "server-9p-xattr.h"
#include "server-9p-coth.h"
#include "trace.h"
#include "migration/migration.h"

/*
 * We don't do error checking for pdu_marshal/unmarshal here
 * because we always expect to have enough space to encode
 * error details
 */
static void xen_complete_pdu(V9fsState *s,
			     V9fsPDU *pdu, ssize_t len)
{
    int8_t id = pdu->id + 1; /* Response */

    if (len < 0) {
        int err = -len;
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

    /* push onto queue and notify */
    virtqueue_push(s->vq, &pdu->elem, len);

    /* FIXME: we should batch these completions */
    virtio_notify(VIRTIO_DEVICE(s), s->vq);

    /* Now wakeup anybody waiting in flush for this request */
    qemu_co_queue_next(&pdu->complete);
    free_pdu(s, pdu);
}

void handle_9p_output(VirtIODevice *vdev, VirtQueue *vq)
{
    V9fsState *s = (V9fsState *)vdev;
    V9fsPDU *pdu;
    ssize_t len;

    while ((pdu = alloc_pdu(s)) &&
            (len = virtqueue_pop(vq, &pdu->elem)) != 0) {
        uint8_t *ptr;
        pdu->s = s;
        BUG_ON(pdu->elem.out_num == 0 || pdu->elem.in_num == 0);
        BUG_ON(pdu->elem.out_sg[0].iov_len < 7);

        ptr = pdu->elem.out_sg[0].iov_base;

        pdu->size = le32_to_cpu(*(uint32_t *)ptr);
        pdu->id = ptr[4];
        pdu->tag = le16_to_cpu(*(uint16_t *)(ptr + 5));
        qemu_co_queue_init(&pdu->complete);
        submit_pdu(s, pdu);
    }
    free_pdu(s, pdu);
}
