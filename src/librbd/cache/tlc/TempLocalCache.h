// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TEMP_LOCAL_CACHE_H
#define CEPH_LIBRBD_CACHE_TEMP_LOCAL_CACHE_H

#include "include/Context.h"
#include "include/rbd/librbd.h"

#include "common/WorkQueue.h"
#include "common/ceph_mutex.h"
#include "common/Thread.h"
#include "common/Cond.h"

#include "librbd/io/Types.h"
#include "blk/BlockDevice.h"
#include "librbd/cache/ImageTempLocalWriteback.h"
#include "librbd/cache/tlc/InitRequest.h"


namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace tlc {


template <typename ImageCtxT>
class TempLocalCache {
public:
  TempLocalCache(ImageCtxT &image_ctx, 
      cache::ImageTempLocalWritebackInterface& image_temp_local_writeback, 
      plugin::Api<ImageCtxT>& plugin_api);
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
  


private:
  
  class InvalidateCacheThread : public Thread {
    TempLocalCache *tlc;
  public:
    explicit InvalidateCacheThread(TempLocalCache *o) : tlc(o) {}
    void *entry() override {
      tlc->invalidate_part_cache();
      return 0;
    }
  } m_invalidate_cache_thread;
  bool invalidater_stop;
  std::chrono::duration<int> invalidate_interval;
  ceph::condition_variable invalidater_cond;

  ceph::mutex m_cache_lock;
  ImageCtxT &m_image_ctx;
  plugin::Api<ImageCtxT> &m_plugin_api;
  cache::ImageTempLocalWritebackInterface &m_image_temp_local_writeback;
  ThreadPool m_thread_pool;
  ContextWQ m_work_queue;

  //resize watcher
  struct Watcher {
    TempLocalCache* cache;
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
    explicit Watcher(TempLocalCache* c, rbd_image_t m, uint64_t s):cache(c), image(m), old_size(s){}
  } m_watcher;

  BlockDevice* m_bdev;

  std::atomic<uint64_t> m_cached_bytes {0}; 
  std::uint64_t m_cache_file_size;
  std::string m_cache_file_path;
  std::string m_metadata_file_path;

  std::vector<std::map<uint64_t, uint64_t>> m_cache_md_vec;

  std::atomic<unsigned int> m_ws_tid{0}; // writesame tid;

  void start();
  void stop();
  void invalidate_part_cache();
  void handle_resize_image(uint64_t old_size, uint64_t new_size);
  void assemble_write_same_extent(
    uint64_t length, const ceph::bufferlist& data,
    ceph::bufferlist *ws_data, uint64_t data_off);

  using This = TempLocalCache<ImageCtxT>;
  friend class librbd::cache::tlc::InitRequest<ImageCtxT, This>;

};


} // namespace tlc
} // namespace cache
} // namespace librbd

extern template class librbd::cache::tlc::TempLocalCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_TEMP_LOCAL_CACHE_H
