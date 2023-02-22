// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_INIT_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_INIT_REQUEST_H

#include "librbd/io/Types.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/LocalCacheSlot.h"
#include "librbd/cache/ImageWriteThrough.h"

namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace hac {

template <typename ImageCtxT>
class HotAreaCache;

template <typename ImageCtxT, typename HotAreaCacheT>
class InitRequest {
public:
  InitRequest(Context *on_finish, HotAreaCacheT& hac, ImageCtxT &image_ctx,
              plugin::Api<ImageCtxT> &plugin_api,
              cache::ImageWriteThroughInterface& image_write_through);
  void send();
private:
  void finish(int r);

  void init_area_and_load_records(Context* on_finish);
  void init_local_cache_file_and_slots(Context* on_finish);
  void create_new_local_cache_file(Context* on_finish, std::string slot_file);
  void check_existing_local_cache_file(Context* on_finish, std::string slot_file);
  void associate_area_and_slot();

  Context* m_on_finish;
  HotAreaCacheT& m_hac;
  ImageCtxT &m_image_ctx;
  plugin::Api<ImageCtxT> &m_plugin_api;
  bufferlist m_tmp_bl;
  std::vector<AreaRecord> m_area_record_vec;
  std::vector<SlotRootRecord> m_slot_root_record_vec;
  cache::ImageWriteThroughInterface &m_image_write_through;
  uint64_t m_cache_file_size;

};

} // namespace hac
} // namespace cache
} // namespace librbd

extern template class librbd::cache::hac::InitRequest<librbd::ImageCtx, librbd::cache::hac::HotAreaCache<librbd::ImageCtx>>;

#endif // CEPH_LIBRBD_CACHE_HAC_INIT_REQUEST_H
