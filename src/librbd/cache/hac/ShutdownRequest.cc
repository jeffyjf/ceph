// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/denc.h"

#include "librbd/cache/hac/ShutdownRequest.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::ShutdownRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

ShutdownRequest::ShutdownRequest(
    Context *on_finish, BlockDevice* bdev,
    std::vector<LocalCacheSlot*>& all_slots,
    CephContext* cct) :
  m_on_finish(on_finish), m_bdev(bdev),
  m_all_slots(all_slots), m_cct(cct) {}

void ShutdownRequest::send() {
  ldout(m_cct, 20) << dendl;
  for (auto slot : m_all_slots) {
    if (slot != nullptr && (!slot->m_is_free.load())) {
      m_slot_records.push_back(slot->get_root_record(m_cct));
    }
  }
  bufferlist record_bl;
  encode(m_slot_records, record_bl);
  ldout(m_cct, 20) << "Slot records data len: " << record_bl.length() << dendl;
  uint64_t align_len = round_up_to(record_bl.length(), BLOCK_SIZE);
  record_bl.append_zero(align_len - record_bl.length());
  SlotRootHeader header{record_bl.length(), ceph_clock_now(), true};
  ldout(m_cct, 20) << "Local cache slot timestamp: " << header.timestamp << dendl;
  bufferlist header_bl;
  encode(header, header_bl);
  header_bl.append_zero(BLOCK_SIZE - header_bl.length());
  bufferlist total_bl;
  total_bl.claim_append(header_bl);
  total_bl.claim_append(record_bl);
  int r = m_bdev->write(0, total_bl, false);
  finish(r);
}

void ShutdownRequest::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd
