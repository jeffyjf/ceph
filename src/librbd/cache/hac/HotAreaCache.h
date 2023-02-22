// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H
#define CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H

#include "include/Context.h"

#include "common/WorkQueue.h"
#include "common/ceph_mutex.h"
#include "common/Thread.h"
#include "common/Cond.h"

#include "librbd/io/Types.h"
#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/LocalCacheSlot.h"
#include "librbd/cache/hac/InitRequest.h"




namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace hac {


template <typename ImageCtxT>
class HotAreaCache {
public:
  HotAreaCache(ImageCtxT &image_ctx, cache::ImageWriteThroughInterface& image_write_through, plugin::Api<ImageCtxT>& plugin_api);
  void init(Context *on_finish);
  void shut_down(Context *on_finish);

  /// IO methods
  void read(
      io::Extents&& image_extents, ceph::bufferlist *bl,
      int fadvise_flags, Context *on_finish, uint64_t tid);
  void write(
      io::Extents&& image_extents, ceph::bufferlist&& bl,
      int fadvise_flags, Context *on_finish, uint64_t tid);

  void init_all_areas_vec(uint64_t image_size);


private:

  void increase_area_read_count(uint64_t area_offset);
  void update_hot_area();



  std::vector<Area*> m_all_areas;
  AreaOffsetSet m_current_hot_areas;
  AreaCountSet m_all_recorded_areas;

  std::vector<LocalCacheSlot*> m_all_slots;
  CacheSlotSet m_free_slots;

  class HotAreaUpdaterThread : public Thread {
    HotAreaCache *hac;
  public:
    explicit HotAreaUpdaterThread(HotAreaCache *o) : hac(o) {}
    void *entry() override {
      hac->update_hot_area();
      return 0;
    }
  } hot_area_updater_thread;

  bool updater_stop;
  std::chrono::duration<int> update_interval;
  ceph::condition_variable updater_cond;
  ceph::mutex m_cache_lock;

  void start();
  void stop();


  ImageCtxT &m_image_ctx;
  plugin::Api<ImageCtxT> &m_plugin_api;
  cache::ImageWriteThroughInterface &m_image_write_through;
  ThreadPool m_thread_pool;
  ContextWQ m_work_queue;
  BlockDevice* m_bdev = nullptr;

  using This = HotAreaCache<ImageCtxT>;
  friend class librbd::cache::hac::InitRequest<ImageCtxT, This>;

};

std::string unique_lock_name(const std::string &name, void *address);

} // namespace hac
} // namespace cache
} // namespace librbd

extern template class librbd::cache::hac::HotAreaCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H
