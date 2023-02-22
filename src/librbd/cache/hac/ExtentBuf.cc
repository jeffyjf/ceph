// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/hac/ExtentBuf.h"

namespace librbd {
namespace cache {
namespace hac {

ExtentBuf::ExtentBuf(io::Extent&& extent) :
    offset(extent.first), len(extent.second) {}

io::Extent ExtentBuf::get_extent() {
  io::Extent extent{offset, len};
  return extent;
}

bool ExtentBuf::try_merge(io::Extent& ext) {
  uint64_t current_end = offset + len;
  uint64_t try_merge_offset = ext.first;
  uint64_t try_mege_end = ext.first + ext.second;

  if (try_mege_end < offset || current_end < try_merge_offset) {
    return false;
  }

  offset = std::min(offset, try_merge_offset);
  len = std::max(try_mege_end-offset, current_end-offset);
  return true;
}

std::ostream& operator<<(std::ostream& os, const ExtentBuf& ext_buf) {
  os << "{" << ext_buf.offset << "~" << ext_buf.len << "}";
  return os;
}

} // namespace hac
} // namespace cache
} // namespace librbd
