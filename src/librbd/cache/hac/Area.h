// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_AREA_H
#define CEPH_LIBRBD_CACHE_HAC_AREA_H

#include "include/denc.h"

#include "librbd/io/Types.h"
#include "include/Context.h"

#include "librbd/cache/hac/LocalCacheSlot.h"


const uint64_t RECORD_HEADER_SIZE = 64;

namespace librbd {
namespace cache {
namespace hac {

class AreaConf {
public:
  uint64_t size(){
    return m_size;  
  }
  uint64_t size_order(){
    return m_size_order;
  }
  uint64_t offset_mask(){
    return m_offset_mask;
  }
  void init(uint64_t area_size){
    m_size = area_size;
    m_size_order = std::log2(m_size);
    m_offset_mask = ~(m_size - 1);
  }
private:
  uint64_t m_size;
  uint64_t m_size_order;
  uint64_t m_offset_mask;
};

AreaConf* area_conf();

struct AreaRecord {
  uint64_t offset;
  uint64_t count;
  DENC(AreaRecord, v, p) {
    DENC_START(1, 1, p);
    denc(v.offset, p);
    denc(v.count, p);
    DENC_FINISH(p);
  }
};

struct RecordHeader {
  uint64_t data_len;
  uint64_t area_size;
  DENC(RecordHeader, v, p) {
    DENC_START(1, 1, p);
    denc(v.data_len, p);
    denc(v.area_size, p);
    DENC_FINISH(p);
  }
};

struct AreaRecords {
  std::vector<AreaRecord> system_records;
  std::vector<AreaRecord> common_records;
  DENC(AreaRecords, v, p) {
    DENC_START(1, 1, p);
    denc(v.system_records, p);
    denc(v.common_records, p);
    DENC_FINISH(p);
  }
};

class Area  {  
public:
  Area(uint64_t offset);
  Area(AreaRecord& record);
  AreaRecord get_record();
  uint64_t get_offset();
  void increase_count();
  uint64_t get_count();
  void set_count(uint64_t count);

  uint64_t get_index();

  void hire(LocalCacheSlot* cache_slot, BlockDevice *bdev);

  LocalCacheSlot* retire(CephContext* cct);
  LocalCacheSlot* get_cache_slot();


  std::atomic<bool> hot;

private:
  uint64_t offset;
  uint64_t count;
  LocalCacheSlot* m_cache_slot = nullptr;
  friend class AreaCountCompare;
  friend class AreaOffsetCompare;
  friend std::ostream& operator<<(std::ostream& os, const Area& area);
};

std::ostream& operator<<(std::ostream& os, const Area& area);

class AreaCountCompare {
public:
  bool operator()(const Area* lar,
                  const Area* rar) const {
    return lar->count > rar->count;
  }
};

class AreaOffsetCompare {
public:
  bool operator()(const Area* lar,
                  const Area* rar) const {
    return lar->offset > rar->offset;
  }
};

typedef std::multiset<Area*, AreaCountCompare> AreaCountSet;
typedef std::set<Area*, AreaOffsetCompare> AreaOffsetSet;

std::string area_record_object_name(const std::string img_id);

} // namespace hac
} // namespace cache
} // namespace librbd

WRITE_CLASS_DENC(librbd::cache::hac::AreaRecord)
WRITE_CLASS_DENC(librbd::cache::hac::RecordHeader)
WRITE_CLASS_DENC(librbd::cache::hac::AreaRecords)

#endif // CEPH_LIBRBD_CACHE_HAC_AREA_H
