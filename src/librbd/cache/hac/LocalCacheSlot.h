// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_LOCAL_CACHE_SLOT_H
#define CEPH_LIBRBD_CACHE_HAC_LOCAL_CACHE_SLOT_H

#include "common/RWLock.h"
#include "include/utime.h"
#include "include/denc.h"
#include "blk/BlockDevice.h"
#include "librbd/io/Types.h"
#include "include/Context.h"
#include "librbd/cache/hac/ExtentBuf.h"
#include "librbd/cache/ImageWriteThrough.h"

#define BLOCK_SIZE_ORDER 12

namespace librbd {
namespace cache {
namespace hac {


const uint64_t ROOT_RECORD_SIZE = 1ULL << 24;
const uint64_t BLOCK_SIZE = 1ULL << BLOCK_SIZE_ORDER;

class ReadResult : public Context {
public:
  ReadResult(ExtentBufPtr extent_buf, io::Extent slot_extent, Context *on_finish,  CephContext* cct) :
    m_origin_extent(extent_buf), m_on_finish(on_finish), m_cct(cct), m_slot_extent(slot_extent) {}
  void add_extent_to_hit(io::Extent&& extent);
  void add_extent_to_miss(io::Extent&& extent);
  ExtentBufVec m_current_hit_extents;
  ExtentBufVec m_current_miss_extents;
  ExtentBufVec m_all_extents;
private:
  void finish(int r);
  ExtentBufPtr m_origin_extent;
  Context *m_on_finish;
  CephContext* m_cct;
  io::Extent m_slot_extent;
};

struct SlotRootHeader {
  uint64_t root_data_len;
  utime_t timestamp;
  bool shutdown;
  DENC(SlotRootHeader, v, p) {
    DENC_START(1, 1, p);
    denc(v.root_data_len, p);
    denc(v.timestamp, p);
    denc(v.shutdown, p);
    DENC_FINISH(p);
  }
};

struct SlotRootRecord {
  uint64_t pos;
  uint64_t image_offset;
  std::vector<uint8_t> block_flags;
  DENC(SlotRootRecord, v, p) {
    DENC_START(1, 1, p);
    denc(v.pos, p);
    denc(v.image_offset, p);
    denc(v.block_flags, p);
    DENC_FINISH(p);
  }
  friend std::ostream& operator<<(std::ostream& os, const SlotRootRecord& record);
};

std::ostream& operator<<(std::ostream& os, const SlotRootRecord& record);

class LocalCacheSlot {
public:
  LocalCacheSlot(uint64_t pos, cache::ImageWriteThroughInterface& image_write_through);
  LocalCacheSlot(SlotRootRecord& root_record, cache::ImageWriteThroughInterface& image_write_through, CephContext* cct);
  void read(
      ExtentBufPtr image_extent_buf, uint64_t tid, CephContext* cct, Context *on_finish);
  void write(
      ExtentBufPtr image_extent_buf, uint64_t tid, CephContext* cct, Context *on_finish);

  uint64_t get_index();
  uint64_t get_area_index();
  io::Extent get_slot_extent(ExtentBufPtr image_extent_buf);

  SlotRootRecord get_root_record(CephContext* cct);
  void set_bdev(BlockDevice* bdev);
  void set_image_offset(uint64_t image_offset);
  void reset_block_flags();
  void reset(CephContext* cct);

  static void aio_cache_cb(void *priv, void *priv2) {
    AioTransContext *c = static_cast<AioTransContext*>(priv2);
    c->aio_finish();
  }

  std::atomic<bool> m_is_free;

private:

  uint64_t m_pos;
  uint64_t m_image_offset;
  BlockDevice* m_bdev = nullptr;
  std::vector<bool> m_block_flags;
  cache::ImageWriteThroughInterface &m_image_write_through;
  RWLock m_slot_lock;

  void update_slot_cached_block(ExtentBufPtr extent_buf, CephContext* cct, Context *on_finish);

  class AioTransContext {
  public:
    Context* on_finish;
    ::IOContext ioc;
    explicit AioTransContext(CephContext* cct, Context *cb)
      : on_finish(cb), ioc(cct, this) {}

    ~AioTransContext(){}

    void aio_finish() {
      on_finish->complete(ioc.get_return_value());
      delete this;
    }
  }; //class AioTransContext

  friend class LocalCacheSlotCompare;
  friend std::ostream& operator<<(std::ostream& os, const LocalCacheSlot& slot);
};

class LocalCacheSlotCompare {
  public:
    bool operator()(const LocalCacheSlot* lcs,
                    const LocalCacheSlot* rcs) const {
      return lcs->m_pos < rcs->m_pos;
    }
};

std::ostream& operator<<(std::ostream& os, const LocalCacheSlot& slot);

typedef std::set<LocalCacheSlot*, LocalCacheSlotCompare> CacheSlotSet;

std::string local_placement_slot_file_name(const std::string path, const std::string img_id, const std::string pool_name);

} // namespace hac
} // namespace cache
} // namespace librbd


WRITE_CLASS_DENC(librbd::cache::hac::SlotRootHeader)
WRITE_CLASS_DENC(librbd::cache::hac::SlotRootRecord)

#endif // CEPH_LIBRBD_CACHE_HAC_LOCAL_CACHE_SLOT_H
