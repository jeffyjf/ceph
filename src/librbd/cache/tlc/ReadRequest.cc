// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#include "include/Context.h"
#include "librbd/cache/tlc/ReadRequest.h"
#include "librbd/cache/tlc/Types.h"


#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::tlc::ReadRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace tlc {

ReadRequest::ReadRequest(
      CephContext *cct, bufferlist *out_bl, 
      cache::ImageTempLocalWritebackInterface& image_temp_local_writeback, 
      Context *on_finish, uint64_t tid, BlockDevice* bdev)
    : m_cct(cct), m_on_finish(on_finish), m_out_bl(out_bl), 
      m_image_temp_local_writeback(image_temp_local_writeback), m_tid(tid),
      m_bdev(bdev){}

void ReadRequest::push_to_read_from_rados(ExtentBufPtr extent_buf) {
  if (extent_buf->len > 0){
    m_read_from_rados.push_back(extent_buf);
    m_read_extents.push_back(extent_buf);
  }
}

void ReadRequest::push_to_read_from_cache(ExtentBufPtr extent_buf) {
  if (extent_buf->len > 0){
    m_read_from_cache.push_back(extent_buf);
    m_read_extents.push_back(extent_buf);
  }
}

void ReadRequest::send() {
  ldout(m_cct, 20) << dendl;
  auto gather_ctx = new C_Gather(m_cct, new LambdaContext([this](int r){
    for (auto ext : m_read_extents) {
      m_out_bl->claim_append(ext->buf);
    }
    finish(r);  
  }));

  for (auto ext_buf : m_read_from_rados) {
    ldout(m_cct, 20) << "Read from rados: " << ext_buf->offset << "~" << ext_buf->len << dendl;
    m_image_temp_local_writeback.aio_read(
      {ext_buf->get_extent()}, &(ext_buf->buf), 0, gather_ctx->new_sub());
  }
  
  if (!m_read_from_cache.empty()){
    AioTransContext* aio = new AioTransContext(m_cct, gather_ctx->new_sub());
    for(auto ext_buf: m_read_from_cache){
      ldout(m_cct, 20) << "Read from cache: " << ext_buf->offset << "~" << ext_buf->len << dendl;    
      m_bdev->aio_read(ext_buf->offset, ext_buf->len, &(ext_buf->buf), &aio->ioc);

    }
    m_bdev->aio_submit(&aio->ioc);
  }

  gather_ctx->activate();
}

void ReadRequest::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace tlc
} // namespace cache
} // namespace librbd
