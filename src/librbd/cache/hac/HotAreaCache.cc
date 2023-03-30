// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/denc.h"

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/io/Types.h"
#include "librbd/cache/hac/CompareAndWriteRequest.h"
#include "librbd/cache/hac/DiscardRequest.h"
#include "librbd/cache/hac/HotAreaCache.h"
#include "librbd/cache/hac/SyncRequest.h"
#include "librbd/cache/hac/ShutdownRequest.h"
#include "librbd/cache/hac/Types.h"
#include "librbd/cache/hac/ReadRequest.h"
#include "librbd/cache/hac/WriteRequest.h"
#include "librbd/cache/hac/WriteSameRequest.h"

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
                  &m_thread_pool),
    m_watcher(this, static_cast<rbd_image_t>(&image_ctx), 
              static_cast<uint64_t>(m_plugin_api.get_image_size(&m_image_ctx))),
    system_boot_timer_thread(this, m_image_ctx.config.template get_val<uint64_t>("rbd_hot_area_cache_sys_boot_seconds")) {}

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
    auto shut_down_req = new ShutdownRequest(shut_down_ctx, m_plugin_api, m_bdev, m_all_slots, m_image_ctx);
    shut_down_req->send();
  });
  auto sync_req = new SyncRequest(sync_ctx, m_image_ctx, m_plugin_api, m_system_recorded_areas, m_common_recorded_areas, m_bdev);
  sync_req->send();
}

template <typename I>
void HotAreaCache<I>::increase_area_read_count(uint64_t area_offset) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << area_offset << dendl;
  auto ctx = new LambdaContext([this, area_offset, cct](int r){
    uint64_t index = area_offset >> area_conf()->size_order();
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
  
  m_read_count++;
  ReadRequest *read_ctx = new ReadRequest(cct, bl, m_image_write_through, on_finish, tid);

  int area_hit_num = 0;
  int area_miss_num = 0;
  for (auto &extent : image_extents) {
    uint64_t extent_end = extent.first + extent.second;
    uint64_t area_offset = extent.first & area_conf()->offset_mask();
    
    //If a system disk for vm, record the area offset in the first few seconds of startup
    if (system_boot_start){
        system_area_offsets.insert(area_offset);
    }

    do {
      uint32_t index = area_offset >> area_conf()->size_order();
      auto area = m_all_areas[index];
      uint64_t offset = std::max(extent.first, area_offset);
      uint64_t len = std::min(extent_end-offset, area_offset+area_conf()->size()-offset);

      auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{offset, len});

      if (area != nullptr && area->hot.load()) {
        read_ctx->push_to_read_from_hot_area(extent_buf, area);
        area_hit_num++;
      } else {
        read_ctx->push_to_read_from_rados(extent_buf);
        area_miss_num++;
      }
      increase_area_read_count(area_offset);
      area_offset += area_conf()->size();
    }while(extent_end > area_offset);
  }
  read_ctx->send();
  m_perfcounter->inc(l_librbd_hac_area_hit, area_hit_num);
  m_perfcounter->inc(l_librbd_hac_area_miss, area_miss_num);
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
    uint64_t area_offset = extent.first & area_conf()->offset_mask();
    do{
      uint32_t index = area_offset >> area_conf()->size_order();
      auto area = m_all_areas[index];

      auto sub_offset = std::max(extent.first, area_offset);
      auto sub_len = std::min(extent_end-sub_offset, area_offset+area_conf()->size()-sub_offset);
      if (area != nullptr && area->hot.load()) {
        auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{sub_offset, sub_len});
        extent_buf->buf.substr_of(bl, bl_offset, sub_len);
        write_ctx->push_to_update_hot_area(extent_buf, area);
      }
      area_offset += area_conf()->size();
      bl_offset += sub_len;
    }while(extent_end > area_offset);
  }
  write_ctx->send();
  m_image_write_through.aio_write(std::move(image_extents), std::move(bl), fadvise_flags, gather_ctx->new_sub());
  gather_ctx->activate();
}

