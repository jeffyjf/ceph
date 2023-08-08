// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H

#include "librbd/plugin/Api.h"
#include "librbd/io/Types.h"
#include "librbd/cache/hac/LocalCacheSlot.h"

namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace hac {

template <typename ImageCtxT>
class ShutdownRequest {
public:
  ShutdownRequest(Context *on_finish, plugin::Api<ImageCtxT>& plugin_api, BlockDevice* bdev, std::vector<LocalCacheSlot*>& all_slots, ImageCtxT &image_ctx);
  void send();
private:

  void finish(int r);

  Context* m_on_finish;
  plugin::Api<ImageCtxT>& m_plugin_api;
  BlockDevice* m_bdev;
  std::vector<LocalCacheSlot*>& m_all_slots;
  std::vector<SlotRootRecord> m_slot_records;
  ImageCtxT& m_image_ctx;
};

} // namespace hac
} // namespace cache
} // namespace librbd

extern template class librbd::cache::hac::ShutdownRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H
