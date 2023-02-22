// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_WRITE_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_WRITE_REQUEST_H

#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/ExtentBuf.h"

namespace librbd {
namespace cache {
namespace hac {

class WriteRequest {
public:
  WriteRequest(
      CephContext *cct, Context *on_finish, uint64_t tid) :
        m_cct(cct), m_on_finish(on_finish), m_tid(tid) {}
  void push_to_update_hot_area(ExtentBufPtr extent_buf, Area* area);
  void send();
private:
  void finish(int r);
  ExtentBufVec update_hot_area;
  std::vector<Area*> areas_to_write;

  CephContext *m_cct;
  Context *m_on_finish;
  uint64_t m_tid;
};

} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_WRITE_REQUEST_H