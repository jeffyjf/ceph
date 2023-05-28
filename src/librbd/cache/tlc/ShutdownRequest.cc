// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/denc.h"

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"
#include "librbd/cache/tlc/ShutdownRequest.h"
#include "librbd/cache/tlc/Types.h"

#include <iostream>
#include <fstream>


#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::tlc::ShutdownRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace tlc {

template <typename I>
ShutdownRequest<I>::ShutdownRequest(
    Context *on_finish, plugin::Api<I>& plugin_api,
    std::vector<std::map<uint64_t, uint64_t>> &cache_md, I& image_ctx, 
    uint64_t cached_bytes, std::string metadata_file_path) :
  m_on_finish(on_finish), m_plugin_api(plugin_api), m_cache_md(cache_md), 
  m_image_ctx(image_ctx), m_cached_bytes(cached_bytes), 
  m_metadata_file_path(metadata_file_path) {}

template <typename I>
void ShutdownRequest<I>::send() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  
  // create metadata file and write cache metadata
  utime_t timestamp = ceph_clock_now();
  ldout(cct, 20) << "Temp Local close timestamp: " << timestamp << "when shuting down"<< dendl;
  TempLocalCacheMetadata meta{timestamp, m_cached_bytes, m_cache_md};

  int fd = ::open(m_metadata_file_path.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644);
  if (fd < 0) {
    lderr(cct) << "failed to create temp local metadata file: " << m_metadata_file_path << dendl;
    finish(fd);
    return;
  }
  int r = truncate(m_metadata_file_path.c_str(), 1024*1024*1024);
  if (r != 0) {
    lderr(cct) << "failed to resize temp local metadata file: " << m_metadata_file_path << dendl;
    ::close(fd);
    finish(r);
    return ;
  }
  ::close(fd);

  BlockDevice* bdev = BlockDevice::create(
      cct, m_metadata_file_path, AioTransContext::aio_cache_cb, nullptr, nullptr, nullptr);
  r = bdev->open(m_metadata_file_path);
  if (r < 0) {
    lderr(cct) << "failed to open bdev for writing metadata file:" << r << dendl;
    finish(r);
    return;
  } 

  bufferlist meta_bl;
  encode(meta, meta_bl);
  uint64_t len = meta_bl.length();
  uint64_t align_len = round_up_to(len, BLOCK_SIZE);
  meta_bl.append_zero(align_len - len);
  ldout(cct, 20) << "length of metadata:" << align_len << dendl;

  bufferlist header_bl;
  encode(align_len, header_bl);
  len = header_bl.length();
  align_len = round_up_to(len, BLOCK_SIZE);
  header_bl.append_zero(align_len - len);
  ldout(cct, 20) << "length of header:" << align_len << dendl;

  r = bdev->write(0, header_bl, false);
  if (r < 0){
    lderr(cct) << "failed to write length of metadata to file:" << r << dendl;
    finish(r);
    bdev->close();
    delete bdev;
    return;
  } 
  r = bdev->write(align_len, meta_bl, false);
  if (r < 0){
    lderr(cct) << "failed to write metadata to file:" << r << dendl;
    finish(r);
    bdev->close();
    delete bdev;
    return;
  }
  bdev->close();
  delete bdev;

  // setting xattr named tlc.close_client indicates that cient shutdown normally.
  auto ctx = new LambdaContext([this](int r){
    if (r<0) {
      lderr(m_image_ctx.cct) << "Set xattr named" << HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY << " of header object failed for closing, errno is " << r << dendl;
    }
    finish(r);
  });
  bufferlist bl;
  bl.append(HEADER_OBJECT_XATTR_CLOSE_VALUE.c_str(), HEADER_OBJECT_XATTR_CLOSE_VALUE.length());
  m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY, bl);
}

template <typename I>
void ShutdownRequest<I>::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace tlc
} // namespace cache
} // namespace librbd

template class librbd::cache::tlc::ShutdownRequest<librbd::ImageCtx>;
