// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "include/Context.h"
#include "common/blkdev.h"
#include "common/errno.h"
#include "common/Throttle.h"
#include "include/compat.h"
#include "include/encoding.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include <iostream>
#include <boost/program_options.hpp>
#include <boost/scoped_ptr.hpp>

namespace rbd {
namespace action {

int do_import_diff_fd(librbd::Image &image, int fd,
		   bool no_progress, int format)
{
  int r;
  struct stat stat_buf;
  utils::ProgressContext pc("Importing image diff", no_progress);
  uint64_t size = 0;
  uint64_t off = 0;
  string from, to;
  string banner = (format == 1 ? utils::RBD_DIFF_BANNER:utils::RBD_DIFF_BANNER_V2);
  char buf[banner.size() + 1];

  bool from_stdin = (fd == 0);
  if (!from_stdin) {
    r = ::fstat(fd, &stat_buf);
    if (r < 0)
      goto done;
    size = (uint64_t)stat_buf.st_size;
  }

  r = safe_read_exact(fd, buf, banner.size());
  if (r < 0)
    goto done;
  buf[banner.size()] = '\0';
  if (strcmp(buf, banner.c_str())) {
    std::cerr << "invalid banner '" << buf << "', expected '"
	     << banner << "'" << std::endl;
    r = -EINVAL;
    goto done;
  }

  while (true) {
    __u8 tag;
    r = safe_read_exact(fd, &tag, 1);
    if (r < 0) {
      goto done;
    }

    if (tag == RBD_DIFF_END) {
      break;
    } else if (tag == RBD_DIFF_FROM_SNAP) {
      r = utils::read_string(fd, 4096, &from);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
	goto done;

      bool exists; 
      r = image.snap_exists2(from.c_str(), &exists);
      if (r < 0)
	goto done;

      if (!exists) {
	std::cerr << "start snapshot '" << from
		  << "' does not exist in the image, aborting" << std::endl;
	r = -EINVAL;
	goto done;
      }
    }
    else if (tag == RBD_DIFF_TO_SNAP) {
      r = utils::read_string(fd, 4096, &to);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
	goto done;

      // verify this snap isn't already present
      bool exists;
      r = image.snap_exists2(to.c_str(), &exists);
      if (r < 0)
	goto done;
      
      if (exists) {
	std::cerr << "end snapshot '" << to
		  << "' already exists, aborting" << std::endl;
	r = -EEXIST;
	goto done;
      }
    } else if (tag == RBD_DIFF_IMAGE_SIZE) {
      uint64_t end_size;
      char buf[8];
      r = safe_read_exact(fd, buf, 8);
      if (r < 0)
	goto done;
      bufferlist bl;
      bl.append(buf, 8);
      bufferlist::iterator p = bl.begin();
      ::decode(end_size, p);
      uint64_t cur_size;
      image.size(&cur_size);
      if (cur_size != end_size) {
	image.resize(end_size);
      }
      if (from_stdin)
	size = end_size;
    } else if (tag == RBD_DIFF_WRITE || tag == RBD_DIFF_ZERO) {
      uint64_t len;
      char buf[16];
      r = safe_read_exact(fd, buf, 16);
      if (r < 0)
	goto done;
      bufferlist bl;
      bl.append(buf, 16);
      bufferlist::iterator p = bl.begin();
      ::decode(off, p);
      ::decode(len, p);

      if (tag == RBD_DIFF_WRITE) {
	bufferptr bp = buffer::create(len);
	r = safe_read_exact(fd, bp.c_str(), len);
	if (r < 0)
	  goto done;
	bufferlist data;
	data.append(bp);
	image.write2(off, len, data, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      } else {
	image.discard(off, len);
      }
    } else {
      std::cerr << "unrecognized tag byte " << (int)tag
		<< " in stream; aborting" << std::endl;
      r = -EINVAL;
      goto done;
    }
    if (!from_stdin) {
      // progress through input
      uint64_t off = lseek64(fd, 0, SEEK_CUR);
      pc.update_progress(off, size);
    } else if (size) {
      // progress through image offsets.  this may jitter if blocks
      // aren't in order, but it is better than nothing.
      pc.update_progress(off, size);
    }
  }
  // take final snap
  if (to.length()) {
    r = image.snap_create(to.c_str());
  }

done:
  if (r < 0)
    pc.fail();
  else
    pc.finish();
  return r;
}

int do_import_diff(librbd::Image &image, const char *path,
                bool no_progress)
{
  int r;
  int fd;

  if (strcmp(path, "-") == 0) {
    fd = 0;
  } else {
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      r = -errno;
      std::cerr << "rbd: error opening " << path << std::endl;
      return r;
    }
  }
  r = do_import_diff_fd(image, fd, no_progress, 1);

  if (fd != 0)
    close(fd);
  return r;
}

namespace import_diff {

namespace at = argument_types;
namespace po = boost::program_options;

void get_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_path_options(positional, options,
                       "import file (or '-' for stdin)");
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
  at::add_no_progress_option(options);
}

int execute(const po::variables_map &vm) {
  std::string path;
  int r = utils::get_path(vm, utils::get_positional_argument(vm, 0), &path);
  if (r < 0) {
    return r;
  }

  size_t arg_index = 1;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, image_name, "", false,
                                 &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  r = do_import_diff(image, path.c_str(), vm[at::NO_PROGRESS].as<bool>());
  if (r < 0) {
    cerr << "rbd: import-diff failed: " << cpp_strerror(r) << std::endl;
    return r;
  }
  return 0;
}

Shell::Action action(
  {"import-diff"}, {}, "Import an incremental diff.", "", &get_arguments,
  &execute);

} // namespace import_diff

namespace import {

namespace at = argument_types;
namespace po = boost::program_options;

class C_Import : public Context {
public:
  C_Import(SimpleThrottle &simple_throttle, librbd::Image &image,
           bufferlist &bl, uint64_t offset)
    : m_throttle(simple_throttle), m_image(image),
      m_aio_completion(
        new librbd::RBD::AioCompletion(this, &utils::aio_context_callback)),
      m_bufferlist(bl), m_offset(offset)
  {
  }

