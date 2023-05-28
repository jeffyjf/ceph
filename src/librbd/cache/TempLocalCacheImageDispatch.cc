// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/dout.h"
#include "include/neorados/RADOS.hpp"
#include "librbd/cache/tlc/TempLocalCache.h"
#include "librbd/cache/TempLocalCacheImageDispatch.h"
#include "librbd/ImageCtx.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/Utils.h"
#include "librbd/io/ImageDispatcherInterface.h"

#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::TempLocalCacheImageDispatch: " << this << " " \
                           << __func__ << ": "

namespace librbd {
namespace cache {

template <typename I>
void TempLocalCacheImageDispatch<I>::shut_down(Context* on_finish) {
  m_image_cache->shut_down(on_finish);
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::read(
    io::AioCompletion* aio_comp, io::Extents &&image_extents,
    io::ReadResult &&read_result, IOContext io_context,
    int op_flags, int read_flags,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  if (io_context->read_snap().value_or(CEPH_NOSNAP) != CEPH_NOSNAP) {
    return false;
  }

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;
  if (preprocess_length(aio_comp, image_extents)) {
    return true;
  }

  m_plugin_api.update_aio_comp(aio_comp, 1, read_result, image_extents);

  auto *req_comp = m_plugin_api.create_image_read_request(aio_comp, 0, image_extents);
  m_image_cache->read(std::move(image_extents),
                      &req_comp->bl, op_flags,
                      req_comp, tid);
  return true;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::write(
    io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&bl,
    IOContext io_context, int op_flags, const ZTracer::Trace &parent_trace,
    uint64_t tid, std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {

  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  if (io_context->read_snap().value_or(CEPH_NOSNAP) != CEPH_NOSNAP) {
    return false;
  }

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;
  if (preprocess_length(aio_comp, image_extents)) {
    return true;
  }

  m_plugin_api.update_aio_comp(aio_comp, 1);
  io::C_AioRequest *req_comp = m_plugin_api.create_aio_request(aio_comp);
  m_image_cache->write(std::move(image_extents),
                       std::move(bl), op_flags, req_comp, tid);

  return true;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::discard(
    io::AioCompletion* aio_comp, io::Extents &&image_extents,
    uint32_t discard_granularity_bytes, IOContext io_context,
    const ZTracer::Trace &parent_trace,
    uint64_t tid, std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {

  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::write_same(
    io::AioCompletion* aio_comp, io::Extents &&image_extents,
    bufferlist &&bl, IOContext io_context,
    int op_flags, const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {
  
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;
  
  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::compare_and_write(
    io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&cmp_bl,
    bufferlist &&bl, uint64_t *mismatch_offset, IOContext io_context,
    int op_flags, const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {
  
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::flush(
    io::AioCompletion* aio_comp, io::FlushSource flush_source,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result,
    Context** on_finish, Context* on_dispatched) {
  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::list_snaps(
    io::AioCompletion* aio_comp, io::Extents&& image_extents,
    io::SnapIds&& snap_ids,
    int list_snaps_flags, io::SnapshotDelta* snapshot_delta,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context** on_finish,
    Context* on_dispatched) {
  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::invalidate_cache(Context* on_finish) {
  return false;
}

template <typename I>
bool TempLocalCacheImageDispatch<I>::preprocess_length(
    io::AioCompletion* aio_comp, io::Extents &image_extents) const {
  auto total_bytes = io::util::get_extents_length(image_extents);
  if (total_bytes == 0) {
    m_plugin_api.update_aio_comp(aio_comp, 0);
    return true;
  }
  return false;
}

} // namespace io
} // namespace librbd

template class librbd::cache::TempLocalCacheImageDispatch<librbd::ImageCtx>;
