// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/container/flat_map.hpp>
#include <filesystem>
#include <errno.h>

#include "include/denc.h"

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/cache/hac/InitRequest.h"
#include "librbd/cache/hac/HotAreaCache.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::InitRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

template <typename I, typename H>
InitRequest<I, H>::InitRequest(Context *on_finish, H& hac,
                         I &image_ctx, plugin::Api<I> &plugin_api,
                         cache::ImageWriteThroughInterface& image_write_through) :
    m_on_finish(on_finish), m_hac(hac), m_image_ctx(image_ctx),
    m_plugin_api(plugin_api), m_image_write_through(image_write_through) {}

template <typename I, typename H>
void InitRequest<I, H>::associate_area_and_slot() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::set<uint64_t> system_area_indexes;
  for (AreaRecord record: m_area_records.system_records){
    ldout(cct, 20) << "System area record: " << "{" << record.offset << "~ " << record.count << "}" << dendl;
    Area* area = new Area(record);
    uint64_t index = area->get_index();
    m_hac.m_all_areas[index] = area;
    m_hac.m_system_recorded_areas.insert(area);
    system_area_indexes.insert(area->get_offset() >> area_conf()->size_order());
  }
  for (AreaRecord record: m_area_records.common_records){
    ldout(cct, 20) << "Common area record: " << "{" << record.offset << "~ " << record.count << "}" << dendl;
    Area* area = new Area(record);
    uint64_t index = area->get_index();
    m_hac.m_all_areas[index] = area;
    m_hac.m_common_recorded_areas.insert(area);
  }
  for (auto root_record : m_slot_root_record_vec) {
    ldout(cct, 20) << "Slot recoed: " << "{" << root_record.image_offset << "~" << root_record.pos << "}" << dendl;
    LocalCacheSlot* slot = new LocalCacheSlot(root_record, m_image_write_through, cct);
    uint64_t index = slot->get_index();
    m_hac.m_all_slots[index] = slot;
  }
  for (uint64_t i=0; i<m_hac.m_all_slots.size(); i++) {
    auto slot = m_hac.m_all_slots[i];
    if (slot == nullptr) {
      LocalCacheSlot* slot = new LocalCacheSlot(ROOT_RECORD_SIZE + i * area_conf()->size(), m_image_write_through);
      ceph_assert(slot->get_index() == i);
      m_hac.m_all_slots[i] = slot;
      m_hac.m_free_slots.insert(slot);
    } else {
      ceph_assert(slot->get_index() == i);
      uint64_t area_index = slot->get_area_index();
      m_hac.m_all_areas[area_index]->hire(slot, m_hac.m_bdev);
      m_hac.m_current_hot_areas.insert(m_hac.m_all_areas[area_index]);
    }
  }
  m_hac.update_system_area_index(system_area_indexes);
  m_hac.update_common_area_index(system_area_indexes);
}

