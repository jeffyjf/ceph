// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/container/flat_map.hpp>
#include <filesystem>

#include "librbd/ImageCtx.h"
#include "librbd/plugin/Api.h"

#include "librbd/cache/tlc/InitRequest.h"
#include "librbd/cache/tlc/TempLocalCache.h"
#include "librbd/cache/tlc/Types.h"


#define dout_subsys ceph_subsys_rbd_tlc
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::tlc::InitRequest: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace tlc {

template <typename I, typename T>
InitRequest<I, T>::InitRequest(Context *on_finish, T& tlc,
                         I &image_ctx, plugin::Api<I> &plugin_api) :
    m_on_finish(on_finish), m_tlc(tlc), m_image_ctx(image_ctx),
    m_plugin_api(plugin_api) {}

template <typename I, typename H>
int InitRequest<I, H>::init_temp_local_data_file(bool invalid_cache) {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;
  std::string pool_name = m_image_ctx.md_ctx.get_pool_name();
  std::string path = m_image_ctx.config.template get_val<std::string>(
          "rbd_temp_local_cache_path");
  
  m_tlc.m_cache_file_path = path + "/rbd-tlc." + pool_name + "." + m_image_ctx.id + ".pool"; 
  bool data_file_exist = access(m_tlc.m_cache_file_path.c_str(), F_OK) == 0;
  
  //metadata file
  m_tlc.m_metadata_file_path = path + "/rbd-tlc." + pool_name + "." + m_image_ctx.id + ".metadata";
  if (!invalid_cache && data_file_exist){
    if (access(m_tlc.m_metadata_file_path.c_str(), F_OK) == 0) {
      BlockDevice* bdev = BlockDevice::create(
        cct, m_tlc.m_metadata_file_path, AioTransContext::aio_cache_cb, nullptr, nullptr, nullptr);
      int r = bdev->open(m_tlc.m_metadata_file_path);
      if (r < 0) {
        lderr(cct) << "failed to open bdev for reading metadata file:" << r << dendl;
        delete bdev;
        return r;
      }

      ::IOContext ioctx(cct, nullptr);
      bufferlist header_bl;
      r = bdev->read(0, BLOCK_SIZE, &header_bl, &ioctx, false);
      if (r < 0) {
        lderr(cct) << "failed to read length of metadata:" << r << dendl;
        bdev->close();
        delete bdev;
        return -EINVAL;
      }
      auto p = header_bl.cbegin();
      uint64_t metadata_len;
      decode(metadata_len, p);

      bufferlist metadata_bl;
      r = bdev->read(BLOCK_SIZE, metadata_len, &metadata_bl, &ioctx, false);
      if (r < 0) {
        lderr(cct) << "failed to read metadata file:" << r << dendl;
        bdev->close();
        delete bdev;
        return -EINVAL;
      }
      p = metadata_bl.cbegin();
      TempLocalCacheMetadata metadata;
      decode(metadata, p);

      auto image_mod_time = m_plugin_api.get_image_modify_timestamp(&m_image_ctx);
      ldout(cct, 20) << "image modify timestamp: " << image_mod_time << dendl;
      if (image_mod_time<=metadata.timestamp && !metadata.cache_md.empty()) {
        m_tlc.m_cache_md_vec = std::move(metadata.cache_md);
        ldout(cct, 20) << "reuse m_cache_md_vec:"  << m_tlc.m_cache_md_vec.size() << dendl;
        m_tlc.m_cached_bytes =  metadata.cached_bytes;
      }else{
        invalid_cache = true;
      }
      bdev->close();
      delete bdev;
    }else{
      invalid_cache = true;
    }
  }
  
  if (m_tlc.m_cache_md_vec.empty()){
    m_tlc.m_cache_md_vec.resize(m_image_ctx.size/MAP_SIZE + 1);
    ldout(cct, 20) << "init m_cache_md_vec:"  << m_tlc.m_cache_md_vec.size() << dendl;
    m_tlc.m_cached_bytes = 0;
  }

  unlink(m_tlc.m_metadata_file_path.c_str());
  
  // data file
  m_tlc.m_cache_file_size = m_image_ctx.config.template get_val<uint64_t>(
        "rbd_temp_local_cache_size");

  if (!data_file_exist || invalid_cache) {
    int fd = ::open( m_tlc.m_cache_file_path.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
      lderr(cct) << "failed to open temp local cahe file: " <<  m_tlc.m_cache_file_path << dendl;
      return fd;
    }
    int r = truncate( m_tlc.m_cache_file_path.c_str(), m_image_ctx.size);
    if (r != 0) {
      lderr(cct) << "Failed to truncated temp local cache file: " <<  m_tlc.m_cache_file_path << dendl;
      ::close(fd);
      return r;
    }
    ::close(fd);
  }

  BlockDevice* bdev = BlockDevice::create(cct,  m_tlc.m_cache_file_path, AioTransContext::aio_cache_cb, nullptr, nullptr, nullptr);
  int r = bdev->open( m_tlc.m_cache_file_path);
  if (r < 0) {
    lderr(cct) << "failed to open bdev" << dendl;
    delete bdev;
    return r;
  }
  m_tlc.m_bdev=bdev;
  
  return 0;  
}

template <typename I, typename T>
void InitRequest<I, T>::send() {
  auto cct = m_image_ctx.cct;
  ldout(cct, 20) << dendl;

  boost::container::flat_map<std::string, bufferlist> xattrs;
  boost::container::flat_map<std::string, bufferlist>* xattrs_ptr = &xattrs;
  auto ctx = new LambdaContext([this, cct, xattrs_ptr](int r){
    // Determine whether the cache is valid from rados. 
    boost::container::flat_map<std::string, bufferlist> xattrs = *xattrs_ptr;
    ldout(cct, 20) << "Read xattr of header object:" << r <<dendl;
    bool invalid_cache = true;
    if (r<0) {
      lderr(cct) << "Read xattr of header object failed, errno:" << r <<dendl;
      this->finish(r);
      return;
    } else {
      // Check if client is closed normally. If close normally, there is
      // xattr named tlc.close_client and value is 1. Cache may be valid.
      auto it = xattrs.find(HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY);
      if (it != xattrs.end()) {
        std::string close_client_value (it->second.c_str(), it->second.length());
        ldout(cct, 20) << "tlc.close_client:" << close_client_value <<dendl;
        if (close_client_value == HEADER_OBJECT_XATTR_CLOSE_VALUE){
          // Check if cache is valid. If there is xattr named tlc.valid_cache and 
          // value is 1. Cache is valid.
          it = xattrs.find(HEADER_OBJECT_XATTR_VALID_CACHE_KEY);
          if (it != xattrs.end()){
            std::string validate_cache_value (it->second.c_str(), it->second.length());
            ldout(cct, 20) << "tlc.valid_cache:" << validate_cache_value <<dendl;
            if (validate_cache_value == HEADER_OBJECT_XATTR_VALID_CACHE_VALUE){
              invalid_cache = false;
            }
          }
        }
      }
    }
    
    ldout(cct, 5) << "Status of tlc cache is:" << (invalid_cache? "invalid": "valid") << dendl;


    r = init_temp_local_data_file(invalid_cache);
    if (r<0){
      this->finish(r);
      return;
    }
    
    // Set xattr named tlc.close_client of header object. 
    auto ctx = new LambdaContext([this, cct](int r){
      if (r<0) {
        lderr(cct) << "Set xattr named" << HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY << " of header object failed , errno is " << r << dendl;
        this->finish(r);
        return;
      }
      // Set xattr named hac.valid_cache of header object. 
      auto ctx = new LambdaContext([this, cct](int r){
        if (r<0) {
          lderr(cct) << "Set xattr named" << HEADER_OBJECT_XATTR_VALID_CACHE_KEY << " of header object failed , errno is " << r << dendl;
        }
        this->finish(r);
      });
      bufferlist bl;
      bl.append(HEADER_OBJECT_XATTR_VALID_CACHE_VALUE.c_str(), HEADER_OBJECT_XATTR_VALID_CACHE_VALUE.length());
      m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_VALID_CACHE_KEY, bl);
    });
    bufferlist bl;
    bl.append(HEADER_OBJECT_XATTR_NO_CLOSE_VALUE.c_str(), HEADER_OBJECT_XATTR_NO_CLOSE_VALUE.length());
    m_plugin_api.rados_xattr_write(&m_image_ctx, ctx, RBD_HEADER_PREFIX + m_image_ctx.id, HEADER_OBJECT_XATTR_CLOSE_CLIENT_KEY, bl);    
  });
  m_plugin_api.rados_xattrs_read(&m_image_ctx, ctx, m_image_ctx.header_oid, xattrs_ptr);
}

template <typename I, typename T>
void InitRequest<I, T>::finish(int r) {
  m_on_finish->complete(r);
  delete this;
}

} // namespace tlc
} // namespace cache
} // namespace librbd

template class librbd::cache::tlc::InitRequest<librbd::ImageCtx, librbd::cache::tlc::TempLocalCache<librbd::ImageCtx>>;
