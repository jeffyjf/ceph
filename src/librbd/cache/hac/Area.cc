// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/hac/Area.h"

namespace librbd {
namespace cache {
namespace hac {

Area::Area(uint64_t offset) :
  hot(false), offset(offset), count(1) {}

Area::Area(AreaRecord& record) :
  hot(false), offset(record.offset), count(record.count) {}

AreaRecord Area::get_record() {
  AreaRecord record{offset, count};
  return record;
}

void Area::increase_count() {
  this->count++;
}

uint64_t Area::get_index() {
  return offset >> AREA_SIZE_ORDER;
}

void Area::hire(LocalCacheSlot* cache_slot, BlockDevice *bdev) {
  ceph_assert(cache_slot != nullptr);
  ceph_assert(bdev != nullptr);
  this->m_cache_slot = cache_slot;
  this->m_cache_slot->set_image_offset(offset);
  this->m_cache_slot->set_bdev(bdev);
  this->m_cache_slot->m_is_free.store(false);
  hot.store(true);
}

LocalCacheSlot* Area::get_cache_slot() {
  return this->m_cache_slot;
}

LocalCacheSlot* Area::retire(CephContext* cct) {
  hot.store(false);
  if (m_cache_slot != nullptr) {
    m_cache_slot->reset(cct);
  }
  return m_cache_slot;
}

std::ostream& operator<<(std::ostream& os, const Area& area) {
  os << "{" << "offset: " << area.offset << ", " << "count: " << area.count << "}";
  return os;
}

std::string area_record_object_name(const std::string img_id) {
  return "hot_area_cache_record." + img_id;
}

} // namespace hac
} // namespace cache
} // namespace librbd
