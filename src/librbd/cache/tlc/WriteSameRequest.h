// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_WRITE_SAME_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_WRITE_SAME_REQUEST_H

#include "librbd/cache/tlc/WriteRequest.h"

namespace librbd {
namespace cache {
namespace tlc {

class WriteSameRequest : public WriteRequest {
public:
  WriteSameRequest(
      CephContext *cct, Context *on_finish, uint64_t tid, io::Extents& image_extents, 
      bufferlist&& bl, BlockDevice* bdev): WriteRequest(cct, on_finish, tid, 
      image_extents, std::move(bl), bdev) {}
};

} // namespace tlc
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_TLC_WRITE_SAME_REQUEST_H