// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_READ_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_READ_REQUEST_H

#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/hac/ExtentBuf.h"
#include "librbd/cache/hac/Area.h"

namespace librbd {
namespace cache {
namespace hac {

class ReadRequest {
public:
  ReadRequest(
      CephContext *cct, bufferlist *out_bl, cache::ImageWriteThroughInterface& image_write_through, Context *on_finish, uint64_t tid)
    : m_cct(cct), m_on_finish(on_finish), m_out_bl(out_bl), m_image_write_through(image_write_through), m_tid(tid) {}

  void push_to_read_from_rados(ExtentBufPtr extent_buf);
  void push_to_read_from_hot_area(ExtentBufPtr extent_buf, Area* area);
  void send();

private:
  void finish(int r);

  ExtentBufVec read_extents;
  ExtentBufVec read_from_rados;
  ExtentBufVec read_from_hot_area;
  std::vector<Area*> read_areas;
  CephContext *m_cct;
  Context *m_on_finish;
  bufferlist *m_out_bl;
  cache::ImageWriteThroughInterface &m_image_write_through;
  uint64_t m_tid;
//  io::Extent& m_origin_extent;
};

} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_READ_REQUEST_H
