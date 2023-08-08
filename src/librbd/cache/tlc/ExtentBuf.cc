// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/tlc/ExtentBuf.h"

namespace librbd {
namespace cache {
namespace tlc {

ExtentBuf::ExtentBuf(io::Extent&& extent) :
    offset(extent.first), len(extent.second) {}

io::Extent ExtentBuf::get_extent() {
  io::Extent extent{offset, len};
  return extent;
}

std::ostream& operator<<(std::ostream& os, const ExtentBuf& ext_buf) {
  os << "{" << ext_buf.offset << "~" << ext_buf.len << "}";
  return os;
}

} // namespace tlc
} // namespace cache
} // namespace librbd
