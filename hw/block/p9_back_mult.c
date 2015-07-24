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

#include "hw/hw.h"
#include "hw/xen/xen_backend.h"
#include "xen_p9.h"


/* ------------------------------------------------------------- */


static int max_requests = 2;

/* ------------------------------------------------------------- */

#define BLOCK_SIZE  512
#define P9_MAX_SEGMENTS_PER_REQUEST 1

/*struct ioreq {
    p9_request_t        req;
    int16_t             status;

 
    // grant mapping 
    uint32_t            domids[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    uint32_t            refs[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    int                 prot;
    void                *page[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    void                *pages;
    int                 num_unmap;

  
    struct XenP9Dev    *p9dev;
 
    };*/

struct XenP9Dev {
    struct XenDevice    xendev;  /* must be first */
    char                *params;
    char                *mode;

    int                 ring_ref;
    void                *sring;
    p9_back_ring_t      ring;
    void                *page;
    grant_ref_t         gref;  
  
    int                 more_work;
   
    unsigned int        max_grants;

    QEMUBH              *bh;
};

/* ------------------------------------------------------------- */


static int p9_send_response(struct XenP9Dev  *p9dev, p9_request_t *p9reqp)
{
    int               send_notify   = 0;
    int               have_requests = 0;
    p9_response_t     resp;
    void              *dst;

    resp.id        = p9reqp->id;
    resp.operation = p9reqp->operation;
    resp.status    = P9_RSP_OKAY;

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
        p9dev->more_work++;
    }
    return send_notify;
}

/*
 * a bunch of strings to "read" based on # of bytes
 */
const char *sarray[] = {
  "1", "22", "333", "4444", "5char", "6bytes", "7 bytes", "8 charac", "9 charact", "10 charact",};
  

/*
 * parse request and do something with it
 * right now the do something is to read and print a string if op is a read
 * generate and return a string if op is a read
 */
static void p9_parse_request (struct XenP9Dev *p9dev, p9_request_t *p9reqp)
{
  char astring[1024];
 
  /*
   * for now, assume only read or write of strings
   */
  fprintf (stderr, "in parse: offset is %u, num bytes is %d\n", p9reqp->offset,
	   p9reqp->nrbytes);
  if (p9reqp->operation == P9_OP_READ) {
    int i = p9reqp->nrbytes - 1;
  
    memcpy (p9dev->page + p9reqp->offset, sarray[i], p9reqp->nrbytes);
    fprintf (stderr, "read: %s\n", sarray[i]);
  }
  else {  // is write
    memcpy (astring, p9dev->page + p9reqp->offset, p9reqp->nrbytes);
    astring[p9reqp->nrbytes] = 0;
    fprintf (stderr, "writing:  %s\n", astring);  
  }
}

static void p9_handle_requests(struct XenP9Dev *p9dev)
{
    p9_request_t p9req;
  

    /*
     * get a request from the ring buffer
     */
  
    RING_IDX rc, rp;


    p9dev->more_work = 0;

    rc = p9dev->ring.req_cons;
    rp = p9dev->ring.sring->req_prod;
    xen_rmb(); /* Ensure we see queued requests up to 'rp'. */

 
    //    p9_send_response_all(p9dev);  //not needed now
    while (rc != rp) {
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
	/*
         *  for now, this is always the same, but it's easier to redo this
         *  than test for this being the first request
         */
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
         * parse request and do something with it
         */
        p9_parse_request(p9dev, &p9req);
        if (p9_send_response(p9dev, &p9req)) {
            xen_be_send_notify(&p9dev->xendev);
        }
    }

    /*
     * repeat if more requests
     */
    if (p9dev->more_work) {
        qemu_bh_schedule(p9dev->bh);
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
            MAX_GRANTS(max_requests, 1)) < 0) {
        xen_be_printf(xendev, 0, "xc_gnttab_set_max_grants failed: %s\n",
                      strerror(errno));
    }
}

static int p9_init(struct XenDevice *xendev)
{
  //    struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
   
    /* 
     * read xenstore entries -none yet
     */
      fprintf (stderr,"in p9_init\n");  
 
   return 0;

}

static void p9_connect(struct XenDevice *xendev)
{
  // struct XenP9Dev *p9dev = container_of(xendev, struct XenP9Dev, xendev);
  
  
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

    g_free(p9dev->params);
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

   
