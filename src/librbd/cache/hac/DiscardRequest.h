// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_DISCARD_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_DISCARD_REQUEST_H

#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/ExtentBuf.h"

namespace librbd {
namespace cache {
namespace hac {

class DiscardRequest {
public:
  DiscardRequest(CephContext *cct):m_cct(cct){}
  void push_to_update_hot_area(ExtentBufPtr extent_buf, Area* area);
  void send();

private:  
  ExtentBufVec update_hot_area;
  std::vector<Area*> areas_to_write;
  CephContext *m_cct;
};

} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_DISCARD_REQUEST_H