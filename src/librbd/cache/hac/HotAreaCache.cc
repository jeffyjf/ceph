// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/denc.h"

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/io/Types.h"
#include "librbd/cache/hac/HotAreaCache.h"
#include "librbd/cache/hac/SyncRequest.h"
#include "librbd/cache/hac/ShutdownRequest.h"
#include "librbd/cache/hac/ReadRequest.h"
#include "librbd/cache/hac/WriteRequest.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::HotAreaCache: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

template <typename I>
HotAreaCache<I>::HotAreaCache(
    I &image_ctx, cache::ImageWriteThroughInterface& image_write_through, plugin::Api<I>& plugin_api)
  : hot_area_updater_thread(this), updater_stop(false), update_interval(30),
    m_cache_lock(ceph::make_mutex(unique_lock_name("librbd::cache::hac::HotAreaCache::m_cache_lock", this))),
    m_image_ctx(image_ctx),
    m_plugin_api(plugin_api), m_image_write_through(image_write_through),
    m_thread_pool(image_ctx.cct, "librbd::cache::hac::HotAreaCache::thread_pool", "tp_pwl", 1, ""),
    m_work_queue("librbd::cache::hac::HotAreaCache::work_queue",
                 ceph::make_timespan(
                   image_ctx.config.template get_val<uint64_t>(
		                 "rbd_op_thread_timeout")),
                 &m_thread_pool) {}

template <typename I>
void HotAreaCache<I>::init(Context *on_finish) {
    auto cct = m_image_ctx.cct;
    ldout(cct, 20) << dendl;
    auto ctx = new LambdaContext([this, on_finish](int r){
      if (r>=0) {
        start();
        m_thread_pool.start();
      } else {
        // TODO
      }
      on_finish->complete(r);
    });

    auto init_req = new InitRequest<I, This>(ctx, *this, m_image_ctx, m_plugin_api, m_image_write_through);
    init_req->send();
}

template <typename I>
void HotAreaCache<I>::shut_down(Context *on_finish) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  m_thread_pool.stop();
  stop();
  auto sync_ctx = new LambdaContext([this, on_finish, cct](int r) {
    auto shut_down_ctx = new LambdaContext([this, on_finish, cct](int r) {
      if (r<0) {
        // TODO
      }
      on_finish->complete(r);
    });
    auto shut_down_req = new ShutdownRequest(shut_down_ctx, m_bdev, m_all_slots, cct);
    shut_down_req->send();
  });
  auto sync_req = new SyncRequest(sync_ctx, m_image_ctx, m_plugin_api, m_all_recorded_areas, m_bdev);
  sync_req->send();
}

template <typename I>
void HotAreaCache<I>::increase_area_read_count(uint64_t area_offset) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << area_offset << dendl;
  auto ctx = new LambdaContext([this, area_offset, cct](int r){
    uint64_t index = area_offset >> AREA_SIZE_ORDER;
    if (m_all_areas[index] == nullptr) {
      ldout(cct, 20) << "Create new area" << dendl;
      m_all_areas[index] = new Area(area_offset);
    } else {
      ldout(cct, 20) << "Increase existing area's count." << dendl;
      m_all_areas[index]->increase_count();
    }
  });
  m_work_queue.queue(ctx, 0);
}

template <typename I>
void HotAreaCache<I>::read(
      io::Extents&& image_extents, ceph::bufferlist *bl,
      int fadvise_flags, Context *on_finish, uint64_t tid){

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << tid << dendl;

  ReadRequest *read_ctx = new ReadRequest(cct, bl, m_image_write_through, on_finish, tid);

  for (auto &extent : image_extents) {
    uint64_t extent_end = extent.first + extent.second;
    uint64_t area_offset = extent.first & AREA_OFFSET_MASK;
    do {
      uint32_t index = area_offset >> AREA_SIZE_ORDER;
      auto area = m_all_areas[index];
      uint64_t offset = std::max(extent.first, area_offset);
      uint64_t len = std::min(extent_end-offset, area_offset+AREA_SIZE-offset);

      auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{offset, len});

      if (area != nullptr && area->hot.load()) {
        read_ctx->push_to_read_from_hot_area(extent_buf, area);
      } else {
        read_ctx->push_to_read_from_rados(extent_buf);
      }
      increase_area_read_count(area_offset);
      area_offset += AREA_SIZE;
    }while(extent_end > area_offset);
  }
  read_ctx->send();
}

