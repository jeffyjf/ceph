// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_DISCARD_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_DISCARD_REQUEST_H

#include "include/Context.h"
#include "librbd/cache/tlc/ExtentBuf.h"

namespace librbd {
namespace cache {
namespace tlc {

class DiscardRequest {
public:
  DiscardRequest(CephContext *cct, Context *on_finish, 
    std::string cache_file_path, ExtentBufPtr extent_buf);
  void send();

private:  
  CephContext *m_cct;
  Context *m_on_finish;

  std::string m_cache_file_path;
  ExtentBufPtr m_extent_buf;

  void finish(int r);
};

} // namespace tlc
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_TLC_DISCARD_REQUEST_H