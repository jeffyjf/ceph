// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_READ_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_READ_REQUEST_H

#include "librbd/cache/ImageTempLocalWriteback.h"
#include "librbd/cache/tlc/ExtentBuf.h"
#include "blk/BlockDevice.h"


namespace librbd {
namespace cache {
namespace tlc {

class ReadRequest {
public:
  ReadRequest(
      CephContext *cct, bufferlist *out_bl, 
      cache::ImageTempLocalWritebackInterface& image_temp_local_writeback, 
      Context *on_finish, uint64_t tid, BlockDevice* bdev);

  void push_to_read_from_rados(ExtentBufPtr extent_buf);
  void push_to_read_from_cache(ExtentBufPtr extent_buf);
  void send();

private:
  void finish(int r);

  CephContext *m_cct;
  Context *m_on_finish;
  bufferlist *m_out_bl;
  ImageTempLocalWritebackInterface &m_image_temp_local_writeback;
  uint64_t m_tid;
  BlockDevice* m_bdev;

  ExtentBufVec m_read_extents;
  ExtentBufVec m_read_from_rados;
  ExtentBufVec m_read_from_cache;
};

} // namespace tlc
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_TLC_READ_REQUEST_H
