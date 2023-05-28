// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_PLUGIN_TEMP_LOCAL_IMAGE_CACHE_H
#define CEPH_LIBRBD_PLUGIN_TEMP_LOCAL_IMAGE_CACHE_H

#include "librbd/plugin/Types.h"
#include "include/Context.h"

namespace librbd {

struct ImageCtx;

namespace plugin {

template <typename ImageCtxT>
class TempLocalImageCache : public Interface<ImageCtxT> {
public:
  TempLocalImageCache(CephContext* cct) : Interface<ImageCtxT>(cct) {
  }

  ~TempLocalImageCache() override;

  void init(ImageCtxT* image_ctx, Api<ImageCtxT>& api,
            cache::ImageWritebackInterface& image_writeback,
            cache::ImageWriteThroughInterface& image_write_through,
            cache::ImageTempLocalWritebackInterface& image_temp_local_writeback,
            PluginHookPoints& hook_points_list,
            Context* on_finish) override;
private:
  ImageCtxT* m_image_ctx;

};

} // namespace plugin
} // namespace librbd

extern template class librbd::plugin::TempLocalImageCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_PLUGIN_TEMP_LOCAL_IMAGE_CACHE_H
