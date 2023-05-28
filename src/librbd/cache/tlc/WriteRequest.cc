// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/Context.h"
#include "librbd/cache/tlc/WriteRequest.h"
#include "librbd/cache/tlc/Types.h"

#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::tlc::WriteRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace tlc {

WriteRequest::WriteRequest(
      CephContext *cct, Context *on_finish, uint64_t tid, io::Extents& image_extents, bufferlist&& bl, BlockDevice* bdev):
      m_cct(cct), m_on_finish(on_finish), m_tid(tid),  m_image_extents(image_extents), m_bl{bl}, m_bdev(bdev){}

void WriteRequest::send() {
  auto ctx = new LambdaContext([this](int r){
    finish(r);
  });
  auto gather_ctx = new C_Gather(m_cct, ctx);
  uint64_t offset=0;
  for(auto image_extent: m_image_extents){
    bufferlist write_bl;
    write_bl.substr_of(m_bl, offset, image_extent.second);
    AioTransContext* aio = new AioTransContext(m_cct, gather_ctx->new_sub());
    m_bdev->aio_write(image_extent.first, write_bl, &aio->ioc, false, WRITE_LIFE_NOT_SET);
    m_bdev->aio_submit(&aio->ioc);
    offset += image_extent.second;
  }
  gather_ctx->activate();
}

void WriteRequest::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace tlc
} // namespace cache
} // namespace librbd
