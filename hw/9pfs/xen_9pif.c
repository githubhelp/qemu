/*
 * Xen 9p backend - based largely on xen-disk.c; contains xen specific functions
 *
 * Copyright Linda Jacobson 2015
 *
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "qemu/queue.h"
#include "fsdev/qemu-fsdev.h"
#include "hw/hw.h"
#include "hw/xen/xen_backend.h"
#include "virtio-9p-coth.h"
#include "xen_p9.h"
#include "v9fs_server.h"
#include "xen-9p.h"

/* ------------------------------------------------------------- */

static void init_freelist (XenP9Dev *p9dev)
{
    int i;

    for (i=0; i<MAX_REQUESTS; i++) 
        p9dev->in_use_req[i] = false;
}

static int  get_id_from_freelist (XenP9Dev *p9dev)
{
    int i;

    for (i=0; i<MAX_REQUESTS; i++) 
        if (!p9dev->in_use_req[i]) {
            p9dev->in_use_req[i] = true;
            return i;
	}
    return -1;
}

static p9_request_t *find_request (XenP9Dev *p9dev,  uint16_t tag)
{
    int i;

    for (i=0; i<MAX_REQUESTS; i++) 
        if (p9dev->p9req[i].tag == tag) 
            return &(p9dev->p9req[i]);
    return NULL;
}


/* ------------------------------------------------------------- */


void p9_send_response(void *devptr, uint16_t tag)
{
    int               send_notify   = 0;
    int               have_requests = 0;
    p9_response_t     resp;
    void              *dst;
    struct XenP9Dev  *p9dev = (struct XenP9Dev *) devptr;
    p9_request_t *p9reqp;

    p9reqp = find_request (p9dev, tag);
    if (p9reqp == NULL)
    resp.id        = p9reqp->id;
    resp.status    = P9_RSP_OKAY;
    resp.tag = tag;

    dst = RING_GET_RESPONSE(&p9dev->ring, p9dev->ring.rsp_prod_pvt);
    memcpy(dst, &resp, sizeof(resp));
    p9dev->ring.rsp_prod_pvt++;

    RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&p9dev->ring, send_notify);
    if (p9dev->ring.rsp_prod_pvt == p9dev->ring.req_cons) {
        /*
         * Tail check for pending requests. Allows frontend to avoid
         * notifications if requests are already in flight (lower
         * overheads and promotes batching).
         */
        RING_FINAL_CHECK_FOR_REQUESTS(&p9dev->ring, have_requests);
    } else if (RING_HAS_UNCONSUMED_REQUESTS(&p9dev->ring)) {
        have_requests = 1;
    }

    if (have_requests) {
        qemu_bh_schedule(p9dev->bh);
    }
    if (send_notify) {
        xen_be_send_notify(&p9dev->xendev);
    }
}

static void p9_handle_requests(XenP9Dev *p9dev)
{
    p9_request_t p9req;
    bool         pdu_avail=true;
    struct iovec *in_sg;
    struct iovec *out_sg;
    int i;

    /*
     * get a request from the ring buffer
     */
  
    RING_IDX rc, rp;


    p9dev->more_work = 0;

    rc = p9dev->ring.req_cons;
    rp = p9dev->ring.sring->req_prod;
    xen_rmb(); /* Ensure we see queued requests up to 'rp'. */

 
    //    p9_send_response_all(p9dev);  //not needed now
    while ((rc != rp) && pdu_avail)  {
        /* pull request from ring */
        if (RING_REQUEST_CONS_OVERFLOW(&p9dev->ring, rc)) {
            break;
        }
	//        p9_get_request(p9dev, &p9req, rc);
	
        memcpy(&p9req, RING_GET_REQUEST(&(p9dev->ring), rc), sizeof(p9req));
       /*
        *  update pointers
        */
        p9dev->ring.req_cons = ++rc;
        p9dev->gref = p9req.gref;
        p9dev->page  = xc_gnttab_map_grant_ref(p9dev->xendev.gnttabdev,
					   p9dev->xendev.dom,
                                           p9dev->gref,
                                           PROT_READ | PROT_WRITE);
        if (!(p9dev->page)) {
            fprintf (stderr,"leavin handle_reqeuests  error\n");       
            return;
        }
	/*
         * save request where I can find it; a list would be preferable, but this is simple
         */
	i = get_id_from_freelist(p9dev);
	memcpy (&(p9dev->p9req[i]), &p9req, sizeof (p9_request_t));
	p9dev->in_use_req[i] = true;
        /*
         * parse request and do something with it
         */
        in_sg = g_malloc0(sizeof (struct iovec));
        out_sg = g_malloc0(sizeof (struct iovec));	
        out_sg->iov_base = p9dev->page;
        in_sg->iov_base = p9dev->page + p9req.out_len;
        out_sg->iov_len = p9req.out_len;
        in_sg->iov_len = p9req.in_len;
        /*
         * parse request and do something with it 
         * I think I may have a boundary problem here-  FIX ME
         */
        pdu_avail = xen_pdu_handle(p9dev->s, in_sg, out_sg);
    } 
}

