// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_EXTENT_BUF_H
#define CEPH_LIBRBD_CACHE_TLC_EXTENT_BUF_H

#include "librbd/io/Types.h"

namespace librbd {
namespace cache {
namespace tlc {

struct ExtentBuf {
  ExtentBuf(io::Extent&& extent);
  io::Extent get_extent();
  uint64_t offset;
  uint64_t len;
  bufferlist buf;
};

std::ostream& operator<<(std::ostream& os, const ExtentBuf& ext_buf);

typedef std::shared_ptr<ExtentBuf> ExtentBufPtr;
typedef std::vector<ExtentBufPtr> ExtentBufVec;

} // namespace tlc
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_TLC_EXTENT_BUF_H
