// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/io/Types.h"

#include "librbd/cache/tlc/TempLocalCache.h"
#include "librbd/cache/tlc/Types.h"
#include "librbd/cache/tlc/InitRequest.h"
#include "librbd/cache/tlc/DiscardRequest.h"
#include "librbd/cache/tlc/ReadRequest.h"
#include "librbd/cache/tlc/ShutdownRequest.h"
#include "librbd/cache/tlc/WriteRequest.h"
#include "librbd/cache/tlc/WriteSameRequest.h"


#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::tlc::TempLocalCache: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace tlc {

template <typename I>
TempLocalCache<I>::TempLocalCache(
    I &image_ctx, cache::ImageTempLocalWritebackInterface& image_temp_local_writeback, plugin::Api<I>& plugin_api)
  : m_invalidate_cache_thread(this),
    m_cache_lock(ceph::make_mutex(unique_lock_name("librbd::cache::tlc::TempLocalCache::m_cache_lock", this))),
    m_image_ctx(image_ctx),
    m_plugin_api(plugin_api), 
    m_image_temp_local_writeback(image_temp_local_writeback),
    m_thread_pool(image_ctx.cct, "librbd::cache::tlc::TempLocalCache::thread_pool",
	                "tp_tlc", 1, ""),
    m_work_queue("librbd::cache::tlc::TempLocalCache::work_queue",
                  ceph::make_timespan(60), &m_thread_pool),
    m_watcher(this, static_cast<rbd_image_t>(&image_ctx), 
              static_cast<uint64_t>(m_plugin_api.get_image_size(&m_image_ctx)))
    {}

template <typename I>
void TempLocalCache<I>::init(Context *on_finish) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  auto ctx = new LambdaContext([this, on_finish](int r){
    if (r>=0) {
      start();
    }
    on_finish->complete(r);
  });
  auto init_req = new InitRequest<I, This>(ctx, *this, m_image_ctx, m_plugin_api);
  init_req->send();
}

template <typename I>
void TempLocalCache<I>::shut_down(Context *on_finish) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  stop();
  auto shut_down_ctx = new LambdaContext([this, on_finish, cct](int r) {
    if (r<0) {
      // TODO
    }
    on_finish->complete(r);
    m_thread_pool.stop();
  });
  auto shut_down_req = new ShutdownRequest(shut_down_ctx, m_plugin_api, 
    m_cache_md_vec, m_image_ctx, m_cached_bytes, m_metadata_file_path);
  shut_down_req->send();
}

template <typename I>
void TempLocalCache<I>::read(
      io::Extents&& image_extents, ceph::bufferlist *bl,
      int fadvise_flags, Context *on_finish, uint64_t tid){

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "tid:" << tid << dendl;
  
  ReadRequest *read_ctx = new ReadRequest(cct, bl, m_image_temp_local_writeback, on_finish, tid, m_bdev);

  for (auto &extent : image_extents){
    uint64_t image_start = extent.first;
    uint64_t image_end = extent.first + extent.second;

    {
      std::lock_guard lock(m_cache_lock);

      uint64_t first_map_index = image_start>>MAP_SIZE_ORDER;
      uint64_t last_map_index = image_end>>MAP_SIZE_ORDER;
      uint64_t offset = image_start;

      for(uint64_t i=first_map_index; i<=last_map_index;i++){
        std::map<uint64_t, uint64_t> &cache_md_map = m_cache_md_vec[i];
        if(cache_md_map.empty()){
          continue;
        }
        auto it = cache_md_map.lower_bound(offset);
        if(it!=cache_md_map.begin()){
          it--;
        }

        for(;it!=cache_md_map.end();it++){
          uint64_t cache_start = it->first;
          uint64_t cache_end = cache_start+it->second;

          if(offset<cache_start&&image_end>cache_start){
            if(image_end<=cache_end){
              auto miss = std::make_shared<ExtentBuf>(io::Extent{offset, cache_start-offset});
              ldout(cct, 20) << "read--------miss1 tid:" << tid  <<","<< offset << "~"<<  (cache_start-offset) << dendl;
              read_ctx->push_to_read_from_rados(miss);
              auto hit = std::make_shared<ExtentBuf>(io::Extent{cache_start, image_end-cache_start});
              ldout(cct, 20) << "read--------hit2 tid:" << tid  <<","<< cache_start << "~"<<  (image_end-cache_start) << dendl;
              read_ctx->push_to_read_from_cache(hit);
              offset = image_end;
              break;
            }else{
              auto miss = std::make_shared<ExtentBuf>(io::Extent{offset, cache_start-offset});
              ldout(cct, 20) << "read--------miss5 tid:" << tid  <<","<< offset << "~"<< (cache_start-offset) << dendl;
              read_ctx->push_to_read_from_rados(miss);
              auto hit = std::make_shared<ExtentBuf>(io::Extent{cache_start, cache_end-cache_start});
              ldout(cct, 20) << "read--------hit6 tid:" << tid  <<","<< cache_start << "~"<<  (cache_end-cache_start) << dendl;
              read_ctx->push_to_read_from_cache(hit);
              offset = cache_end;
            }
          }else if(offset>=cache_start && offset<cache_end){
            if(image_end<=cache_end){
              auto hit = std::make_shared<ExtentBuf>(io::Extent{offset, image_end-offset});
              ldout(cct, 20) << "read--------hit3 tid:" << tid  <<","<< offset << "~"<<  (image_end-offset) << dendl;
              read_ctx->push_to_read_from_cache(hit);
              offset = image_end;
              break;
            }else{
              auto hit = std::make_shared<ExtentBuf>(io::Extent{offset, cache_end-offset});
              ldout(cct, 20) << "read--------hit4 tid:" << tid  <<","<< offset << "~"<<  (cache_end-offset) << dendl;
              read_ctx->push_to_read_from_cache(hit);
              offset = cache_end;
            }
          }
        }
      }
      if (offset < image_end){
        //整个加入miss
        auto miss_buf = std::make_shared<ExtentBuf>(io::Extent{offset, image_end-offset});
        ldout(cct, 20) << "read---------miss7:"<< tid <<":" << offset << "~" << image_end-offset  << dendl;
        read_ctx->push_to_read_from_rados(miss_buf);
      }
    }
  }
  read_ctx->send();  
}

