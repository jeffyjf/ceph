// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/tlc/DiscardRequest.h"

namespace librbd {
namespace cache {
namespace tlc {

DiscardRequest::DiscardRequest(CephContext *cct, Context *on_finish, 
  std::string cache_file_path, ExtentBufPtr extent_buf):m_cct(cct), 
  m_on_finish(on_finish), m_cache_file_path(cache_file_path),
  m_extent_buf(extent_buf){}

void DiscardRequest::send() {

  int fd = ::open(m_cache_file_path.c_str(), O_RDWR, 0644);
  if (fd<0){
    finish(fd);
  }
  int r=::fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE, 
    m_extent_buf->offset, m_extent_buf->len);
  ::close(fd);
  finish(r);
}

void DiscardRequest::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace tlc
} // namespace cache
} // namespace librbd
