#ifndef __QEMU_V9FS_SERVER_H
#define __QEMU_V9FS_SERVER_H
/*
 *  convert virtio 9p server functions to generic 9p server
 *
 */

#define  GENERIC_9P_SERVER

struct V9fsState;

struct V9fsPDU;
  
typedef void (*Complete)(struct V9fsState *s, struct V9fsPDU *pdu, ssize_t len);
//typedef void (*Marshal)(pdu, offset, fmt, args...);
//typedef void (*UnMarshal)(pdu, offset, fmt, args...);


#include "virtio-9p.h"

typedef struct V9GenericFsState
{
    struct V9fsState s;
    void     *devptr;
    Complete  complete;
  //    Marshal   marshal;
  //    UnMarshal unmarshal;
  //    InitQiovFromPDU init_quiov_from_pdu;
} V9GenericFsState;


#endif