static void p9_bh(void *opaque)
{
    struct XenP9Dev *p9dev = opaque;
  fprintf (stderr, "in p9_bh\n");
    p9_handle_requests(p9dev);
  fprintf (stderr, "leaving in p9_bh\n");
}

/*
 * We need to account for the grant allocations requiring contiguous
 * chunks; the worst case number would be
 *     max_req * max_seg + (max_req - 1) * (max_seg - 1) + 1,
 * but in order to keep things simple just use
 *     2 * max_req * max_seg.
 */
#define MAX_GRANTS(max_req, max_seg) (2 * (max_req) * (max_seg))


static void p9_alloc(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
 
    p9dev->bh = qemu_bh_new(p9_bh, p9dev);
    if (xc_gnttab_set_max_grants(xendev->gnttabdev,
            MAX_GRANTS(MAX_REQUESTS, 1)) < 0) {
        xen_be_printf(xendev, 0, "xc_gnttab_set_max_grants failed: %s\n",
                      strerror(errno));
    }
}

static int p9_init(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
    V9fsState *s;
    V9GenericFsState *gs;
    V9fsPath path;
    int i;
    struct stat stat;
    FsDriverEntry *fse;
    char fsdev_id[30];
    const char *fsdev = "fsdev0";
    const char mount_tag[] = "9pMount";
    char m_tag[30];

    strcpy (fsdev_id, "local");
    strcpy (m_tag, "9pMount");
    fprintf (stderr, "in init\n");
    init_freelist (p9dev);
    /*
     *  allocate, the state struct
     */
    p9dev->s = gs = g_malloc0 (sizeof (struct V9GenericFsState));
    s = (V9fsState *)p9dev->s;
    /* initialize pdu allocator */
    QLIST_INIT(&(s->free_list));
    QLIST_INIT(&(s->active_list));
    for (i = 0; i < (MAX_REQ - 1); i++) {
        QLIST_INSERT_HEAD(&(s->free_list), &(s->pdus[i]), next);
    }
    gs->devptr = (void *) p9dev;
    gs->complete = xen_complete_pdu;
    v9fs_path_init(&path);
    fprintf (stderr, "path is %s\n", path.data);
    //    fsdev_id = qemu_opt_get (&qemu_xen_fsdev_opts, fsdev);
    xenstore_write_be_str (xendev, fsdev, fsdev_id);
    fse = get_fsdev_fsentry(fsdev_id);
    if (!fse) {
        /* We don't have a fsdev identified by fsdev_id */
        fprintf(stderr, "Xen-9p device couldn't find fsdev with the "
                   "id = %s\n",
		fsdev_id/* ? fsdev_id : "NULL"*/);
        exit(1);
    }
    fprintf (stderr, "fse path is %s\n", fse->path);
    s->ctx.export_flags = fse->export_flags;
    s->ctx.fs_root = g_strdup(fse->path);
    s->ctx.uid = -1;

    s->ops = fse->ops;
    /* 
     * write xenstore entry for initial mount tag
     */
    //    m_tag = qemu_opt_get (qemu_xen9pfs_opts, mount_tag);
    xenstore_write_be_str (xendev, mount_tag, m_tag);
    s->tag = g_strdup(m_tag);
    qemu_co_rwlock_init(&s->rename_lock);

    if (s->ops->init(&s->ctx) < 0) {
        fprintf (stderr, "Xen-9p Failed to initialize fs-driver with id:%s"
                   " and export path:%s", fsdev_id, s->ctx.fs_root);
	goto out;
    }
    if (v9fs_init_worker_threads() < 0) {
        fprintf (stderr, "worker thread initialization failed");
        goto out;
    }
    set_9p_fd_limit ();
    fprintf (stderr, "about to call callbacks in init\n");
    /*
     * Check details of export path, We need to use fs driver
     * call back to do that. Since we are in the init path, we don't
     * use co-routines here.
     */
    if (s->ops->name_to_path(&s->ctx, NULL, "/", &path) < 0) {
        fprintf(stderr,
                   "error in converting name to path %s", strerror(errno));
        goto out;
    }
    if (s->ops->lstat(&s->ctx, &path, &stat)) {
        fprintf(stderr, "share path %s does not exist", fse->path);
        goto out;
    } else if (!S_ISDIR(stat.st_mode)) {
        fprintf(stderr, "share path %s is not a directory", fse->path);
        goto out;
    }
    v9fs_path_free(&path);
    fprintf (stderr, "exiting init\n");
    return 0;
out:
    g_free(s->ctx.fs_root);
    g_free(s->tag);
    v9fs_path_free(&path);
    exit(1);;
}

