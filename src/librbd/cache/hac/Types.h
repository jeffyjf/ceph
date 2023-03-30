// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_HAC_TYPES_H
#define CEPH_LIBRBD_CACHE_HAC_TYPES_H

enum {
  l_librbd_hac_first = 777000,
  l_librbd_hac_area_hit,
  l_librbd_hac_area_miss,
  l_librbd_hac_sys_hot_area_max_read,
  l_librbd_hac_sys_hot_area_min_read,
  l_librbd_hac_comm_hot_area_max_read,
  l_librbd_hac_comm_hot_area_min_read,
  l_librbd_hac_free_slot, 
  l_librbd_hac_valid_slot, 
  l_librbd_hac_update_area_avg_time, 
  l_librbd_hac_last
};

namespace librbd {
namespace cache {
namespace hac {

const uint64_t UNTIL_RESET_AREA_READ_COUNT = 6;
const uint64_t HOT_AREA_READ_COUNT = 3;
const uint64_t NOT_HOT_AREA_READ_COUNT = 1;

const std::string HEADER_OBJECT_XATTR_VALID_CACHE_KEY = "hac.valid_cache";
const std::string HEADER_OBJECT_XATTR_VALID_CACHE_VALUE = "1";
const std::string HEADER_OBJECT_XATTR_INVALID_CACHE_VALUE = "0";

const std::string HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY = "hac.close_client";
const std::string HEADER_OBJECT_XATTR_CLOSE_VALUE = "1";
const std::string HEADER_OBJECT_XATTR_NO_CLOSE_VALUE = "0";


} // namespace hac
} // namespace cache
} // namespace librbd

#endif // CEPH_LIBRBD_CACHE_HAC_TYPES_H
