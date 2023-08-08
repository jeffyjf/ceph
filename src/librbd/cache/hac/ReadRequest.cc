// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#include "librbd/cache/hac/ReadRequest.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::ReadRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

void ReadRequest::push_to_read_from_rados(ExtentBufPtr extent_buf) {
  this->read_from_rados.push_back(extent_buf);
  this->read_extents.push_back(extent_buf);
}

void ReadRequest::push_to_read_from_hot_area(ExtentBufPtr extent_buf, Area* area) {
  this->read_from_hot_area.push_back(extent_buf);
  this->read_extents.push_back(extent_buf);
  this->read_areas.push_back(area);
}

void ReadRequest::send() {
  ldout(m_cct, 20) << dendl;
  auto gather_ctx = new C_Gather(m_cct, new LambdaContext([this](int r){
    for (auto ext : read_extents) {
      m_out_bl->claim_append(ext->buf);
    }
    this->finish(r);
  }));
  for (auto ext_buf : this->read_from_rados) {
    ldout(m_cct, 20) << "Read from rados: " << ext_buf << dendl;
    m_image_write_through.aio_read(
      {ext_buf->get_extent()}, &(ext_buf->buf), 0, gather_ctx->new_sub());
  }
  for (uint64_t i=0; i<read_areas.size(); i++) {
    ldout(m_cct, 20) << "Read from hot area: " << *(read_from_hot_area[i]) << dendl;
    auto area = read_areas[i];
    auto slot = area->get_cache_slot();
    slot->read(read_from_hot_area[i], m_tid, m_cct, gather_ctx->new_sub());
  }
  gather_ctx->activate();
}

void ReadRequest::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd
