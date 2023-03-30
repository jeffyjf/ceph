// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_WRITE_SAME_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_WRITE_SAME_REQUEST_H

#include "librbd/cache/hac/WriteRequest.h"

namespace librbd {
namespace cache {
namespace hac {

class WriteSameRequest : public WriteRequest {
public:
  WriteSameRequest(
      CephContext *cct, Context *on_finish, uint64_t tid):
       WriteRequest(cct, on_finish, tid) {}
};

} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_WRITE_SAME_REQUEST_H