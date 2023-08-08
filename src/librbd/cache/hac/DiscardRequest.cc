// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/hac/DiscardRequest.h"

namespace librbd {
namespace cache {
namespace hac {

void DiscardRequest::push_to_update_hot_area(ExtentBufPtr extent_buf, Area* area) {
  update_hot_area.push_back(extent_buf);
  areas_to_write.push_back(area);
}

void DiscardRequest::send() {
  for (uint64_t i=0; i<areas_to_write.size(); i++) {
    Area* area = areas_to_write[i];
    auto slot = area->get_cache_slot();
    slot->discard(update_hot_area[i], m_cct);
  }
  delete this;
}

} // namespace hac
} // namespace cache
} // namespace librbd
