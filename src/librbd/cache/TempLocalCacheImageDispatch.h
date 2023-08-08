// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_TEMP_LOCAL_CACHE_IMAGE_DISPATCH_H
#define CEPH_LIBRBD_TEMP_LOCAL_CACHE_IMAGE_DISPATCH_H

#include "librbd/io/ImageDispatchInterface.h"
#include "include/int_types.h"
#include "include/buffer.h"
#include "common/zipkin_trace.h"
#include "librbd/io/ReadResult.h"
#include "librbd/io/Types.h"
#include "librbd/plugin/Api.h"
#include "librbd/cache/ImageWriteThrough.h"
#include "librbd/cache/tlc/TempLocalCache.h"

struct Context;

namespace librbd {

struct ImageCtx;

namespace cache {

template <typename ImageCtxT>
class TempLocalCacheImageDispatch : public io::ImageDispatchInterface {
public:
  io::ImageDispatchLayer get_dispatch_layer() const override {
    return io::IMAGE_DISPATCH_LAYER_TEMP_LOCAL_CACHE;
  }

  static void create_and_init(ImageCtxT* image_ctx,  cache::ImageTempLocalWritebackInterface& image_write_back, plugin::Api<ImageCtxT>& plugin_api, Context* on_finish) {
    if (image_ctx->read_only) {
      on_finish->complete(0);
      return;
    }

    auto dispatch = new TempLocalCacheImageDispatch<ImageCtxT>(image_ctx, image_write_back, plugin_api);
    dispatch->m_image_ctx->io_image_dispatcher->register_dispatch(dispatch);
    dispatch->m_image_cache->init(on_finish);

  }

  void shut_down(Context* on_finish) override;

  bool read(
      io::AioCompletion* aio_comp, io::Extents &&image_extents,
      io::ReadResult &&read_result, IOContext io_context,
      int op_flags, int read_flags,
      const ZTracer::Trace &parent_trace, uint64_t tid,
      std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result, Context** on_finish,
      Context* on_dispatched) override;
    bool write(
      io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&bl,
      IOContext io_context, int op_flags, const ZTracer::Trace &parent_trace,
      uint64_t tid, std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result,
      Context** on_finish, Context* on_dispatched) override;
  bool discard(
      io::AioCompletion* aio_comp, io::Extents &&image_extents,
      uint32_t discard_granularity_bytes, IOContext io_context,
      const ZTracer::Trace &parent_trace, uint64_t tid,
      std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result,
      Context** on_finish, Context* on_dispatched) override;
  bool write_same(
      io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&bl,
      IOContext io_context, int op_flags, const ZTracer::Trace &parent_trace,
      uint64_t tid, std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result,
      Context** on_finish, Context* on_dispatched) override;
  bool compare_and_write(
      io::AioCompletion* aio_comp, io::Extents &&image_extents,
      bufferlist &&cmp_bl,
      bufferlist &&bl, uint64_t *mismatch_offset,
      IOContext io_context, int op_flags,
      const ZTracer::Trace &parent_trace, uint64_t tid,
      std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result,
      Context** on_finish, Context* on_dispatched) override;
  bool flush(
      io::AioCompletion* aio_comp, io::FlushSource flush_source,
      const ZTracer::Trace &parent_trace, uint64_t tid,
      std::atomic<uint32_t>* image_dispatch_flags,
      io::DispatchResult* dispatch_result,
      Context** on_finish, Context* on_dispatched) override;
  bool list_snaps(
    io::AioCompletion* aio_comp, io::Extents&& image_extents,
    io::SnapIds&& snap_ids, int list_snaps_flags,
    io::SnapshotDelta* snapshot_delta,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context** on_finish,
    Context* on_dispatched) override;

  bool invalidate_cache(Context* on_finish) override;

private:

  TempLocalCacheImageDispatch(ImageCtxT* image_ctx,
                            cache::ImageTempLocalWritebackInterface& image_temp_local_writeback,
			                      plugin::Api<ImageCtxT>& plugin_api) :
    m_image_ctx(image_ctx), m_image_cache(new tlc::TempLocalCache<ImageCtx>(*image_ctx, image_temp_local_writeback, plugin_api)),
    m_plugin_api(plugin_api) {
  }

  ImageCtxT* m_image_ctx;
  tlc::TempLocalCache<ImageCtx>* m_image_cache;
  plugin::Api<ImageCtxT>& m_plugin_api;

  bool preprocess_length(
      io::AioCompletion* aio_comp, io::Extents &image_extents) const;
};

} // namespace cache
} // namespace librbd

extern template class librbd::cache::TempLocalCacheImageDispatch<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_TEMP_LOCAL_CACHE_IMAGE_DISPATCH_H
