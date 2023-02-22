// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

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
  for (auto record : m_area_record_vec) {
    ldout(cct, 20) << "Area record: " << "{" << record.offset << "~ " << record.count << "}" << dendl;
    Area* area = new Area(record);
    uint64_t index = area->get_index();
    m_hac.m_all_areas[index] = area;
    m_hac.m_all_recorded_areas.insert(area);
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
      LocalCacheSlot* slot = new LocalCacheSlot(ROOT_RECORD_SIZE + i * AREA_SIZE, m_image_write_through);
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
}

template <typename I, typename H>
void InitRequest<I, H>::send() {
  // TODO need check the existing local cache slot file whether or not is valid.
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  auto gater_ctx = new C_Gather(cct, new LambdaContext([this, cct](int r) {
    if (r>=0) {
      associate_area_and_slot();
    } else {
      // TODO
    }
    this->finish(r);
  }));

  init_area_and_load_records(gater_ctx->new_sub());
  init_local_cache_file_and_slots(gater_ctx->new_sub());

  gater_ctx->activate();
}

template <typename I, typename H>
void InitRequest<I, H>::init_area_and_load_records(Context* on_finish) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::string area_record_obj = area_record_object_name(m_image_ctx.id);
  auto image_size = m_plugin_api.get_image_size(&m_image_ctx);
  m_hac.m_all_areas.resize(image_size / AREA_SIZE, nullptr);

  auto ctx = new LambdaContext([this, on_finish, area_record_obj, cct](int r){
    if (r<0) {
      // TODO
      on_finish->complete(0);
    } else {
      RecordHeader record_header;
      auto p = m_tmp_bl.cbegin();
      decode(record_header, p);
      ldout(cct, 20) << "Area records data len: " << record_header.data_len << dendl;
      m_tmp_bl.clear();
      auto ctx = new LambdaContext([this, on_finish](int r) {
        if (r<0) {
          // TODO
        } else {
          auto p = m_tmp_bl.cbegin();
          decode(m_area_record_vec, p);
        }
        on_finish->complete(r);
      });
      m_plugin_api.rados_object_read(&m_image_ctx, ctx, area_record_obj, m_tmp_bl, RECORD_HEADER_SIZE, record_header.data_len);
    }
  });
  m_plugin_api.rados_object_read(&m_image_ctx, ctx, area_record_obj, m_tmp_bl, 0, RECORD_HEADER_SIZE);
}


template <typename I, typename H>
void InitRequest<I, H>::init_local_cache_file_and_slots(Context* on_finish) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::string pool_name = m_image_ctx.md_ctx.get_pool_name();
  m_cache_file_size = m_image_ctx.config.template get_val<uint64_t>(
        "rbd_hot_area_cache_size");
  m_hac.m_all_slots.resize((m_cache_file_size - ROOT_RECORD_SIZE) / AREA_SIZE, nullptr);
  std::string path = m_image_ctx.config.template get_val<std::string>(
        "rbd_hot_area_cache_path");
  std::string slot_file = local_placement_slot_file_name(path, m_image_ctx.id, pool_name);
  if (access(slot_file.c_str(), F_OK) != 0) {
    create_new_local_cache_file(on_finish, slot_file);
  } else {
    check_existing_local_cache_file(on_finish, slot_file);
  }
}

template <typename I, typename H>
void InitRequest<I, H>::create_new_local_cache_file(Context* on_finish, std::string slot_file) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  int fd = ::open(slot_file.c_str(), O_RDWR|O_CREAT, 0644);
  if (fd >= 0) {
    int r = truncate(slot_file.c_str(), m_cache_file_size);
    if (r != 0) {
      lderr(cct) << "Failed to truncated cache block file: " << slot_file << dendl;
      ::close(fd);
      on_finish->complete(r);
      return;
    }
    ::close(fd);
  } else {
    lderr(cct) << "Failed to create cache block file: " << slot_file << dendl;
    on_finish->complete(fd);
    return;
  }
  m_hac.m_bdev = BlockDevice::create(cct, slot_file, LocalCacheSlot::aio_cache_cb, nullptr, nullptr, nullptr);
  int r = m_hac.m_bdev->open(slot_file);
  if (r < 0) {
    lderr(cct) << "failed to open bdev" << dendl;
    delete m_hac.m_bdev;
    on_finish->complete(fd);
    return;
  }
  SlotRootHeader slot_root_header{0, ceph_clock_now()};
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
void InitRequest<I, H>::check_existing_local_cache_file(Context* on_finish, std::string slot_file) {
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
    create_new_local_cache_file(on_finish, slot_file);
  } else {
    auto image_mod_time = m_plugin_api.get_image_modify_timestamp(&m_image_ctx);
    ldout(cct, 20) << "Image modify timestamp: " << image_mod_time << dendl;
    if (image_mod_time>slot_root_header.timestamp) {
      m_hac.m_bdev->close();
      delete m_hac.m_bdev;
      create_new_local_cache_file(on_finish, slot_file);
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