template <typename I, typename H>
void InitRequest<I, H>::send() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  boost::container::flat_map<std::string, bufferlist> xattrs;
  boost::container::flat_map<std::string, bufferlist>* xattrs_ptr = &xattrs;
  auto ctx = new LambdaContext([this, cct, xattrs_ptr](int r){
    boost::container::flat_map<std::string, bufferlist> xattrs = *xattrs_ptr;
    ldout(cct, 20) << "Read xattr of header object:" << r <<dendl;
    bool invalid_cache = true;
    if (r<0) {
      lderr(cct) << "Read xattr of header object failed, errno:" << r <<dendl;
      this->finish(r);
      return;
    } else {
      // Check if client is closed normally. If close normally, there is
      // xattr named hac.close_client and value is 1. Cache may be valid.
      auto it = xattrs.find(HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY);
      if (it != xattrs.end()) {
        std::string close_client_value (it->second.c_str(), it->second.length());
        if (close_client_value == HEADER_OBJECT_XATTR_CLOSE_VALUE){
          // Check if cache is valid. If there is xattr named hac.valid_cache and 
          // value is 1. Cache is valid.
          it = xattrs.find(HEADER_OBJECT_XATTR_VALID_CACHE_KEY);
          if (it != xattrs.end()){
            std::string validate_cache_value (it->second.c_str(), it->second.length());
            if (validate_cache_value == HEADER_OBJECT_XATTR_VALID_CACHE_VALUE){
              invalid_cache = false;
            }
          }
        }
      }
    }
    ldout(cct, 5) << "Status of hac cache is:" << (invalid_cache? "invalid": "valid") << dendl;
    auto gater_ctx = new C_Gather(cct, new LambdaContext([this, cct, invalid_cache](int r) {
      if (r>=0) {
        associate_area_and_slot();
      } else {
        lderr(cct) << "Initialize area, load records and initialize slots failed, errno:" << r <<dendl;
        this->finish(r);
        return;
      }

      // Set xattr named hac.close_client of header object. 
      auto ctx = new LambdaContext([this, cct](int r){
        if (r<0) {
          lderr(cct) << "Set xattr named" << HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY << " of header object failed , errno is " << r << dendl;
          this->finish(r);
          return;
        }
        // Set xattr named hac.valid_cache of header object. 
        auto ctx = new LambdaContext([this, cct](int r){
          if (r<0) {
            lderr(cct) << "Set xattr named" << HEADER_OBJECT_XATTR_VALID_CACHE_KEY << " of header object failed , errno is " << r << dendl;
          }
          this->finish(r);
        });
        bufferlist bl;
        bl.append(HEADER_OBJECT_XATTR_VALID_CACHE_VALUE.c_str(), HEADER_OBJECT_XATTR_VALID_CACHE_VALUE.length());
        m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_VALID_CACHE_KEY, bl);
      });
      bufferlist bl;
      bl.append(HEADER_OBJECT_XATTR_NO_CLOSE_VALUE.c_str(), HEADER_OBJECT_XATTR_NO_CLOSE_VALUE.length());
      m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY, bl);
    }));
    init_area_and_slot_conf();
    init_area_and_load_records(gater_ctx->new_sub(), invalid_cache);
    init_local_cache_file_and_slots(gater_ctx->new_sub(), invalid_cache);
    gater_ctx->activate();
  });
  m_plugin_api.rados_xattrs_read(&m_image_ctx, ctx, m_image_ctx.header_oid, xattrs_ptr);
}

template <typename I, typename H>
void InitRequest<I, H>::init_area_and_slot_conf() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  uint64_t area_size = m_image_ctx.config.template get_val<uint64_t>("rbd_hot_area_cache_area_size");
  area_conf()->init(area_size); 
  slot_conf()->init(area_size);
  ldout(cct, 20) << "hac_area_size:" << area_conf()->size() << ", "
      "hac_area_size_order:"<< area_conf()->size_order() << "，"
      "hac_area_offset_mask:" << area_conf()->offset_mask()<< ","
      "hac_block_per_slot:" << slot_conf()->blocks_per_slot() << dendl;
}

template <typename I, typename H>
void InitRequest<I, H>::init_area_and_load_records(Context* on_finish, bool invalid_cache) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::string area_record_obj = area_record_object_name(m_image_ctx.id);
  auto image_size = m_plugin_api.get_image_size(&m_image_ctx);
  m_hac.m_all_areas.resize(image_size / area_conf()->size(), nullptr);  

  if (invalid_cache){
    auto ctx = new LambdaContext([this, on_finish, cct](int r){
      if (r<0) {
        lderr(cct) << "Invalid cache failed on initializing area." << dendl;
      }
      on_finish->complete(0);
    });
    m_plugin_api.rados_object_remove(&m_image_ctx, ctx, area_record_obj);
    return;
  }

  auto ctx = new LambdaContext([this, on_finish, area_record_obj, cct](int r){
    if (r<0) {
      // TODO
      on_finish->complete(0);
    } else {
      RecordHeader record_header;
      auto p = m_tmp_bl.cbegin();
      decode(record_header, p);
      // If current area size is different old area size, remove old rados.
      if (record_header.area_size == area_conf()->size()){
        ldout(cct, 20) << "Area records data len: " << record_header.data_len << dendl;
        m_tmp_bl.clear();
        auto ctx = new LambdaContext([this, on_finish](int r) {
          if (r<0) {
            // TODO
          } else {
            auto p = m_tmp_bl.cbegin();
            decode(m_area_records, p);
          }
          on_finish->complete(r);
        });
        m_plugin_api.rados_object_read(&m_image_ctx, ctx, area_record_obj, m_tmp_bl, RECORD_HEADER_SIZE, record_header.data_len);
      } else{
        auto ctx = new LambdaContext([this, on_finish](int r) {
          if (r<0){
            // TODO
          }
          on_finish->complete(0);
        });
        m_plugin_api.rados_object_remove(&m_image_ctx, ctx, area_record_obj);
      }
    }
  });
  m_plugin_api.rados_object_read(&m_image_ctx, ctx, area_record_obj, m_tmp_bl, 0, RECORD_HEADER_SIZE);
}