template <typename I>
void HotAreaCache<I>::discard(uint64_t offset, uint64_t length,
      uint32_t discard_granularity_bytes, Context *on_finish) {
        
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  DiscardRequest *discard_ctx = new DiscardRequest(cct);  
  uint64_t extent_end = offset + length;
  uint64_t area_offset = offset & area_conf()->offset_mask();
  do{
    uint32_t index = area_offset >> area_conf()->size_order();
    auto area = m_all_areas[index];

    auto sub_offset = std::max(offset, area_offset);
    auto sub_len = std::min(extent_end-sub_offset, area_offset+area_conf()->size()-sub_offset);
    if (area != nullptr && area->hot.load()) {
      auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{sub_offset, sub_len});
      discard_ctx->push_to_update_hot_area(extent_buf, area);
    }
    area_offset += area_conf()->size();
  }while(extent_end > area_offset);
  discard_ctx->send();
  m_image_write_through.aio_discard(offset, length, discard_granularity_bytes, on_finish);
}

template <typename I>
void HotAreaCache<I>::writesame(uint64_t offset, uint64_t length,
                                 bufferlist&& bl, int fadvise_flags,
                                 Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;

  auto gather_ctx = new C_Gather(cct, on_finish);
  WriteSameRequest *ws_ctx = new WriteSameRequest(cct, gather_ctx->new_sub(), m_ws_tid++);
  uint64_t bl_offset = 0;
  uint64_t extent_end = offset + length;
  uint64_t area_offset = offset & area_conf()->offset_mask();
  size_t data_len = bl.length();
  uint64_t next_data_off = length % data_len;
  do{
    //area index
    uint32_t index = area_offset >> area_conf()->size_order();
    auto area = m_all_areas[index];
    auto sub_offset = std::max(offset, area_offset);
    auto sub_len = std::min(extent_end-sub_offset, area_offset+area_conf()->size()-sub_offset);
    if (area != nullptr && area->hot.load()) {
      auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{sub_offset, sub_len});
	    assemble_write_same_extent(
            sub_len, bl, &extent_buf->buf, next_data_off, next_data_off);
	    ws_ctx->push_to_update_hot_area(extent_buf, area);
    }
    area_offset += area_conf()->size();
    bl_offset += sub_len;
  }while(extent_end > area_offset);
  ws_ctx->send();
  m_image_write_through.aio_writesame(offset, length, std::move(bl), fadvise_flags, gather_ctx->new_sub());
  gather_ctx->activate();
}

template <typename I>
void HotAreaCache<I>::assemble_write_same_extent(
  uint64_t length, const ceph::bufferlist& data,
  ceph::bufferlist *ws_data, uint64_t data_off, uint64_t& next_data_off) {
  size_t data_len = data.length();
  bufferlist sub_bl;
  uint64_t sub_off = data_off;
  uint64_t sub_len = data_len - data_off;
  uint64_t extent_left = length;
  while (extent_left >= sub_len) {
    sub_bl.substr_of(data, sub_off, sub_len);
    ws_data->claim_append(sub_bl);
    extent_left -= sub_len;
    if (sub_off) {
      sub_off = 0;
      sub_len = data_len;
    }
  }
  if (extent_left) {
    sub_bl.substr_of(data, sub_off, extent_left);
    ws_data->claim_append(sub_bl);
  }
	next_data_off = sub_off + extent_left;
	if (data_len <= next_data_off){
	  next_data_off = next_data_off % data_len; 
	}
}

template <typename I>
void HotAreaCache<I>::compare_and_write(
      io::Extents&& image_extents,
      ceph::bufferlist&& cmp_bl, ceph::bufferlist&& bl,
      uint64_t *mismatch_offset,int fadvise_flags,
      Context *on_finish){

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  CompareAndWriteRequest *caw_ctx = new CompareAndWriteRequest(cct);  
  for (auto &extent : image_extents) {
    uint64_t extent_end = extent.first + extent.second;
    uint64_t area_offset = extent.first & area_conf()->offset_mask();
    do{
      uint32_t index = area_offset >> area_conf()->size_order();
      auto area = m_all_areas[index];
      auto sub_offset = std::max(extent.first, area_offset);
      auto sub_len = std::min(extent_end-sub_offset, area_offset+area_conf()->size()-sub_offset);
      if (area != nullptr && area->hot.load()) {
        auto extent_buf = std::make_shared<ExtentBuf>(io::Extent{sub_offset, sub_len});
        caw_ctx->push_to_update_hot_area(extent_buf, area);
      }
      area_offset += area_conf()->size();
    }while(extent_end > area_offset);
  }
  // CAW Op is not supported, so invalidate involved cache.
  caw_ctx->send();
  m_image_write_through.aio_compare_and_write(
    std::move(image_extents),  std::move(cmp_bl), std::move(bl),
    mismatch_offset, fadvise_flags, on_finish);
}