template <typename I>
void TempLocalCache<I>::write(
      io::Extents&& image_extents, ceph::bufferlist&& bl,
      int fadvise_flags, Context *on_finish, uint64_t tid) {

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "tid=" << tid << dendl;

  auto ctx = new LambdaContext([this, image_extents, on_finish](int r){
    {
      std::lock_guard lock(m_cache_lock);
      for (auto &image_extent: image_extents){
        uint64_t image_start = image_extent.first;
        uint64_t image_end = image_start+image_extent.second;
        
        uint64_t first_map_index = image_start>>MAP_SIZE_ORDER ; 
        uint64_t last_map_index = image_end>>MAP_SIZE_ORDER;

        //初始化 image 在这个map里的边界
        uint64_t bound_start;
        uint64_t  bound_end;

        for(uint64_t i=first_map_index; i <=last_map_index; i++ ){
          if (i==first_map_index){
            bound_start  = image_start;
          }else{
            bound_start = (first_map_index+i)<<MAP_SIZE_ORDER;
          }
 
          if(i==last_map_index){
            bound_end = std::min((first_map_index+i+1)<<MAP_SIZE_ORDER,   image_end);
          }else{
            bound_end =  (first_map_index+i+1)<<MAP_SIZE_ORDER;
          }
  
          std::map<uint64_t, uint64_t> &cache_md_map = m_cache_md_vec[i];
          auto left_it = cache_md_map.lower_bound(bound_start);
          if (left_it==cache_md_map.end()){
            if(cache_md_map.empty()){
              //insert
              cache_md_map[bound_start] = bound_end-bound_start;
              continue;
            }else{
              left_it--;
              if(left_it->first <= bound_end&& left_it->first+ left_it->second>= bound_start){
                bound_start=left_it->first;
              }
              //insert
              cache_md_map[bound_start] = bound_end-bound_start;
              continue;
            }
          }else{
            if (left_it!=cache_md_map.begin()){
              left_it--;
              if(left_it->first <= bound_end && left_it->first+ left_it->second>= bound_start){
                bound_start=left_it->first;
              }else{
                left_it++;
                if(left_it->first <= bound_end && left_it->first+ left_it->second>= bound_start){
             
                }else{
                  //insert
                  cache_md_map[bound_start]=bound_end-bound_start;
	                continue;
                }
              }
            }else{
              if(left_it->first <= bound_end && left_it->first+left_it->second>= bound_start){
    
              }else{
                //insert
                cache_md_map[bound_start]=bound_end-bound_start;	
                continue;
              }
            }
          }

          auto right_it =  cache_md_map.upper_bound(bound_end);
          right_it--;
          if(right_it->first+right_it->second>bound_end){
            bound_end = right_it->first + right_it->second;
          }
          cache_md_map.erase(left_it, ++right_it);
          //insert
          cache_md_map[bound_start]=bound_end-bound_start;
        }
      }
    }
    on_finish->complete(r);
  });
  WriteRequest *write_ctx = new WriteRequest(cct, ctx, tid, image_extents, std::move(bl), m_bdev);
  write_ctx->send();
}

template <typename I>
void TempLocalCache<I>::discard(uint64_t offset, uint64_t length,
      uint32_t discard_granularity_bytes, Context *on_finish) {
        
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  auto buf = std::make_shared<ExtentBuf>(io::Extent{offset, length});
  DiscardRequest *discard_ctx = new DiscardRequest(cct, on_finish, 
    m_cache_file_path, buf);  
  discard_ctx->send();
}

