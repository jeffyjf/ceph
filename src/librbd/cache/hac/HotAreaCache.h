// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H
#define CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H

#include "include/Context.h"
#include "include/rbd/librbd.h"

#include "common/WorkQueue.h"
#include "common/ceph_mutex.h"
#include "common/Thread.h"
#include "common/Cond.h"

#include "librbd/io/Types.h"
#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/LocalCacheSlot.h"
#include "librbd/cache/hac/InitRequest.h"
#include "librbd/cache/hac/Types.h"




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
  void discard(
      uint64_t offset, uint64_t length,
      uint32_t discard_granularity_bytes,
      Context *on_finish);
  void writesame(
      uint64_t offset, uint64_t length,
      ceph::bufferlist&& bl,
      int fadvise_flags, Context *on_finish);
  void compare_and_write(
      io::Extents&& image_extents,
      ceph::bufferlist&& cmp_bl, ceph::bufferlist&& bl,
      uint64_t *mismatch_offset,int fadvise_flags,
      Context *on_finish);
  void invalidate(Context *on_finish);

  void init_all_areas_vec(uint64_t image_size);

  void handle_resize_image(uint64_t old_size, uint64_t new_size);

private:

  void increase_area_read_count(uint64_t area_offset);
  void update_hot_area();
  void assemble_write_same_extent(
    uint64_t length, const ceph::bufferlist& data,
    ceph::bufferlist *ws_data, uint64_t data_off, uint64_t& next_data_off);
  void update_area();
  void update_system_area_index(std::set<uint64_t> system_area_indexes);
  void update_common_area_index(std::set<uint64_t> system_area_indexes);
  void system_boot_timing(uint64_t sysboot_seconds);

  std::vector<Area*> m_all_areas;
  AreaOffsetSet m_current_hot_areas;
  AreaCountSet m_system_recorded_areas;
  AreaCountSet m_common_recorded_areas;

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

  void perf_start(const std::string name);
  void perf_stop();

  ImageCtxT &m_image_ctx;
  plugin::Api<ImageCtxT> &m_plugin_api;
  cache::ImageWriteThroughInterface &m_image_write_through;
  ThreadPool m_thread_pool;
  ContextWQ m_work_queue;
  BlockDevice* m_bdev = nullptr;
  
  //resize watcher
  struct Watcher {
    HotAreaCache* cache;
    rbd_image_t image; //void*
    uint64_t handle = -1;
    uint64_t old_size;
    static void cb(void *arg) {
      Watcher *watcher = static_cast<Watcher *>(arg);
      watcher -> handle_notify();
    }
    void handle_notify(){
      rbd_image_info_t info;
      int r = rbd_stat(image, &info, sizeof(info));
      ceph_assert(r==0);
      uint64_t tmp = old_size;
      uint64_t cur_size = static_cast<uint64_t>(info.size);
      old_size = cur_size;
      cache->handle_resize_image(tmp, cur_size);
    }
    explicit Watcher(HotAreaCache* c, rbd_image_t m, uint64_t s):cache(c), image(m), old_size(s){}
  } m_watcher;

  //If a system disk for vm, record the area offset in the first few seconds of startup
  class SystemBootTimerThread : public Thread {
    HotAreaCache *hac;
    uint64_t sysboot_seconds;
  public:
    explicit SystemBootTimerThread(HotAreaCache *o, uint64_t sysboot_seconds) : hac(o),
        sysboot_seconds {sysboot_seconds}{}
    void *entry() override {
      hac->system_boot_timing(sysboot_seconds);
      return 0;
    }
  } system_boot_timer_thread;
  bool system_boot_start;
  bool system_area_update;
  std::set<uint64_t> system_area_offsets;
  //Index of m_all_areas for areas. 
  std::vector<std::pair<uint64_t, uint64_t>> m_system_area_indexes;
  std::vector<std::pair<uint64_t, uint64_t>> m_common_area_indexes;
  uint64_t m_until_reset_count = UNTIL_RESET_AREA_READ_COUNT; 

  std::atomic<unsigned int> m_ws_tid{0}; // writesame tid;
  
  uint64_t m_read_count;
  
  PerfCounters *m_perfcounter;

  using This = HotAreaCache<ImageCtxT>;
  friend class librbd::cache::hac::InitRequest<ImageCtxT, This>;
  
};

std::string unique_lock_name(const std::string &name, void *address);

} // namespace hac
} // namespace cache
} // namespace librbd

extern template class librbd::cache::hac::HotAreaCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_HOT_AREA_CACHE_H