template <typename I>
void HotAreaCache<I>::invalidate(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  
  auto ctx = new LambdaContext([this, cct, on_finish](int r){
    if (r<0) {
      lderr(cct) << "Invalidate hac cache failed, errno is " << r << dendl;
    } 
    on_finish->complete(r);
  });
 
  bufferlist bl;
  bl.append(HEADER_OBJECT_XATTR_INVALID_CACHE_VALUE.c_str(), HEADER_OBJECT_XATTR_INVALID_CACHE_VALUE.length());
  m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, m_image_ctx.header_oid, HEADER_OBJECT_XATTR_VALID_CACHE_KEY, bl);
}

template <typename I>
void HotAreaCache<I>::update_hot_area() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  uint64_t last_read_count;
  std::unique_lock l{m_cache_lock};
  while (!updater_stop) {
    if (system_area_update){
      system_area_update = false;
      update_area();
    }
    
    auto sort_ctx = new LambdaContext([this, cct](int r) {
      utime_t start_time = ceph_clock_now();

      ldout(cct, 10) << "Starting update hot areas, reorder areas by their access count." << dendl;
      //Sort common area.
      m_common_recorded_areas.clear();
      for(auto area_index : m_common_area_indexes){
        for(uint64_t i=area_index.first; i<=area_index.second; i++){
          Area* area = m_all_areas[i];
          if (area == nullptr) {
            continue;
          }
          m_common_recorded_areas.insert(area);
        }
      }

      //tmp_hot_areas = m_system_recorded_areas + part of m_common_recorded_areas
      uint64_t remain_slot_size = m_all_slots.size() - m_system_recorded_areas.size();
      uint64_t recorded_comm_areas_size = m_common_recorded_areas.size();
      uint64_t size = std::min(remain_slot_size, recorded_comm_areas_size); 
      AreaOffsetSet::iterator comm_it = m_common_recorded_areas.begin();
      std::advance(comm_it, size); 
      AreaOffsetSet tmp_hot_areas(m_common_recorded_areas.begin(), comm_it);
      ceph_assert(tmp_hot_areas.size() == size);
      tmp_hot_areas.insert(m_system_recorded_areas.begin(), m_system_recorded_areas.end());
      ceph_assert(tmp_hot_areas.size() == size + m_system_recorded_areas.size());

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

      m_current_hot_areas = std::move(tmp_hot_areas);
      
      // Reset read count of common areas. 
      if (--m_until_reset_count==0){
        ldout(cct, 10) << "Reset read count." << dendl;
        m_until_reset_count=UNTIL_RESET_AREA_READ_COUNT; 
        AreaCountSet::iterator bound_it = m_common_recorded_areas.begin();
        std::advance(bound_it, size);
        for (auto it=m_common_recorded_areas.begin(); it!=bound_it; it++){
          (*it)->set_count(HOT_AREA_READ_COUNT);
        }
        for (auto it=bound_it; it!=m_common_recorded_areas.end(); it++){
          (*it)->set_count(NOT_HOT_AREA_READ_COUNT);
        }
      }

      utime_t end_time = ceph_clock_now();
      m_perfcounter->tinc(l_librbd_hac_update_area_avg_time, end_time-start_time);
      if (!m_system_recorded_areas.empty()){
        m_perfcounter->set(l_librbd_hac_sys_hot_area_max_read, (*m_system_recorded_areas.begin())->get_count());
        m_perfcounter->set(l_librbd_hac_sys_hot_area_min_read, (*m_system_recorded_areas.rbegin())->get_count());
      }
      if (!m_common_recorded_areas.empty()){
        m_perfcounter->set(l_librbd_hac_comm_hot_area_max_read, (*m_common_recorded_areas.begin())->get_count());
        m_perfcounter->set(l_librbd_hac_comm_hot_area_min_read, (*m_common_recorded_areas.rbegin())->get_count());
      }
    });
    m_work_queue.queue(sort_ctx, 0);

    auto sync_ctx = new LambdaContext([this, cct](int r) {
      ldout(cct, 20) << "Syncing all area records to rados object..." << dendl;
      auto on_finish = new LambdaContext([this](int r){});
      auto sync_req = new SyncRequest(on_finish, m_image_ctx, m_plugin_api, m_system_recorded_areas, m_common_recorded_areas, m_bdev);
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
      uint64_t hot_area_size = m_current_hot_areas.size();
      uint64_t free_slot_size = m_free_slots.size();
      ldout(cct, 20) << hot_area_size << " : " << free_slot_size << " : " << m_all_slots.size() << dendl;
      ceph_assert(hot_area_size+free_slot_size==m_all_slots.size());
      m_perfcounter->set(l_librbd_hac_free_slot, free_slot_size);
      m_perfcounter->set(l_librbd_hac_valid_slot, hot_area_size);
    });
    m_work_queue.queue(process_hot_ctx, 0);
    
    // If there is no read op during interval, wait again. 
    last_read_count = m_read_count;
    while (m_read_count == last_read_count && !updater_stop)
      updater_cond.wait_for(l, update_interval);
  }
}