template <typename I>
void TempLocalCache<I>::writesame(uint64_t offset, uint64_t length,
                                  bufferlist&& bl, int fadvise_flags,
                                  Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;

  size_t data_len = bl.length();
  uint64_t data_off = length % data_len;
  bufferlist write_bl;
  assemble_write_same_extent(length, std::move(bl), &write_bl, data_off);
  
  io::Extents extents = {{offset, length}};
  WriteSameRequest *ws_ctx = new WriteSameRequest(
    cct, on_finish, m_ws_tid++, extents, std::move(write_bl), m_bdev);
  ws_ctx->send();
}

template <typename I>
void TempLocalCache<I>::assemble_write_same_extent(
  uint64_t length, const ceph::bufferlist& data,
  ceph::bufferlist *ws_data, uint64_t data_off) {
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
}

template <typename I>
void TempLocalCache<I>::compare_and_write(
      io::Extents&& image_extents,
      ceph::bufferlist&& cmp_bl, ceph::bufferlist&& bl,
      uint64_t *mismatch_offset,int fadvise_flags,
      Context *on_finish){

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  bufferlist read_bl;
  auto read_complete_ctx = new LambdaContext([this, read_bl_ptr=&read_bl,
    image_extents_ptr=&image_extents, cmp_bl, write_bl_ptr=&bl, 
    mismatch_offset, fadvise_flags, on_finish](int r){
    if(r<0){
      on_finish->complete(r);
      return;
    }

    /* Compare read_bl to cmp_bl to determine if this will produce a write */
    bufferlist aligned_read_bl;
    if (cmp_bl.length() < read_bl_ptr->length()) {
      aligned_read_bl.substr_of(*read_bl_ptr, 0, cmp_bl.length());
    }
    if (cmp_bl.contents_equal(*read_bl_ptr) ||
      cmp_bl.contents_equal(aligned_read_bl)) {
      /* Compare phase succeeds. Begin write */
      ldout(m_image_ctx.cct, 5) << " compare matched" << dendl;
      (*mismatch_offset) = 0;

      write(std::move(*image_extents_ptr), std::move(*write_bl_ptr), 
        fadvise_flags, on_finish, 0);
    } else {
      /* Compare phase fails. Comp-and write ends now. */
      ldout(m_image_ctx.cct, 15) << " compare failed" << dendl;
      /* Bufferlist doesn't tell us where they differed, so we'll have to determine that here */
      uint64_t bl_index = 0;
      for (bl_index = 0; bl_index < cmp_bl.length(); bl_index++) {
        if (cmp_bl[bl_index] != (*read_bl_ptr)[bl_index]) {
          ldout(m_image_ctx.cct, 15) << " mismatch at " << bl_index << dendl;
          break;
        }
      }
      (*mismatch_offset) = bl_index;
      on_finish->complete(0);
    }
  });
  io::Extents image_extents_copy = image_extents;
  read(std::move(image_extents_copy), &read_bl, fadvise_flags, read_complete_ctx, 0);
}

template <typename I>
void TempLocalCache<I>::invalidate(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  
  on_finish->complete(0);
}

template <typename I>
void TempLocalCache<I>::start() {
  rbd_update_watch(m_watcher.image, &m_watcher.handle, Watcher::cb, &m_watcher);
  m_invalidate_cache_thread.create("cache_invalid");
}

template <typename I>
void TempLocalCache<I>::stop() {  
  if(m_invalidate_cache_thread.is_started()){
    m_cache_lock.lock();
    invalidater_stop = true;
    invalidater_cond.notify_all();
    m_cache_lock.unlock();
    m_invalidate_cache_thread.join();
  }
  if (m_watcher.handle>0 and m_watcher.handle!=UINT64_MAX){
    rbd_update_unwatch(m_watcher.image, m_watcher.handle);
  }
}

template <typename I>
void TempLocalCache<I>::invalidate_part_cache() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::unique_lock l{m_cache_lock};
  while(!invalidater_stop){
    invalidater_cond.wait_for(l, std::chrono::duration<int>(50));
    // int fd = ::open(m_cache_file_path.c_str(), O_RDWR, 0644);
    // ::fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE, 8192, 53687091200);
    // ::close(fd);
  }
}

template <typename I>
void TempLocalCache<I>::handle_resize_image(uint64_t old_size, uint64_t new_size){
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "resize image, old:" << old_size << ", new" << new_size << dendl;
  if (old_size < new_size){
    std::lock_guard lock(m_cache_lock);
    m_cache_md_vec.resize(m_image_ctx.size/MAP_SIZE + 1);
  }else if (new_size < old_size){
    lderr(cct) << "tlc cache not support resize smaller:" << old_size << "->" << new_size << dendl;
  }
}

} // namespace tlc
} // namespace cache
} // namespace librbd

template class librbd::cache::tlc::TempLocalCache<librbd::ImageCtx>;