static void p9_connect(struct XenDevice *xendev)
{
  //struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
  
  
   fprintf (stderr,"in p9_connect\n");  

}

/*
 * initializing to sync up w/ front-end
 */
static int p9_initwfe(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
 
    fprintf (stderr,"in initwfe");  
    if (xenstore_read_fe_int(&p9dev->xendev, "ring-ref", &p9dev->ring_ref) == -1)   {
        fprintf (stderr,"leavin initfwe error\n");  
        return -1;
    }
    if (xenstore_read_fe_int(&p9dev->xendev, "event-channel",
                             &p9dev->xendev.remote_port) == -1) {
      fprintf (stderr,"leavin initfwe error2\n");  
         return -1;
    }
   
    p9dev->sring = xc_gnttab_map_grant_ref(p9dev->xendev.gnttabdev,
					    p9dev->xendev.dom,
                                            p9dev->ring_ref,
                                            PROT_READ | PROT_WRITE);
    if (!p9dev->sring) {
        fprintf (stderr,"leavin error3\n");  
        return -1;
    }

    p9_sring_t *sring  = p9dev->sring;
    BACK_RING_INIT(&p9dev->ring, sring, XC_PAGE_SIZE);
 
    xen_be_bind_evtchn(&p9dev->xendev);

    xen_be_printf(&p9dev->xendev, 1, "ok: ring-ref %d, "
                   "remote port %d, local port %d\n",
                   p9dev->ring_ref,  p9dev->xendev.remote_port, p9dev->xendev.local_port);
    return 0;
}

static void p9_disconnect(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);

  
    xen_be_unbind_evtchn(&p9dev->xendev);

    if (p9dev->sring) {
      /*  FIX ME  - need to free memory etc.*/
       p9dev->sring = NULL;
    }
}

static int p9_free(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);

    if (p9dev->sring) {
        p9_disconnect(xendev);
    }
    if (p9dev->page)
        g_free(p9dev->page);
    g_free (p9dev->s);
    qemu_bh_delete(p9dev->bh);

    return 0;
}

static void p9_event(struct XenDevice *xendev)
{
    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
 
    qemu_bh_schedule(p9dev->bh);  
}

struct XenDevOps xen_p9_ops = {
    .size       = sizeof(struct XenP9Dev),
    .flags      = DEVOPS_FLAG_NEED_GNTDEV,
    .alloc      = p9_alloc,   // just been created
    .init       = p9_init,    // prior to synching w/ front-end
    .initialise = p9_initwfe, //initialization when front-end ready
    .connected  = p9_connect, // fe is connected
    .disconnect = p9_disconnect,
    .event      = p9_event,
    .free       = p9_free,  // it's being deleted
};

   
