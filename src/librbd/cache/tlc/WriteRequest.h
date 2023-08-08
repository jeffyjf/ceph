// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_WRITE_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_WRITE_REQUEST_H


#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/tlc/ExtentBuf.h"
#include "blk/BlockDevice.h"


namespace librbd {
namespace cache {
namespace tlc {


class WriteRequest {
public:

  WriteRequest(
      CephContext *cct, Context *on_finish, uint64_t tid, io::Extents& image_extents, bufferlist&& bl, BlockDevice* bdev);
  
  void send();

private:
  void finish(int r);

  CephContext *m_cct;
  Context *m_on_finish;
  uint64_t m_tid;
  io::Extents& m_image_extents;
  bufferlist& m_bl;
  BlockDevice* m_bdev;
};

} // namespace tlc
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_TLC_WRITE_REQUEST_H