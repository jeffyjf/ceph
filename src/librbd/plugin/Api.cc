// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/Timer.h"
#include "librbd/plugin/Api.h"
#include "librbd/ImageCtx.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/Utils.h"
#include "librbd/Operations.h"
#include "librbd/Utils.h"
#include "include/neorados/RADOS.hpp"
#include "librbd/asio/Utils.h"

namespace librbd {
namespace plugin {

template <typename I>
void Api<I>::read_parent(
    I *image_ctx, uint64_t object_no, io::ReadExtents* extents,
    librados::snap_t snap_id, const ZTracer::Trace &trace,
    Context* on_finish) {
  io::util::read_parent<I>(image_ctx, object_no, extents, snap_id, trace,
                           on_finish);
}

template <typename I>
void Api<I>::execute_image_metadata_set(
    I *image_ctx, const std::string &key,
    const std::string &value, Context *on_finish) {
  ImageCtx* ictx = util::get_image_ctx(image_ctx);
  ictx->operations->execute_metadata_set(key, value, on_finish);
}

template <typename I>
void Api<I>::execute_image_metadata_remove(
    I *image_ctx, const std::string &key, Context *on_finish) {
  ImageCtx* ictx = util::get_image_ctx(image_ctx);
  ictx->operations->execute_metadata_remove(key, on_finish);
}

template <typename I>
void Api<I>::get_image_timer_instance(
    CephContext *cct, SafeTimer **timer, ceph::mutex **timer_lock) {
  ImageCtx::get_timer_instance(cct, timer, timer_lock);
}

template <typename I>
bool Api<I>::test_image_features(I *image_ctx, uint64_t features) {
  return image_ctx->test_features(features);
}

template <typename I>
uint64_t Api<I>::get_image_size(I *image_ctx) {
  std::shared_lock image_locker(image_ctx->image_lock);
  return image_ctx->get_current_size();
}

template <typename I>
utime_t Api<I>::get_image_modify_timestamp(I *image_ctx) {
  return image_ctx->get_modify_timestamp();
}

template <typename I>
void Api<I>::update_aio_comp(io::AioCompletion* aio_comp,
                             uint32_t request_count,
                             io::ReadResult &read_result,
                             io::Extents &image_extents) {
  aio_comp->set_request_count(request_count);
  aio_comp->read_result = std::move(read_result);
  aio_comp->read_result.set_image_extents(image_extents);
  start_in_flight_io(aio_comp);
}

template <typename I>
void Api<I>::update_aio_comp(
    io::AioCompletion* aio_comp, uint32_t request_count) {
  aio_comp->set_request_count(request_count);
  start_in_flight_io(aio_comp);
}

template <typename I>
io::ReadResult::C_ImageReadRequest* Api<I>::create_image_read_request(
    io::AioCompletion* aio_comp, uint64_t buffer_offset,
    const Extents& image_extents) {
  return new io::ReadResult::C_ImageReadRequest(
    aio_comp, buffer_offset, image_extents);
}

template <typename I>
io::C_AioRequest* Api<I>::create_aio_request(io::AioCompletion* aio_comp) {
  io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
  return req_comp;
}

template <typename I>
void Api<I>::start_in_flight_io(io::AioCompletion* aio_comp) {
  if (!aio_comp->async_op.started()) {
    aio_comp->start_op();
  }
}

template <typename I>
void Api<I>::rados_object_write(I *image_ctx, Context *on_finish, std::string name, ceph::bufferlist &bl) {
  neorados::WriteOp write_op;
  write_op.write_full(std::move(bl));
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(write_op),
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

template <typename I>
void Api<I>::rados_object_read(I *image_ctx, Context *on_finish, std::string name, ceph::bufferlist &bl, uint64_t offset, uint64_t length) {
  neorados::ReadOp read_op;
  read_op.read(offset, offset+length, &bl);
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(read_op), nullptr,
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

template <typename I>
void Api<I>::rados_object_remove(I *image_ctx, Context *on_finish, std::string name) {
  neorados::WriteOp write_op;
  write_op.remove();
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(write_op),
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

template <typename I>
void Api<I>::rados_xattr_write(I *image_ctx, Context *on_finish, std::string name, std::string xattr_name, ceph::bufferlist& bl) {
  neorados::WriteOp write_op;
  write_op.setxattr(xattr_name, std::move(bl));
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(write_op),
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

template <typename I>
void Api<I>::rados_xattr_read(I *image_ctx, Context *on_finish, std::string name, std::string xattr_name, ceph::bufferlist* out) {
  neorados::ReadOp read_op;
  read_op.get_xattr(xattr_name, out);
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(read_op), nullptr,
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

template <typename I>
void Api<I>::rados_xattrs_read(I *image_ctx, Context *on_finish, std::string name, 
  boost::container::flat_map<std::string, bufferlist> *xattr_out_map) {
  neorados::ReadOp read_op;
  read_op.get_xattrs(xattr_out_map);
  image_ctx->rados_api.execute(
    {name}, *(image_ctx->get_data_io_context()),
    std::move(read_op), nullptr,
    librbd::asio::util::get_context_adapter(on_finish), nullptr, nullptr);
}

} // namespace plugin
} // namespace librbd

template class librbd::plugin::Api<librbd::ImageCtx>;