template <typename I>
void HotAreaCache<I>::write(
      io::Extents&& image_extents, ceph::bufferlist&& bl,
      int fadvise_flags, Context *on_finish, uint64_t tid) {

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "tid=" << tid << dendl;

  auto gather_ctx = new C_Gather(cct, on_finish);

  WriteRequest *write_ctx = new WriteRequest(cct, gather_ctx->new_sub(), tid);
  uint64_t bl_offset = 0;
  for (auto &extent : image_extents) {
    uint64_t extent_end = extent.first + extent.second;
    uint64_t area_offset = extent.first & AREA_OFFSET_MASK;
    do{
      uint32_t index = area_offset >> AREA_SIZE_ORDER;
      auto area = m_all_areas[index];

      auto sub_offset = std::max(extent.first, area_offset);
      auto sub_len = std::min(extent_end-sub_offset, area_offset+AREA_SIZE-sub_offset);
      if (area != nullptr && area->hot.load()) {
        auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{sub_offset, sub_len});
        extent_buf->buf.substr_of(bl, bl_offset, sub_len);
        write_ctx->push_to_update_hot_area(extent_buf, area);
      }
      area_offset += AREA_SIZE;
      bl_offset += sub_len;
    }while(extent_end > area_offset);
  }
  write_ctx->send();
  m_image_write_through.aio_write(std::move(image_extents), std::move(bl), fadvise_flags, gather_ctx->new_sub());
  gather_ctx->activate();
}

template <typename I>
void HotAreaCache<I>::update_hot_area() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::unique_lock l{m_cache_lock};
  while (!updater_stop) {
    auto sort_ctx = new LambdaContext([this, cct](int r) {
      ldout(cct, 10) << "Starting update hot areas, reorder areas by their access count." << dendl;
      m_all_recorded_areas.clear();
      for (auto area : m_all_areas) {
        if (area == nullptr) {
          continue;
        }
        m_all_recorded_areas.insert(area);
      }

      AreaOffsetSet tmp_hot_areas;
      uint64_t i=0;
      for (auto it=m_all_recorded_areas.begin(); it!=m_all_recorded_areas.end(); it++) {
        ldout(cct, 25) << "Recorded area: " << **it << dendl;
        if (i>=m_all_slots.size()) {
          break;
        }
        tmp_hot_areas.insert(*it);
        i++;
      }


      AreaOffsetSet need_retire_areas;

      for (auto it=m_current_hot_areas.begin(); it != m_current_hot_areas.end(); it++) {
        if (tmp_hot_areas.find(*it) == tmp_hot_areas.end()) {
          need_retire_areas.insert(*it);
        }
      }

      ldout(cct, 10) << "The size of areas need retire from local cache slot is: " << need_retire_areas.size() << dendl;
      std::for_each(
        need_retire_areas.begin(),
        need_retire_areas.end(),
        [this, cct](const Area* area) {
          ldout(cct, 25) << "Retireing area: " << *area << dendl;
          Area* area_ptr = const_cast<Area*>(area);
          auto slot = area_ptr->retire(cct);
          if (slot != nullptr) {
            m_free_slots.insert(slot);
          }
        });
      m_current_hot_areas.clear();

      std::copy(tmp_hot_areas.begin(), tmp_hot_areas.end(), std::inserter(m_current_hot_areas, m_current_hot_areas.end()));
    });
    m_work_queue.queue(sort_ctx, 0);

    auto sync_ctx = new LambdaContext([this, cct](int r) {
      ldout(cct, 20) << "Syncing all area records to rados object..." << dendl;
      auto on_finish = new LambdaContext([this](int r){});
      auto sync_req = new SyncRequest(on_finish, m_image_ctx, m_plugin_api, m_all_recorded_areas, m_bdev);
      sync_req->send();
    });
    m_work_queue.queue(sync_ctx, 0);
    updater_cond.wait_for(l, std::chrono::duration<int>(1));
    auto process_hot_ctx = new LambdaContext([this, cct](int r) {
      ldout(cct, 10) << "Current hot areas size is: " << m_current_hot_areas.size() << dendl;
      std::for_each(
        m_current_hot_areas.begin(),
        m_current_hot_areas.end(),
        [this, cct](const Area *area) {
          Area* area_ptr = const_cast<Area*>(area);
          if (!area_ptr->hot.load()) {
            auto it = m_free_slots.begin();
            ldout(cct, 20) << "Allocate slot: " << **it << " to area: " << *area_ptr << dendl;
            area_ptr->hire(*it, m_bdev);
            m_free_slots.erase(it);
          }
          ceph_assert(area_ptr->get_index()==area_ptr->get_cache_slot()->get_area_index());
        });
      ldout(cct, 20) << m_current_hot_areas.size() << " : " << m_free_slots.size() << " : " << m_all_slots.size() << dendl;
      ceph_assert(m_current_hot_areas.size()+m_free_slots.size()==m_all_slots.size());
    });
    m_work_queue.queue(process_hot_ctx, 0);

    updater_cond.wait_for(l, update_interval);

    if (updater_stop)
      break;
  }
}

template <typename I>
void HotAreaCache<I>::start() {
  hot_area_updater_thread.create("hac-updater");
}

template <typename I>
void HotAreaCache<I>::stop() {
    ceph_assert(hot_area_updater_thread.is_started());
    m_cache_lock.lock();
    updater_stop = true;
    updater_cond.notify_all();
    m_cache_lock.unlock();
    hot_area_updater_thread.join();
}


std::string unique_lock_name(const std::string &name, void *address) {
  return name + " (" + stringify(address) + ")";
}

} // namespace hac
} // namespace cache
} // namespace librbd

template class librbd::cache::hac::HotAreaCache<librbd::ImageCtx>;
