// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/hac/LocalCacheSlot.h"
#include "librbd/cache/hac/Area.h"

#define dout_subsys ceph_subsys_rbd_hac
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::hac::LocalCacheSlot: " << this << " " \
                           <<  __func__ << ": "

namespace librbd {
namespace cache {
namespace hac {


void ReadResult::add_extent_to_hit(io::Extent&& extent) {
  if (m_current_hit_extents.size() == 0) {
    auto new_ext = std::make_shared<ExtentBuf>(std::move(extent));
    m_current_hit_extents.push_back(new_ext);
    m_all_extents.push_back(new_ext);
  } else {
    auto back_ext = m_current_hit_extents.back();
    if (!back_ext->try_merge(extent)) {
      auto new_ext = std::make_shared<ExtentBuf>(std::move(extent));
      m_current_hit_extents.push_back(new_ext);
      m_all_extents.push_back(new_ext);
    }
  }
}

void ReadResult::add_extent_to_miss(io::Extent&& extent) {
  if (m_current_miss_extents.size() == 0) {
    auto new_ext = std::make_shared<ExtentBuf>(std::move(extent));
    m_current_miss_extents.push_back(new_ext);
    m_all_extents.push_back(new_ext);
  } else {
    auto back_ext = m_current_miss_extents.back();
    if (!back_ext->try_merge(extent)) {
      auto new_ext = std::make_shared<ExtentBuf>(std::move(extent));
      m_current_miss_extents.push_back(new_ext);
      m_all_extents.push_back(new_ext);
    }
  }
}

void ReadResult::finish(int r) {
  ldout(m_cct, 20) << dendl;
  bufferlist tmp_bl;
  for (auto ext_buf : m_all_extents) {
    tmp_bl.claim_append(ext_buf->buf);
  }
  uint64_t bl_offset = m_slot_extent.first-m_all_extents.front()->offset;
  m_origin_extent->buf.substr_of(tmp_bl, bl_offset, m_origin_extent->len);
  m_on_finish->complete(r);
}

std::ostream& operator<<(std::ostream& os, const SlotRootRecord& record) {
  os << "{" << "pos: " << record.pos << ", " << "image_offset: " << record.image_offset << "}";
  return os;
}

LocalCacheSlot::LocalCacheSlot(uint64_t pos,
    cache::ImageWriteThroughInterface& image_write_through) :
    m_is_free(true), m_pos(pos),
    m_image_write_through(image_write_through),
    m_slot_lock("librbd::cache::hac::LocalCacheSlot::m_slot_lock") {
  m_block_flags.resize(slot_conf()->blocks_per_slot(), false);
}

LocalCacheSlot::LocalCacheSlot(SlotRootRecord& root_record,
    cache::ImageWriteThroughInterface& image_write_through, CephContext* cct) :
    m_is_free(false), m_pos(root_record.pos),
    m_image_offset(root_record.image_offset),
    m_image_write_through(image_write_through),
    m_slot_lock("librbd::cache::hac::LocalCacheSlot::m_slot_lock") {
  ldout(cct, 20) << "Construct slot from record: " << root_record << dendl;
  this->m_block_flags.resize(slot_conf()->blocks_per_slot(), false);
  uint64_t flags = slot_conf()->blocks_per_slot() / 8;
  for (uint64_t i=0; i<flags; i++) {
    uint8_t record_flag = root_record.block_flags[i];
    for (int n=0; n<8; n++) {
      bool block_flag = record_flag & (1ULL << (7 - n % 8));
      this->m_block_flags[i*8+n] = block_flag;
      if (block_flag) {
        ldout(cct, 20) << i*8+n << " " << m_block_flags[i*8+n] << dendl;
      }
    }
  }
}

SlotRootRecord LocalCacheSlot::get_root_record(CephContext* cct) {
  ldout(cct, 20) << dendl;
  SlotRootRecord root_record{
    .pos = this->m_pos,
    .image_offset = this->m_image_offset};

  uint64_t flags = slot_conf()->blocks_per_slot() / 8;
  root_record.block_flags.resize(flags, 0);
  for (uint64_t i=0; i<flags; i++) {
    uint8_t tmp_flag = root_record.block_flags[i];
    for (int n=0; n<8; n++) {
      if (this->m_block_flags[i*8+n]) {
        ldout(cct, 20) << i*8+n << " " << m_block_flags[i*8+n] << dendl;
        tmp_flag |= (1ULL << (7-n));
      }
    }
    root_record.block_flags[i] = tmp_flag;
  }
  return root_record;
}

uint64_t LocalCacheSlot::get_index() {
  return (this->m_pos - ROOT_RECORD_SIZE) >> area_conf()->size_order();
}

uint64_t LocalCacheSlot::get_area_index() {
  return this->m_image_offset >> area_conf()->size_order();
}

io::Extent LocalCacheSlot::get_slot_extent(ExtentBufPtr extent_buf) {
  io::Extent extent{extent_buf->offset-m_image_offset, extent_buf->len};
  return extent;
}

void LocalCacheSlot::update_slot_cached_block(ExtentBufPtr extent_buf, CephContext* cct, Context *on_finish) {
  ldout(cct, 20) << dendl;
  RWLock::RLocker l(m_slot_lock);
  ceph_assert(m_bdev != nullptr);
  AioTransContext* aio = new AioTransContext(cct, on_finish);
  bufferlist copy_bl;
  extent_buf->buf.begin(0).copy(extent_buf->buf.length(), copy_bl);
  ceph_assert((m_pos+extent_buf->offset) % BLOCK_SIZE == 0);
  ceph_assert(copy_bl.length() % BLOCK_SIZE == 0);
  m_bdev->aio_write(m_pos+extent_buf->offset, copy_bl, &aio->ioc, false, WRITE_LIFE_NOT_SET);
  m_bdev->aio_submit(&aio->ioc);
  uint64_t start_block_index = extent_buf->offset >> BLOCK_SIZE_ORDER;
  uint64_t end_block_index = (extent_buf->offset + extent_buf->len - 1) >> BLOCK_SIZE_ORDER;
  ldout(cct, 24) << "Set block: " << start_block_index << "~" <<  end_block_index << " in slot: " << *this << " as true." << dendl;
  std::vector<bool>::iterator it = m_block_flags.begin();
  std::advance(it, start_block_index);
  std::fill_n(it, end_block_index-start_block_index+1, true);
}

void LocalCacheSlot::read(
    ExtentBufPtr image_extent_buf, uint64_t tid, CephContext* cct, Context *on_finish) {
  ldout(cct, 10) << "tid=" << tid << ", image extent " << *image_extent_buf << dendl;
  ceph_assert(image_extent_buf->offset>=m_image_offset);

  io::Extent slot_ext = get_slot_extent(image_extent_buf);
  ReadResult* read_result = new ReadResult(
    image_extent_buf, slot_ext, on_finish, cct);

  uint64_t start_block_index = slot_ext.first >> BLOCK_SIZE_ORDER;
  uint64_t end_block_index = (slot_ext.first+slot_ext.second-1) >> BLOCK_SIZE_ORDER;
  ldout(cct, 20) << start_block_index << "~" << end_block_index << dendl;
  uint64_t continuous_hit_blocks = 0;
  uint64_t continuous_miss_blocks = 0;
  for (uint64_t i=start_block_index; i<=end_block_index; i++) {
    if (m_block_flags[i]) {
      continuous_hit_blocks++;
      if (continuous_miss_blocks > 0){
        read_result->add_extent_to_miss({(i-continuous_miss_blocks)<<BLOCK_SIZE_ORDER, BLOCK_SIZE*continuous_miss_blocks});
        continuous_miss_blocks = 0;
      }
    } else {
      continuous_miss_blocks++;
      if (continuous_hit_blocks > 0){
        read_result->add_extent_to_hit({(i-continuous_hit_blocks)<<BLOCK_SIZE_ORDER, BLOCK_SIZE*continuous_hit_blocks});
        continuous_hit_blocks = 0;
      }
    }
  }
  if (continuous_hit_blocks > 0){
    read_result->add_extent_to_hit({(end_block_index+1-continuous_hit_blocks)<<BLOCK_SIZE_ORDER, BLOCK_SIZE*continuous_hit_blocks});
  }
  if (continuous_miss_blocks > 0){
    read_result->add_extent_to_miss({(end_block_index+1-continuous_miss_blocks)<<BLOCK_SIZE_ORDER, BLOCK_SIZE*continuous_miss_blocks});
  }

  auto gather_ctx = new C_Gather(cct, read_result);
  if (read_result->m_current_hit_extents.size()>0) {
    RWLock::RLocker l(m_slot_lock);
    AioTransContext* aio = new AioTransContext(cct, gather_ctx->new_sub());
    for (auto hit_extent : read_result->m_current_hit_extents) {
      ldout(cct, 20) << "Hit extent: " << *hit_extent << dendl;
      ceph_assert((this->m_pos+hit_extent->offset) % BLOCK_SIZE == 0);
      ceph_assert(hit_extent->len % BLOCK_SIZE == 0);
      m_bdev->aio_read(this->m_pos+hit_extent->offset, hit_extent->len, &(hit_extent->buf), &aio->ioc);
    }
    m_bdev->aio_submit(&aio->ioc);
  }
  for (auto miss_extent : read_result->m_current_miss_extents) {
    ldout(cct, 20) << "Miss extent: " << *miss_extent << dendl;
    auto ctx = gather_ctx->new_sub();
    auto miss_ctx = new LambdaContext([this, ctx, miss_extent, cct](int r) {
      ldout(cct, 20) << "Read from rados complete, extent: " << *miss_extent << dendl;
      update_slot_cached_block(miss_extent, cct, ctx);
    });

    m_image_write_through.aio_read(
      {{m_image_offset+miss_extent->offset, miss_extent->len}},
      &(miss_extent->buf), 0, miss_ctx);
  }
  gather_ctx->activate();
}

void LocalCacheSlot::write(
    ExtentBufPtr image_extent_buf, uint64_t tid, CephContext* cct, Context *on_finish) {
  ldout(cct, 20) << "tid=" << tid << ", image extent " << *image_extent_buf << dendl;

  io::Extent slot_ext = get_slot_extent(image_extent_buf);
  uint64_t start_offset = slot_ext.first;
  uint64_t end_offset = slot_ext.first + slot_ext.second;
  uint64_t start_block_index = start_offset >> BLOCK_SIZE_ORDER;
  uint64_t end_block_index = (end_offset-1) >> BLOCK_SIZE_ORDER;

  if (!(m_block_flags[start_block_index] && m_block_flags[end_block_index])) {
    if (image_extent_buf->len<2*BLOCK_SIZE) {
      on_finish->complete(0);
      return;
    }
  }

  if (!m_block_flags[start_block_index]) {
    start_offset =  round_up_to(start_offset, BLOCK_SIZE);
    start_block_index = start_offset >> BLOCK_SIZE_ORDER;
    ceph_assert(start_offset % BLOCK_SIZE == 0);
  }

  if (!m_block_flags[end_block_index]) {
    uint64_t end_align_mod = end_offset % BLOCK_SIZE;
    end_offset = end_offset - end_align_mod;
    end_block_index = (end_offset-1) >> BLOCK_SIZE_ORDER;
    ceph_assert(end_offset % BLOCK_SIZE == 0);
  }

  bufferlist* tmp_bl = new bufferlist();
  tmp_bl->substr_of(image_extent_buf->buf, start_offset-slot_ext.first, end_offset-start_offset);


  uint64_t aligned_start_offset = start_block_index<<BLOCK_SIZE_ORDER;
  uint64_t aliened_end_offset = (end_block_index+1)<<BLOCK_SIZE_ORDER;
  ceph_assert(aligned_start_offset<=start_offset);
  ceph_assert(aliened_end_offset>=end_offset);

  ExtentBufPtr aliend_ext_buf = std::make_shared<ExtentBuf>(io::Extent{aligned_start_offset, aliened_end_offset-aligned_start_offset});
  ExtentBufPtr mod_start_ext = std::make_shared<ExtentBuf>(io::Extent{0, 0});
  ExtentBufPtr mod_end_ext = std::make_shared<ExtentBuf>(io::Extent{0, 0});
  auto gather_ctx = new C_Gather(cct, new LambdaContext([this, aliend_ext_buf, mod_start_ext, tmp_bl, mod_end_ext, on_finish, cct](int r) {
    if (r<0) {
      // TODO
      on_finish->complete(r);
      delete tmp_bl;
    }

    aliend_ext_buf->buf.claim_append(mod_start_ext->buf);
    aliend_ext_buf->buf.claim_append(*tmp_bl);
    aliend_ext_buf->buf.claim_append(mod_end_ext->buf);
    ceph_assert(aliend_ext_buf->buf.length()==aliend_ext_buf->len);
    update_slot_cached_block(aliend_ext_buf, cct, on_finish);
    delete tmp_bl;
  }));

  if (aligned_start_offset<start_offset) {
    mod_start_ext->offset = aligned_start_offset + m_image_offset;
    mod_start_ext->len = start_offset - aligned_start_offset;
    read(mod_start_ext, tid, cct, gather_ctx->new_sub());
  }

  if (end_offset<aliened_end_offset) {
    mod_end_ext->offset = end_offset + m_image_offset;
    mod_end_ext->len = aliened_end_offset - end_offset;
    read(mod_end_ext, tid, cct, gather_ctx->new_sub());
  }
  gather_ctx->activate();
}

void LocalCacheSlot::discard(
    ExtentBufPtr image_extent_buf, CephContext* cct) {
  ldout(cct, 20) << dendl;

  io::Extent slot_ext = get_slot_extent(image_extent_buf);
  uint64_t start_offset = slot_ext.first;
  uint64_t end_offset = slot_ext.first + slot_ext.second;
  uint64_t start_block_index = start_offset >> BLOCK_SIZE_ORDER;
  uint64_t end_block_index = (end_offset-1) >> BLOCK_SIZE_ORDER;
  
  if (start_block_index != end_block_index){
    if (m_block_flags[start_block_index]) {
      uint64_t offset =  round_up_to(start_offset, BLOCK_SIZE);
      // If discard involes the whole block，invalid the block.
      if (offset == start_offset){
        m_block_flags[start_block_index] = false;
      }
    }
    if (m_block_flags[end_block_index]) {
      uint64_t offset =  round_up_to(end_offset, BLOCK_SIZE);
      // If discard involes the whole block，invalid the block.
      if (offset == end_offset){
          m_block_flags[end_block_index] = false;
      }
    }
    if (end_block_index-start_block_index>1){
      std::vector<bool>::iterator it = m_block_flags.begin();
      std::advance(it, start_block_index+1);
      std::fill_n(it, end_block_index-start_block_index-1, false);
    }
  }else{
    if (m_block_flags[start_block_index]){
        // If discard involes the whole block，invalid the block.
        if (slot_ext.second == BLOCK_SIZE){
           m_block_flags[end_block_index] = false;
        }
    }
  }
}

void LocalCacheSlot::compare_and_write(
    ExtentBufPtr image_extent_buf, CephContext* cct) {
  ldout(cct, 20) << dendl;

  io::Extent slot_ext = get_slot_extent(image_extent_buf);
  uint64_t start_offset = slot_ext.first;
  uint64_t end_offset = slot_ext.first + slot_ext.second;
  uint64_t start_block_index = start_offset >> BLOCK_SIZE_ORDER;
  uint64_t end_block_index = (end_offset-1) >> BLOCK_SIZE_ORDER;
  
  // Invalid involed block.
  std::vector<bool>::iterator it = m_block_flags.begin();
  std::advance(it, start_block_index);
  std::fill_n(it, end_block_index-start_block_index+1, false);
}

void LocalCacheSlot::set_bdev(BlockDevice* bdev) {
  this->m_bdev = bdev;
}

void LocalCacheSlot::set_image_offset(uint64_t image_offset) {
  this->m_image_offset = image_offset;
}

void LocalCacheSlot::reset_block_flags() {
  std::fill_n(m_block_flags.begin(), slot_conf()->blocks_per_slot(), false);
}

void LocalCacheSlot::reset(CephContext* cct) {
  ldout(cct, 20) << "Reseting slot: " << *this << dendl;
  RWLock::WLocker l(m_slot_lock);
  m_is_free.store(true);
  reset_block_flags();
}


std::ostream& operator<<(std::ostream& os, const LocalCacheSlot& slot) {
  os << "{" << "pos: " << slot.m_pos << ", " << "is_free: " << slot.m_is_free.load() << "}";
  return os;
}

std::string local_placement_slot_file_name(
    const std::string path, const std::string img_id,
    const std::string pool_name) {
  return path + "/rbd-hac." + pool_name + "." + img_id + ".pool";
}

SlotConf* slot_instance = 0;
SlotConf* slot_conf(){ 
  if (slot_instance == 0){
    slot_instance = new SlotConf();
  }
  return slot_instance;
}


} // namespace hac
} // namespace cache
} // namespace librbd