  void send()
  {
    m_throttle.start_op();

    int op_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL |
                   LIBRADOS_OP_FLAG_FADVISE_NOCACHE;
    int r = m_image.aio_write2(m_offset, m_bufferlist.length(), m_bufferlist,
                               m_aio_completion, op_flags);
    if (r < 0) {
      std::cerr << "rbd: error requesting write to destination image"
                << std::endl;
      m_aio_completion->release();
      m_throttle.end_op(r);
    }
  }

  void finish(int r) override
  {
    if (r < 0) {
      std::cerr << "rbd: error writing to destination image at offset "
                << m_offset << ": " << cpp_strerror(r) << std::endl;
    }
    m_throttle.end_op(r);
  }

private:
  SimpleThrottle &m_throttle;
  librbd::Image &m_image;
  librbd::RBD::AioCompletion *m_aio_completion;
  bufferlist m_bufferlist;
  uint64_t m_offset;
};

static int do_import(librbd::RBD &rbd, librados::IoCtx& io_ctx,
                     const char *imgname, const char *path,
		     librbd::ImageOptions& opts, bool no_progress,
		     int import_format)
{
  // check current supported formats.
  if (import_format != 1 && import_format != 2) {
    std::cerr << "rbd: wrong file format to import" << std::endl;
    return -EINVAL;
  }

  int fd, r;
  struct stat stat_buf;
  utils::ProgressContext pc("Importing image", no_progress);

  assert(imgname);

  uint64_t order;
  if (opts.get(RBD_IMAGE_OPTION_ORDER, &order) != 0) {
    order = g_conf->rbd_default_order;
  }

  // try to fill whole imgblklen blocks for sparsification
  uint64_t image_pos = 0;
  size_t imgblklen = 1 << order;
  char *p = new char[imgblklen];
  size_t reqlen = imgblklen;    // amount requested from read
  ssize_t readlen;              // amount received from one read
  size_t blklen = 0;            // amount accumulated from reads to fill blk
  librbd::Image image;
  uint64_t size = 0;
  uint64_t snap_num;
  char image_buf[utils::RBD_IMAGE_BANNER_V2.size() + 1];

  boost::scoped_ptr<SimpleThrottle> throttle;
  bool from_stdin = !strcmp(path, "-");
  if (from_stdin) {
    throttle.reset(new SimpleThrottle(1, false));
    fd = 0;
    size = 1ULL << order;
  } else {
    throttle.reset(new SimpleThrottle(
      max(g_conf->rbd_concurrent_management_ops, 1), false));
    if ((fd = open(path, O_RDONLY)) < 0) {
      r = -errno;
      std::cerr << "rbd: error opening " << path << std::endl;
      goto done2;
    }

    if ((fstat(fd, &stat_buf)) < 0) {
      r = -errno;
      std::cerr << "rbd: stat error " << path << std::endl;
      goto done;
    }
    if (S_ISDIR(stat_buf.st_mode)) {
      r = -EISDIR;
      std::cerr << "rbd: cannot import a directory" << std::endl;
      goto done;
    }
    if (stat_buf.st_size)
      size = (uint64_t)stat_buf.st_size;

    if (!size) {
      int64_t bdev_size = 0;
      r = get_block_device_size(fd, &bdev_size);
      if (r < 0) {
        std::cerr << "rbd: unable to get size of file/block device"
                  << std::endl;
        goto done;
      }
      assert(bdev_size >= 0);
      size = (uint64_t) bdev_size;
    }
#ifdef HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
  
  if (import_format == 2) {
    if (from_stdin || size < utils::RBD_IMAGE_BANNER_V2.size()) {
      r = -EINVAL;
      goto done;
    }
    r = safe_read_exact(fd, image_buf, utils::RBD_IMAGE_BANNER_V2.size());
    if (r < 0)
      goto done;

    image_buf[utils::RBD_IMAGE_BANNER_V2.size()] = '\0';
    if (strcmp(image_buf, utils::RBD_IMAGE_BANNER_V2.c_str())) {
      // Old format
      r = -EINVAL;
      goto done;
    }

    r = safe_read_exact(fd, &size, 8);
    if (r < 0) {
      goto done;
    }

    // As V1 format for image is already deprecated, import image in V2 by default.
    uint64_t image_format = 2;
    if (opts.get(RBD_IMAGE_OPTION_FORMAT, &image_format) != 0) {
      opts.set(RBD_IMAGE_OPTION_FORMAT, image_format);
    }

    while (1) {
      __u8 tag;
      r = safe_read_exact(fd, &tag, 1);
      if (r < 0) {
	goto done;
      }

      if (tag == RBD_EXPORT_IMAGE_END) {
	break;
      } else if (tag == RBD_EXPORT_IMAGE_ORDER) {
	uint64_t order = 0;
	r = safe_read_exact(fd, &order, 8);
	if (r < 0) {
	  goto done;
	}
	if (opts.get(RBD_IMAGE_OPTION_ORDER, &order) != 0) {
	  opts.set(RBD_IMAGE_OPTION_ORDER, order);
	}
      } else {
	std::cerr << "rbd: invalid tag in image priority zone: " << tag << std::endl;
	r = -EINVAL;
	goto done;
      }
      //TODO, set the image options according flags and appending data.
    }
  }

  r = rbd.create4(io_ctx, imgname, size, opts);
  if (r < 0) {
    std::cerr << "rbd: image creation failed" << std::endl;
    goto done;
  }

  r = rbd.open(io_ctx, image, imgname);
  if (r < 0) {
    std::cerr << "rbd: failed to open image" << std::endl;
    goto err;
  }

  if (import_format == 2) {
    char snap_buf[utils::RBD_IMAGE_SNAPS_BANNER_V2.size() + 1];

    r = safe_read_exact(fd, snap_buf, utils::RBD_IMAGE_SNAPS_BANNER_V2.size());
    if (r < 0)
      goto done;
    snap_buf[utils::RBD_IMAGE_SNAPS_BANNER_V2.size()] = '\0';
    if (strcmp(snap_buf, utils::RBD_IMAGE_SNAPS_BANNER_V2.c_str())) {
      cerr << "Incorrect RBD_IMAGE_SNAPS_BANNER." << std::endl;
      return -EINVAL;
    } else {
      r = safe_read_exact(fd, &snap_num, 8);
      if (r < 0) {
        goto close;
      }
    }

    for (size_t i = 0; i < snap_num; i++) {
      r = do_import_diff_fd(image, fd, true, 2);
      if (r < 0) {
	pc.fail();
	std::cerr << "rbd: import-diff failed: " << cpp_strerror(r) << std::endl;
	return r;
      }
      pc.update_progress(i + 1, snap_num);
    }
  } else {
    reqlen = min(reqlen, size);
    // loop body handles 0 return, as we may have a block to flush
    while ((readlen = ::read(fd, p + blklen, reqlen)) >= 0) {
      if (throttle->pending_error()) {
	break;
      }

      blklen += readlen;
      // if read was short, try again to fill the block before writing
      if (readlen && ((size_t)readlen < reqlen)) {
	reqlen -= readlen;
	continue;
      }
      if (!from_stdin)
	pc.update_progress(image_pos, size);

      bufferlist bl(blklen);
      bl.append(p, blklen);
      // resize output image by binary expansion as we go for stdin
      if (from_stdin && (image_pos + (size_t)blklen) > size) {
	size *= 2;
	r = image.resize(size);
	if (r < 0) {
	  std::cerr << "rbd: can't resize image during import" << std::endl;
	  goto close;
	}
      }

      // write as much as we got; perhaps less than imgblklen
      // but skip writing zeros to create sparse images
      if (!bl.is_zero()) {
	C_Import *ctx = new C_Import(*throttle, image, bl, image_pos);
	ctx->send();
      }

      // done with whole block, whether written or not
      image_pos += blklen;
      if (!from_stdin && image_pos >= size)
	break;
      // if read had returned 0, we're at EOF and should quit
      if (readlen == 0)
	break;
      blklen = 0;
      reqlen = imgblklen;
    }
    r = throttle->wait_for_ret();
    if (r < 0) {
      goto close;
    }

    if (from_stdin) {
      r = image.resize(image_pos);
      if (r < 0) {
	std::cerr << "rbd: final image resize failed" << std::endl;
	goto close;
      }
    }
  }

close:
  r = image.close();
err:
  if (r < 0)
    rbd.remove(io_ctx, imgname);
done:
  if (r < 0)
    pc.fail();
  else
    pc.finish();
  if (!from_stdin)
    close(fd);
done2:
  delete[] p;
  return r;
}

void get_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_path_options(positional, options,
                       "import file (or '-' for stdin)");
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_DEST);
  at::add_create_image_options(options, true);
  at::add_no_progress_option(options);
  options->add_options()
    ("import-format", po::value<int>(), "format of the file to be imported");

