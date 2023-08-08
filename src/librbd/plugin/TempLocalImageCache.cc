// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/ImageDispatcher.h"
#include "librbd/plugin/TempLocalImageCache.h"
#include "ceph_ver.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/PluginRegistry.h"
#include "librbd/ImageCtx.h"
#include "librbd/cache/TempLocalCacheImageDispatch.h"
#include "librbd/cache/tlc/TempLocalCache.h"


extern "C" {

const char *__ceph_plugin_version() {
  return CEPH_GIT_NICE_VER;
}

int __ceph_plugin_init(CephContext *cct, const std::string& type,
                       const std::string& name) {
  auto plugin_registry = cct->get_plugin_registry();
  return plugin_registry->add(
    type, name, new librbd::plugin::TempLocalImageCache<librbd::ImageCtx>(cct));
}

} // extern "C"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::plugin::TempLocalImageCache: " \
                           << this << " " << __func__ << ": "


namespace librbd {
namespace plugin {

template <typename I>
void TempLocalImageCache<I>::init(I* image_ctx, Api<I>& api,
                                  cache::ImageWritebackInterface& image_writeback,
                                  cache::ImageWriteThroughInterface& image_write_through,
                                  cache::ImageTempLocalWritebackInterface& image_temp_local_writeback,
                                  PluginHookPoints& hook_points_list,
                                  Context* on_finish) {
  bool temp_local_cache_enabled = image_ctx->config.template get_val<bool>(
    "rbd_temp_local_cache_enabled");

  if (!temp_local_cache_enabled || !image_ctx->data_ctx.is_valid()) {
    on_finish->complete(0);
    return;
  }

  m_image_ctx = image_ctx;
  auto cct = image_ctx->cct;
  ldout(cct, 5) << dendl;

  cache::TempLocalCacheImageDispatch<I>::create_and_init(image_ctx, image_temp_local_writeback, api, on_finish);
}

template <typename I>
TempLocalImageCache<I>::~TempLocalImageCache() {
}

} // namespace plugin
} // namespace librbd

template class librbd::plugin::TempLocalImageCache<librbd::ImageCtx>;
