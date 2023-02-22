// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H
#define CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H

#include "librbd/io/Types.h"
#include "librbd/cache/hac/LocalCacheSlot.h"

namespace librbd {
namespace cache {
namespace hac {

class ShutdownRequest {
public:
  ShutdownRequest(Context *on_finish, BlockDevice* bdev, std::vector<LocalCacheSlot*>& all_slots, CephContext* cct);
  void send();
private:

  void finish(int r);

  Context* m_on_finish;
  BlockDevice* m_bdev;
  std::vector<LocalCacheSlot*>& m_all_slots;
  std::vector<SlotRootRecord> m_slot_records;
  CephContext* m_cct;
};



} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_SHUTDOWN_REQUEST_H
