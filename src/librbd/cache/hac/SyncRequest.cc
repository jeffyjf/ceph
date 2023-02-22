// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/cache/hac/SyncRequest.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/LocalCacheSlot.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::SyncRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

template <typename I>
SyncRequest<I>::SyncRequest(
  Context* on_finish, I& image_ctx,
  plugin::Api<I>& plugin_api,
  AreaCountSet& recorded_areas, BlockDevice* bdev) :
m_on_finish(on_finish), m_image_ctx(image_ctx),
m_plugin_api(plugin_api), m_recorded_areas(recorded_areas), m_bdev(bdev) {}

template <typename I>
void SyncRequest<I>::send() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  auto gater_ctx = new C_Gather(cct, new LambdaContext([this, cct](int r) {
    if (r<0) {
      // TODO
    }
    this->finish(r);
  }));

  sync_area_records(gater_ctx->new_sub());
  sync_slot_timestamp(gater_ctx->new_sub());

  gater_ctx->activate();
}

template <typename I>
void SyncRequest<I>::sync_slot_timestamp(Context* ctx) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  SlotRootHeader header{0, ceph_clock_now(), false};
  bufferlist header_bl;
  encode(header, header_bl);
  header_bl.append_zero(BLOCK_SIZE - header_bl.length());
  int r = m_bdev->write(0, header_bl, false);
  ctx->complete(r);
}

template <typename I>
void SyncRequest<I>::sync_area_records(Context* ctx) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  for (auto area : m_recorded_areas) {
    ldout(cct, 20) << "Area record: " << *area << dendl;
    m_area_record_vec.push_back(area->get_record());
  }
  bufferlist record_bl;
  encode(m_area_record_vec, record_bl);
  RecordHeader header{record_bl.length()};
  ldout(cct, 20) << "Area records data len: " << header.data_len << dendl;
  bufferlist header_bl;
  encode(header, header_bl);
  header_bl.append_zero(RECORD_HEADER_SIZE - header_bl.length());
  bufferlist total_bl;
  total_bl.claim_append(header_bl);
  total_bl.claim_append(record_bl);
  std::string record_obj_name = area_record_object_name(m_image_ctx.id);
  m_plugin_api.rados_object_write(&m_image_ctx, ctx, record_obj_name, total_bl);
}

template <typename I>
void SyncRequest<I>::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd

template class librbd::cache::hac::SyncRequest<librbd::ImageCtx>;
