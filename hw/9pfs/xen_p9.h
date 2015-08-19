/******************************************************************************
 * p9.h
 *
 * Unified 9p I/O interface for Xen guest OSes.  MANY COMMENTS BELOW ARE 
 * copied directly from blkif.h, since the state changes, etc. 
 * apply universally, and they're a good explanation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2015 Linda Jacobson
 */

#ifndef __QEMU_XEN_P9_H__
#define __QEMU_XEN_P9_H__

/*
 * Front->back notifications: When enqueuing a new request, sending a
 * notification can be made conditional on req_event (i.e., the generic
 * hold-off mechanism provided by the ring macros). Backends must set
 * req_event appropriately (e.g., using RING_FINAL_CHECK_FOR_REQUESTS()).
 *
 * Back->front notifications: When enqueuing a new response, sending a
 * notification can be made conditional on rsp_event (i.e., the generic
 * hold-off mechanism provided by the ring macros). Frontends must set
 * rsp_event appropriately (e.g., using RING_FINAL_CHECK_FOR_RESPONSES()).
 */


/*
 * Feature and Parameter Negotiation
 * =================================
 * The two halves of a Xen block driver utilize nodes within the XenStore to
 * communicate capabilities and to negotiate operating parameters.  This
 * section enumerates these nodes which reside in the respective front and
 * backend portions of the XenStore, following the XenBus convention.
 *
 * All data in the XenStore is stored as strings.  Nodes specifying numeric
 * values are encoded in decimal.  Integer value ranges listed below are
 * expressed as fixed sized integer types capable of storing the conversion
 * of a properly formated node string, without loss of information.
 *
 * Any specified default value is in effect if the corresponding XenBus node
 * is not present in the XenStore.
 *
 * XenStore nodes in sections marked "PRIVATE" are solely for use by the
 * driver side whose XenBus tree contains them.
 *
 *
 * See the XenBus state transition diagram below for details on when XenBus
 * nodes must be published and when they can be queried.
 *
 *****************************************************************************
 *                            Backend XenBus Nodes
 *****************************************************************************
 *
 *------------------ Backend Device Identification (PRIVATE) ------------------
 *
 * mode
 *      Values:         "r" (read only), "w" (writable)
 *
 *      The read or write access permissions to the backing store to be
 *      granted to the frontend.
 *
 * params
 *      Values:         string
 *
 *      A free formatted string providing sufficient information for the
 *      backend driver to open the backing device.  (e.g. the path to the
 *      file or block device representing the backing store.)
 *
  *----------------------- Request Transport Parameters ------------------------
 *
 * max-ring-page-order
 *      Values:         <uint32_t>
 *      Default Value:  0
 *      Notes:          1, 3
 *
 *      The maximum supported size of the request ring buffer in units of
 *      lb(machine pages). (e.g. 0 == 1 page,  1 = 2 pages, 2 == 4 pages,
 *      etc.).
 *
 * max-ring-pages
 *      Values:         <uint32_t>
 *      Default Value:  1
 *      Notes:          DEPRECATED, 2, 3
 *
 *      The maximum supported size of the request ring buffer in units of
 *      machine pages.  The value must be a power of 2.
 *
 *------------------------- Backend Device Properties -------------------------
 *
 *
 *
 *****************************************************************************
 *                            Frontend XenBus Nodes
 *****************************************************************************
 *
 *----------------------- Request Transport Parameters -----------------------
 *
 * event-channel
 *      Values:         <uint32_t>
 *
 *      The identifier of the Xen event channel used to signal activity
 *      in the ring buffer.
 *
 * ring-ref
 *      Values:         <uint32_t>
 *      Notes:          6
 *
 *      The Xen grant reference granting permission for the backend to map
 *      the sole page in a single page sized ring buffer.
 *
 * ring-ref%u
 *      Values:         <uint32_t>
 *      Notes:          6
 *
 *      For a frontend providing a multi-page ring, a "number of ring pages"
 *      sized list of nodes, each containing a Xen grant reference granting
 *      permission for the backend to map the page of the ring located
 *      at page index "%u".  Page indexes are zero based.
 *
 *
 * ring-page-order
 *      Values:         <uint32_t>
 *      Default Value:  0
 *      Maximum Value:  MAX(ffs(max-ring-pages) - 1, max-ring-page-order)
 *      Notes:          1, 3
 *
 *      The size of the frontend allocated request ring buffer in units
 *      of lb(machine pages). (e.g. 0 == 1 page, 1 = 2 pages, 2 == 4 pages,
 *      etc.).
 *
 * num-ring-pages
 *      Values:         <uint32_t>
 *      Default Value:  1
 *      Maximum Value:  MAX(max-ring-pages,(0x1 << max-ring-page-order))
 *      Notes:          DEPRECATED, 2, 3
 *
 *      The size of the frontend allocated request ring buffer in units of
 *      machine pages.  The value must be a power of 2.
 *
 */
 