template <typename I, typename H>
void InitRequest<I, H>::init_local_cache_file_and_slots(Context* on_finish, bool invalid_cache) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::string pool_name = m_image_ctx.md_ctx.get_pool_name();
  m_cache_file_size = m_image_ctx.config.template get_val<uint64_t>(
        "rbd_hot_area_cache_size");
  m_hac.m_all_slots.resize((m_cache_file_size - ROOT_RECORD_SIZE) / area_conf()->size(), nullptr);
  std::string path = m_image_ctx.config.template get_val<std::string>(
        "rbd_hot_area_cache_path");
  std::string slot_file = local_placement_slot_file_name(path, m_image_ctx.id, pool_name);
  if (access(slot_file.c_str(), F_OK) != 0 || invalid_cache) {
    create_new_local_cache_file(on_finish, slot_file, path);
  } else {
    check_existing_local_cache_file(on_finish, slot_file, path);
  }
}

template <typename I, typename H>
void InitRequest<I, H>::create_new_local_cache_file(Context* on_finish, std::string slot_file, std::string slot_dir) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  m_hac.m_bdev = BlockDevice::create(cct, slot_file, LocalCacheSlot::aio_cache_cb, nullptr, nullptr, nullptr);
  int fd = ::open(slot_file.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644);
  // If no enough space for creating cache file, remove invalid cache files.
  if(fd < 0 && errno == ENOSPC){
    ldout(cct, 5) << "No enough space for creating cache file, remove invalid cache file and rados." << dendl;
    utime_t now = ceph_clock_now();
    for (auto& p : std::filesystem::directory_iterator(slot_dir)) {	
      std::string file_name = p.path().filename().string();
      std::string file_path = p.path().string();
      if (file_name.find("rbd-hac") != std::string::npos && file_path != slot_file){
        int r = remove_invalid_cache_file(file_path, now);
        if (r < 0){
          lderr(cct) << "failed to remove cache file: " << file_path << dendl;
          m_hac.m_bdev->close();
          delete m_hac.m_bdev;
          on_finish->complete(r);
          return;
        }
        
        int last = file_name.rfind(".") - 1;
        int first = file_name.rfind(".", last) + 1;
        std::string image_id = file_name.substr(first, last-first+1);
        std::string area_record_obj = area_record_object_name(image_id);
        auto ctx = new LambdaContext([this, area_record_obj](int r) {
          if (r<0){ 
            lderr(m_image_ctx.cct) << "failed to remove rados: " << area_record_obj << dendl;
          }
        });
        m_plugin_api.rados_object_remove(&m_image_ctx, ctx, area_record_obj);
      }
    }
    // Try again.
    fd = ::open(slot_file.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644);
  }
  if (fd >= 0) {
    int r = truncate(slot_file.c_str(), m_cache_file_size);
    if (r != 0) {
      lderr(cct) << "Failed to truncated cache block file: " << slot_file << dendl;
      ::close(fd);
      m_hac.m_bdev->close();
      delete m_hac.m_bdev;
      on_finish->complete(r);
      return;
    }
    ::close(fd);
  }else {
    lderr(cct) << "Failed to create cache block file: " << slot_file << dendl;
    m_hac.m_bdev->close();
    delete m_hac.m_bdev;
    on_finish->complete(fd);
    return;
  }
  int r = m_hac.m_bdev->open(slot_file);
  if (r < 0) {
    lderr(cct) << "failed to open bdev" << dendl;
    delete m_hac.m_bdev;
    on_finish->complete(fd);
    return;
  }
  SlotRootHeader slot_root_header{0, ceph_clock_now(), area_conf()->size(), false};
  bufferlist bl;
  encode(slot_root_header, bl);
  bl.append_zero(BLOCK_SIZE - bl.length());
  r = m_hac.m_bdev->write(0, bl, false);
  if (r < 0) {
    lderr(cct) << "Failed to write super block." << " r="  << r << dendl;
    m_hac.m_bdev->close();
    delete m_hac.m_bdev;
    on_finish->complete(-EINVAL);
    return;
  }
  on_finish->complete(0);
}

