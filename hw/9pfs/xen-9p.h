#ifndef __QEMU_XEN_9P_H
#define __QEMU_XEN_9P_H

#include "qemu/config-file.h"
#include "qemu/option.h"
#include "qemu/module.h"
/*
 * FIX ME THIS NEEDS TO CHANGE
 */
#define P9_MAX_SEGMENTS_PER_REQUEST 1

#define MAX_REQUESTS 128
/*
extern QemuOptsList qemu_xen_fsdev_opts;
extern QemuOptsList qemu_xen9pfs_opts;*/

typedef struct XenP9Dev {
    struct XenDevice    xendev;  /* must be first */
    V9GenericFsState    *s;     /* metadata */
    int                 ring_ref;
    void                *sring;
    p9_back_ring_t      ring;
    void                *page;
    grant_ref_t         gref;
    int                 more_work;
    unsigned int        max_grants;
    p9_request_t        p9req[MAX_REQUESTS];
    bool                in_use_req[MAX_REQUESTS];
    QEMUBH              *bh;
} XenP9Dev;

  /*ssize_t xen_pdu_unmarshal (V9fsPDU *pdu, size_t offset, const char *fmt,...);
ssize_t xen_pdu_marshal (V9fsPDU *pdu, size_t offset, const char *fmt,...);
void v9fs_xen_init_qiov_from_pdu(QEMUIOVector *qiov, V9fsPDU *pdu,
                                    size_t skip, size_t size, bool is_write);*/
void p9_send_response(void *devptr, uint16_t tag);
void xen_complete_pdu(V9fsState *s, V9fsPDU *pdu, ssize_t len);
bool xen_pdu_handle (V9GenericFsState *s, struct iovec *in_sg,
		     struct iovec *out_sg);
void set_9p_fd_limit(void);

#endif