/*
 * STATE DIAGRAMS
 *
 *****************************************************************************
 *                                   Startup                                 *
 *****************************************************************************
 *
 * Tool stack creates front and back nodes with state XenbusStateInitialising.
 *
 * Front                                Back
 * =================================    =====================================
 * XenbusStateInitialising              XenbusStateInitialising
 *  o Query virtual device               o Query backend device identification
 *    properties.                          data.
 *  o Setup OS device instance.          o Open and validate backend device.
 *                                       o Publish backend features and
 *                                         transport parameters.
 *                                                      |
 *                                                      |
 *                                                      V
 *                                      XenbusStateInitWait
 *
 * o Query backend features and
 *   transport parameters.
 * o Allocate and initialize the
 *   request ring.
 * o Publish transport parameters
 *   that will be in effect during
 *   this connection.
 *              |
 *              |
 *              V
 * XenbusStateInitialised
 *
 *                                       o Query frontend transport parameters.
 *                                       o Connect to the request ring and
 *                                         event channel.
 *                                       o Publish backend device properties.
 *                                                      |
 *                                                      |
 *                                                      V
 *                                      XenbusStateConnected
 *
 *  o Query backend device properties.
 *  o Finalize OS virtual device
 *    instance.
 *              |
 *              |
 *              V
 * XenbusStateConnected
 *
 * Note: Drivers that do not support any optional features, or the negotiation
 *       of transport parameters, can skip certain states in the state machine:
 *
 *       o A frontend may transition to XenbusStateInitialised without
 *         waiting for the backend to enter XenbusStateInitWait.  In this
 *         case, default transport parameters are in effect and any
 *         transport parameters published by the frontend must contain
 *         their default values.
 *
 *       o A backend may transition to XenbusStateInitialised, bypassing
 *         XenbusStateInitWait, without waiting for the frontend to first
 *         enter the XenbusStateInitialised state.  In this case, default
 *         transport parameters are in effect and any transport parameters
 *         published by the backend must contain their default values.
 *
 *       Drivers that support optional features and/or transport parameter
 *       negotiation must tolerate these additional state transition paths.
 *       In general this means performing the work of any skipped state
 *       transition, if it has not already been performed, in addition to the
 *       work associated with entry into the current state.
 */


#include <xen/io/ring.h>
#include <xen/grant_table.h>

/*
 * REQUEST CODES.
 */
#define P9_OP_READ              0
#define P9_OP_WRITE             1


/*
 * STATUS RETURN CODES.
 */
#define P9_RSP_ERROR       -1
#define P9_RSP_OKAY         0


/*
 *  When we have multiple page ops.
struct p9_request_segment {
    grant_ref_t    gref;        // grant ref to data        
    uint32_t       nrbytes;     // number of bytes to transfer    
};
*/
/*
 *  request for 9p transport front_end
 *
 *  @id  integer identifier
 *  @gref reference to the I/O buffer frame
 *  @offset offset in page where request starts
 *  @nr_bytes total number of bytes allocated to this request's data
 *  @metadata_len - number of bytes of information about the request
 *  @out_len number of bytes of data being sent (may be 0)
 *  @in_len  number of bytes of data that may be returned
 *  @tag identifies the request to client on return
 *  
 */

struct p9_request {
        uint64_t       id;         
        grant_ref_t    gref;        
        uint32_t       offset;   
        uint32_t       nrbytes;
        uint32_t       out_len;
        uint32_t       in_len;
        uint16_t       tag;  
};

/*
 * not to be confused with p9_req_t in 9p client.h
 */
typedef struct p9_request p9_request_t;

struct p9_response {
        uint64_t        id;              /* copied from request */
        uint16_t        tag;             /*  ditto              */
        int16_t         status;          /* BLKIF_RSP_???       */
};

typedef struct p9_response p9_response_t;

enum p9_state {
    P9_STATE_DISCONNECTED,
    P9_STATE_CONNECTED,
    P9_STATE_SUSPENDED,
};

#define P9_RING_SIZE __CONST_RING_SIZE(p9, PAGE_SIZE)

DEFINE_RING_TYPES(p9, struct p9_request, struct p9_response);


#endif

