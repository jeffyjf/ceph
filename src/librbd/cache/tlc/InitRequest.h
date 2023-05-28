// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_TLC_INIT_REQUEST_H
#define CEPH_LIBRBD_CACHE_TLC_INIT_REQUEST_H

#include "librbd/io/Types.h"

namespace librbd {

struct ImageCtx;

namespace plugin { template <typename> struct Api; }

namespace cache {
namespace tlc {

template <typename ImageCtxT>
class TempLocalCache;

template <typename ImageCtxT, typename TempLocalCacheT>
class InitRequest {
public:
  InitRequest(Context *on_finish, TempLocalCacheT& tlc, ImageCtxT &image_ctx,
              plugin::Api<ImageCtxT> &plugin_api);
  void send();
private:
  void finish(int r);

  int init_temp_local_data_file(bool invalid_cache);

  Context* m_on_finish;
  TempLocalCacheT& m_tlc;
  ImageCtxT &m_image_ctx;
  plugin::Api<ImageCtxT> &m_plugin_api;
};

} // namespace tlc
} // namespace cache
} // namespace librbd

extern template class librbd::cache::tlc::InitRequest<librbd::ImageCtx, librbd::cache::tlc::TempLocalCache<librbd::ImageCtx>>;

#endif // CEPH_LIBRBD_CACHE_TLC_INIT_REQUEST_H
