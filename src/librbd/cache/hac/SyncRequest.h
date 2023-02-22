// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_SYNC_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_SYNC_REQUEST_H

#include "librbd/cache/hac/Area.h"

namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace hac {

template <typename ImageCtxT>
class SyncRequest {
public:
  SyncRequest(Context* on_finish, ImageCtxT& image_ctx, plugin::Api<ImageCtxT>& plugin_api, AreaCountSet& recorded_areas, BlockDevice* bdev);
  void send();
private:

  void finish(int r);
  void sync_area_records(Context* ctx);
  void sync_slot_timestamp(Context* ctx);

  Context* m_on_finish;
  ImageCtxT& m_image_ctx;
  plugin::Api<ImageCtxT>& m_plugin_api;
  AreaCountSet& m_recorded_areas;
  BlockDevice* m_bdev;

  std::vector<AreaRecord> m_area_record_vec;
};

} // namespace hac
} // namespace cache
} // namespace librbd

extern template class librbd::cache::hac::SyncRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_HAC_SYNC_REQUEST_H