  // TODO legacy rbd allowed import to accept both 'image'/'dest' and
  //      'pool'/'dest-pool'
  at::add_pool_option(options, at::ARGUMENT_MODIFIER_NONE, " (deprecated)");
  at::add_image_option(options, at::ARGUMENT_MODIFIER_NONE, " (deprecated)");
}

int execute(const po::variables_map &vm) {
  std::string path;
  int r = utils::get_path(vm, utils::get_positional_argument(vm, 0), &path);
  if (r < 0) {
    return r;
  }

  // odd check to support legacy / deprecated behavior of import
  std::string deprecated_pool_name;
  if (vm.count(at::POOL_NAME)) {
    deprecated_pool_name = vm[at::POOL_NAME].as<std::string>();
    std::cerr << "rbd: --pool is deprecated for import, use --dest-pool"
              << std::endl;
  }

  std::string deprecated_image_name;
  if (vm.count(at::IMAGE_NAME)) {
    utils::extract_spec(vm[at::IMAGE_NAME].as<std::string>(),
                        &deprecated_pool_name, &deprecated_image_name, nullptr,
                        utils::SPEC_VALIDATION_FULL);
    std::cerr << "rbd: --image is deprecated for import, use --dest"
              << std::endl;
  } else {
    deprecated_image_name = path.substr(path.find_last_of("/") + 1);
  }

  size_t arg_index = 1;
  std::string pool_name = deprecated_pool_name;
  std::string image_name;
  std::string snap_name;
  r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_DEST, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_FULL,
    false);
  if (r < 0) {
    return r;
  }

  if (image_name.empty()) {
    image_name = deprecated_image_name;
  }

  librbd::ImageOptions opts;
  r = utils::get_image_options(vm, true, &opts);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  int format = 1;
  if (vm.count("import-format"))
    format = vm["import-format"].as<int>();

  librbd::RBD rbd;
  r = do_import(rbd, io_ctx, image_name.c_str(), path.c_str(),
                opts, vm[at::NO_PROGRESS].as<bool>(), format);
  if (r < 0) {
    std::cerr << "rbd: import failed: " << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

Shell::Action action(
  {"import"}, {}, "Import image from file.", at::get_long_features_help(),
  &get_arguments, &execute);

} // namespace import
} // namespace action
} // namespace rbd