template <typename I, typename H>
int InitRequest<I, H>::remove_invalid_cache_file(std::string& slot_file, utime_t& now){
	auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  //Read slot root head in each cache file. 
	int r = m_hac.m_bdev->open(slot_file);
	if (r < 0) {
		lderr(cct) << "failed to open bdev:" << slot_file << dendl;
		return r;
	}

  bufferlist bl;
  ::IOContext ioctx(cct, nullptr);
	r = m_hac.m_bdev->read(0, BLOCK_SIZE, &bl, &ioctx, false);
	if (r < 0) {
		lderr(cct) << "read ssd cache superblock failed: " << slot_file << dendl;
		return r;
	}	

	auto p = bl.cbegin();
	SlotRootHeader slot_root_header;
	decode(slot_root_header, p);
	m_hac.m_bdev->close();
	
	// Check invalid cache files.
	if (now - slot_root_header.timestamp > 60){
		int r = remove(slot_file.c_str());
		if (r < 0 ) {
			lderr(cct) << "Failed to remove invalid cache file " << slot_file << dendl;
			return r;
		}
	}
	return r;
}

template <typename I, typename H>
void InitRequest<I, H>::check_existing_local_cache_file(
             Context* on_finish, std::string slot_file, std::string slot_dir) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  m_hac.m_bdev = BlockDevice::create(cct, slot_file, LocalCacheSlot::aio_cache_cb, nullptr, nullptr, nullptr);
  int r = m_hac.m_bdev->open(slot_file);
  if (r < 0) {
    lderr(cct) << "failed to open bdev" << dendl;
    delete m_hac.m_bdev;
    on_finish->complete(r);
    return;
  }
  bufferlist bl;
  ::IOContext ioctx(cct, nullptr);
  r = m_hac.m_bdev->read(0, BLOCK_SIZE, &bl, &ioctx, false);
  if (r < 0) {
    lderr(cct) << "read ssd cache superblock failed " << dendl;
    m_hac.m_bdev->close();
    delete m_hac.m_bdev;
    on_finish->complete(-EINVAL);
    return;
  }
  auto p = bl.cbegin();
  SlotRootHeader slot_root_header;
  decode(slot_root_header, p);
  if (!slot_root_header.shutdown) {
    m_hac.m_bdev->close();
    delete m_hac.m_bdev;
    create_new_local_cache_file(on_finish, slot_file, slot_dir);
  } else {
    auto image_mod_time = m_plugin_api.get_image_modify_timestamp(&m_image_ctx);
    ldout(cct, 20) << "Image modify timestamp: " << image_mod_time << dendl;
    if (image_mod_time>slot_root_header.timestamp || slot_root_header.slot_size != area_conf()->size()) {
      m_hac.m_bdev->close();
      delete m_hac.m_bdev;
      create_new_local_cache_file(on_finish, slot_file, slot_dir);
      return;
    } else if (slot_root_header.root_data_len>0) {
      ldout(cct, 20) << "Local cached slot record data len: " << slot_root_header.root_data_len << dendl;
      uint64_t align_len = round_up_to(slot_root_header.root_data_len, BLOCK_SIZE);
      bl.clear();
      r = m_hac.m_bdev->read(BLOCK_SIZE, align_len, &bl, &ioctx, false);
      if (r < 0) {
        lderr(cct) << "read ssd cache superblock failed " << dendl;
        m_hac.m_bdev->close();
        delete m_hac.m_bdev;
        on_finish->complete(-EINVAL);
        return;
      }
      p = bl.cbegin();
      decode(m_slot_root_record_vec, p);
    }
    on_finish->complete(0);
  }
}

template <typename I, typename H>
void InitRequest<I, H>::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd

template class librbd::cache::hac::InitRequest<librbd::ImageCtx, librbd::cache::hac::HotAreaCache<librbd::ImageCtx>>;
