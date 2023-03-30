// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/denc.h"

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/cache/hac/Area.h"
#include "librbd/cache/hac/ShutdownRequest.h"
#include "librbd/cache/hac/Types.h"


#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::ShutdownRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {

template <typename I>
ShutdownRequest<I>::ShutdownRequest(
    Context *on_finish, plugin::Api<I>& plugin_api, BlockDevice* bdev,
    std::vector<LocalCacheSlot*>& all_slots, I& image_ctx) :
  m_on_finish(on_finish), m_plugin_api(plugin_api), m_bdev(bdev),
  m_all_slots(all_slots), m_image_ctx(image_ctx) {}

template <typename I>
void ShutdownRequest<I>::send() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  for (auto slot : m_all_slots) {
    if (slot != nullptr && (!slot->m_is_free.load())) {
      m_slot_records.push_back(slot->get_root_record(cct));
    }
  }
  bufferlist record_bl;
  encode(m_slot_records, record_bl);
  ldout(cct, 20) << "Slot records data len: " << record_bl.length() << dendl;
  uint64_t align_len = round_up_to(record_bl.length(), BLOCK_SIZE);
  record_bl.append_zero(align_len - record_bl.length());
  SlotRootHeader header{record_bl.length(), ceph_clock_now(), area_conf()->size(),true};
  ldout(cct, 20) << "Local cache slot timestamp: " << header.timestamp << dendl;
  bufferlist header_bl;
  encode(header, header_bl);
  header_bl.append_zero(BLOCK_SIZE - header_bl.length());
  bufferlist total_bl;
  total_bl.claim_append(header_bl);
  total_bl.claim_append(record_bl);
  int r = m_bdev->write(0, total_bl, false);
  if (r < 0){
    finish(r);
    return;
  }
  
  // Setting xattr named hac.close_client indicates that cient shutdown normally.
  auto ctx = new LambdaContext([this](int r){
    if (r<0) {
      lderr(m_image_ctx.cct) << "Set xattr named" << HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY << " of header object failed for closing, errno is " << r << dendl;
    }
    this->finish(r);
  });
  bufferlist bl;
  bl.append(HEADER_OBJECT_XATTR_CLOSE_VALUE.c_str(), HEADER_OBJECT_XATTR_CLOSE_VALUE.length());
  // encode(HEADER_OBJECT_XATTR_CLOSE_VALUE, bl);
  m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY, bl);
}

template <typename I>
void ShutdownRequest<I>::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd

template class librbd::cache::hac::ShutdownRequest<librbd::ImageCtx>;
