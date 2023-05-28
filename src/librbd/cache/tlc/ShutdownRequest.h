// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_SHUTDOWN_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_SHUTDOWN_REQUEST_H

#include "librbd/plugin/Api.h"
#include "librbd/io/Types.h"
#include "blk/BlockDevice.h"

namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace tlc {

template <typename ImageCtxT>
class ShutdownRequest {
public:
  ShutdownRequest(Context *on_finish, plugin::Api<ImageCtxT>& plugin_api, 
                  std::vector<std::map<uint64_t, uint64_t>> &cache_md, 
                  ImageCtxT &image_ctx, uint64_t cached_bytes, std::string metadata_file_path);
  void send();
private:

  void finish(int r);

  Context* m_on_finish;
  plugin::Api<ImageCtxT>& m_plugin_api;
  std::vector<std::map<uint64_t, uint64_t>> &m_cache_md;
  ImageCtxT& m_image_ctx;
  uint64_t m_cached_bytes;
  std::string m_metadata_file_path;
};

} // namespace tlc
} // namespace cache
} // namespace librbd

extern template class librbd::cache::tlc::ShutdownRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_TLC_SHUTDOWN_REQUEST_H
