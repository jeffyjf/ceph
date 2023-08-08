// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "blk/BlockDevice.h"
#include "include/Context.h"
#include "include/denc.h"

#ifndef CEPH_LIBRBD_CACHE_TLC_TYPES_H
#define CEPH_LIBRBD_CACHE_TLC_TYPES_H

enum {
  l_librbd_tlc_first = 999000,
  l_librbd_tlc_hit_bytes,
  l_librbd_tlc_miss_bytes,
  l_librbd_tlc_write_avg_time,
  l_librbd_tlc_last
};

namespace librbd {
namespace cache {
namespace tlc {

const std::string HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY = "tlc.close_client";
const std::string HEADER_OBJECT_XATTR_CLOSE_VALUE = "1";
const std::string HEADER_OBJECT_XATTR_NO_CLOSE_VALUE = "0";

const std::string HEADER_OBJECT_XATTR_VALID_CACHE_KEY = "tlc.valid_cache";
const std::string HEADER_OBJECT_XATTR_VALID_CACHE_VALUE = "1";
const std::string HEADER_OBJECT_XATTR_INVALID_CACHE_VALUE = "0";

const uint64_t MAP_SIZE = 16*1024*1024;
const uint64_t MAP_SIZE_ORDER = std::log2(MAP_SIZE);

// const uint64_t MAP_SIZE_MASK =  ~(MAP_SIZE - 1);

const uint64_t VECTOR_SIZE = 8*1024;
const uint64_t VECTOR_SIZE_MASK =  ~(VECTOR_SIZE - 1);
const uint64_t VECTOR_SIZE_ORDER = std::log2(VECTOR_SIZE);

const uint64_t BLOCK_SIZE = 512;

class AioTransContext {
public:
  Context* on_finish;
  ::IOContext ioc;

  explicit AioTransContext(CephContext* cct, Context *cb)
    :on_finish(cb), ioc(cct, this) {}

  ~AioTransContext(){}

  void aio_finish() {
    on_finish->complete(ioc.get_return_value());
    delete this;
  }

  static void aio_cache_cb(void *priv, void *priv2) {
    AioTransContext *c = static_cast<AioTransContext*>(priv2);
    c->aio_finish();
  }
}; //class AioTransContext

struct TempLocalCacheMetadataHeader {
  uint64_t length;
  DENC(TempLocalCacheMetadataHeader, v, p) {
    DENC_START(1, 1, p);
    denc(v.length, p);
    DENC_FINISH(p);
  }
};

struct TempLocalCacheMetadata {
  utime_t timestamp;
  uint64_t cached_bytes;
  std::vector<std::map<uint64_t, uint64_t>> cache_md;
  DENC(TempLocalCacheMetadata, v, p) {
    DENC_START(1, 1, p);
    denc(v.timestamp, p);
    denc(v.cached_bytes, p);
    denc(v.cache_md, p);
    DENC_FINISH(p);
  }
};

std::string unique_lock_name(const std::string &name, void *address);

} // namespace tlc
} // namespace cache
} // namespace librbd

WRITE_CLASS_DENC(librbd::cache::tlc::TempLocalCacheMetadataHeader)
WRITE_CLASS_DENC(librbd::cache::tlc::TempLocalCacheMetadata)

#endif // CEPH_LIBRBD_CACHE_TLC_TYPES_H