template <typename I>
void HotAreaCache<I>::update_area(){
  if (system_area_offsets.empty())
    return;

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  // sys_area_indexes = [2,3,5,7] =>  
  //   m_common_area_indexes = [(0,1),(4,4),(6,6),(8,x)]
  //   m_system_area_indexes = [(2,3),(5,5),(7,7)]

  // System area offset => system area index
  uint64_t area_order = area_conf()->size_order();
  std::set<uint64_t> sys_area_indexes;
  for (auto area_offset : system_area_offsets){
    sys_area_indexes.insert(area_offset >> area_order);
  }

  // Update the index range of the common area
  update_common_area_index(sys_area_indexes);
          
  // Update the index range of the system area
  update_system_area_index(sys_area_indexes);
  
  //Remove system area that is not used by current system boot.
  for (auto it=m_system_recorded_areas.begin(); it!=m_system_recorded_areas.end();){
    if (system_area_offsets.find((*it)->get_offset()) == system_area_offsets.end()){
      it = m_system_recorded_areas.erase(it);
    }else{
      it++;
    }
  }
  //Add system area that is used by current system boot.
  AreaOffsetSet tmp_sys_areas(m_system_recorded_areas.begin(), m_system_recorded_areas.end());
  for(auto area_index : m_system_area_indexes){
    for(uint64_t i=area_index.first; i<=area_index.second; i++){
      Area* area_ptr = m_all_areas[i];
      ceph_assert(area_ptr != nullptr);
      tmp_sys_areas.insert(area_ptr);
    }
  }
  m_system_recorded_areas.clear();
  m_system_recorded_areas.insert(tmp_sys_areas.begin(),  tmp_sys_areas.end());
}

template <typename I>
void HotAreaCache<I>::update_common_area_index(std::set<uint64_t> system_area_indexes){
  m_common_area_indexes.clear();
  if (system_area_indexes.empty()){
    m_common_area_indexes.push_back(std::make_pair<uint64_t, uint64_t>(0,m_all_areas.size()-1));
    return;
  }

  auto it = system_area_indexes.begin();
  uint64_t pre_index = *it;
  //first
  if (pre_index!=0){
    m_common_area_indexes.push_back(std::make_pair<uint64_t, uint64_t>(0, pre_index-1));
  }
  //middle
  uint64_t next_index;
  std::advance(it, 1);
  for (; it!=system_area_indexes.end(); it++){
      next_index = *it;
      if (pre_index + 1 == next_index){
        pre_index = next_index;
        continue;
      }
      //[start, end]
      m_common_area_indexes.push_back(std::make_pair(pre_index+1, next_index-1));
      pre_index = next_index;
  }
  //end
  next_index = m_all_areas.size();
  m_common_area_indexes.push_back(std::make_pair(pre_index+1, next_index-1));
}

template <typename I>
void HotAreaCache<I>::update_system_area_index(std::set<uint64_t> system_area_indexes){
  m_system_area_indexes.clear();
  if (system_area_indexes.empty()){
    return;
  }
  
  auto it = system_area_indexes.begin();
  uint64_t pre_index = *it;
  uint64_t start_index = pre_index;
  std::advance(it, 1);
  uint64_t next_index;
  for (; it!=system_area_indexes.end(); it++){
      next_index = *it;
      if (pre_index + 1 == next_index){
        pre_index = next_index;
        continue;
      }else{
        //[start, end]
        m_system_area_indexes.push_back(std::make_pair(start_index, pre_index));
        pre_index = next_index;
        start_index = pre_index;
      }
  }
  m_system_area_indexes.push_back(std::make_pair(start_index, pre_index));
}

template <typename I>
void HotAreaCache<I>::system_boot_timing(uint64_t sysboot_seconds){
  if (sysboot_seconds > 0){
    system_boot_start=true;
    sleep(sysboot_seconds);
    system_boot_start=false;
    system_area_update=true;
  }
}

template <typename I>
void HotAreaCache<I>::start() {
  rbd_update_watch(m_watcher.image, &m_watcher.handle, Watcher::cb, &m_watcher);
  hot_area_updater_thread.create("hac-updater");
  system_boot_timer_thread.create("sys-boot-timer");
  auto pname = std::string("librbd-hac-") + m_image_ctx.id +
      std::string("-") + m_image_ctx.md_ctx.get_pool_name() +
      std::string("-") + m_image_ctx.name;
  perf_start(pname);
}

template <typename I>
void HotAreaCache<I>::stop() {  
  if(hot_area_updater_thread.is_started()){
    m_cache_lock.lock();
    updater_stop = true;
    updater_cond.notify_all();
    m_cache_lock.unlock();
    hot_area_updater_thread.join();
  }
  if (m_watcher.handle !=0){
    rbd_update_unwatch(m_watcher.image, m_watcher.handle);
  }
  perf_stop();
}

template <typename I>
void HotAreaCache<I>::handle_resize_image(uint64_t old_size, uint64_t new_size){
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  ldout(cct, 20) << "resize image, old:" << old_size << ", new" << new_size << dendl;
  
  // TODO: need to use lock, or error.
  if (old_size < new_size){
    m_all_areas.resize(new_size / area_conf()->size(), nullptr);
  }else if (new_size < old_size){
    uint64_t first = new_size / area_conf()->size();
    uint64_t last = old_size / area_conf()->size();
    for (uint64_t i=first; i<last; i++){
        Area* area_ptr = m_all_areas[i];
        if (area_ptr == nullptr)
          continue;
        auto slot = area_ptr->retire(cct);
        if (slot != nullptr) {
          ldout(cct, 25) << "Retireing area on resizeing: " << *area_ptr << dendl;
          m_free_slots.insert(slot);
        }
        delete area_ptr;
    }
    m_all_areas.resize(first);
    m_all_areas.shrink_to_fit();
  }
}

template <typename I>
void HotAreaCache<I>::perf_start(std::string name) {
  PerfCountersBuilder plb(m_image_ctx.cct, name, l_librbd_hac_first,
                          l_librbd_hac_last);
  plb.add_u64_counter(l_librbd_hac_area_hit, "hot_area_hit", "Num of area hits on read");
  plb.add_u64_counter(l_librbd_hac_area_miss, "hot_area_miss", "Num of area miss on read");
  plb.add_u64(l_librbd_hac_sys_hot_area_max_read, "sys_hot_area_max_read", "Max read num for system hot areas");
  plb.add_u64(l_librbd_hac_sys_hot_area_min_read, "sys_hot_area_min_read", "Min read num for system hot areas");
  plb.add_u64(l_librbd_hac_comm_hot_area_max_read, "comm_hot_area_max_read", "Max read num for common hot areas");
  plb.add_u64(l_librbd_hac_comm_hot_area_min_read, "comm_hot_area_min_read", "Min read num for common hot areas");
  plb.add_u64(l_librbd_hac_free_slot, "free_slot", "Free slot num in cache file");
  plb.add_u64(l_librbd_hac_valid_slot, "cache_slot", "Cache slog num in cache file");
  plb.add_time_avg(l_librbd_hac_update_area_avg_time, "update_hot_area_avg_time", "Time spent updating hot areas");
  m_perfcounter = plb.create_perf_counters();
  m_image_ctx.cct->get_perfcounters_collection()->add(m_perfcounter);
}

template <typename I>
void HotAreaCache<I>::perf_stop() {
  ceph_assert(m_perfcounter);
  m_image_ctx.cct->get_perfcounters_collection()->remove(m_perfcounter);
  delete m_perfcounter;
}

std::string unique_lock_name(const std::string &name, void *address) {
  return name + " (" + stringify(address) + ")";
}

} // namespace hac
} // namespace cache
} // namespace librbd

template class librbd::cache::hac::HotAreaCache<librbd::ImageCtx>;
