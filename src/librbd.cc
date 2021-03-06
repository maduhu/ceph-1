// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2.1, as published by the Free Software
 * Foundation.	See file COPYING.
 *
 */

#include "common/Cond.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/rbd/librbd.hpp"

#include <errno.h>
#include <inttypes.h>

#define DOUT_SUBSYS rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd: "

namespace librbd {

  using ceph::bufferlist;
  using librados::snap_t;
  using librados::IoCtx;
  using librados::Rados;

  class WatchCtx;

  struct SnapInfo {
    snap_t id;
    uint64_t size;
    SnapInfo(snap_t _id, uint64_t _size) : id(_id), size(_size) {};
  };

  struct ImageCtx {
    CephContext *cct;
    struct rbd_obj_header_ondisk header;
    ::SnapContext snapc;
    vector<snap_t> snaps;
    std::map<std::string, struct SnapInfo> snaps_by_name;
    uint64_t snapid;
    std::string name;
    std::string snapname;
    IoCtx data_ctx, md_ctx;
    WatchCtx *wctx;
    bool needs_refresh;
    Mutex refresh_lock;
    Mutex lock; // protects access to snapshot and header information

    ImageCtx(std::string imgname, IoCtx& p) : cct(p.cct()), snapid(CEPH_NOSNAP),
					      name(imgname),
					      needs_refresh(true),
					      refresh_lock("librbd::ImageCtx::refresh_lock"),
					      lock("librbd::ImageCtx::lock") {
      md_ctx.dup(p);
      data_ctx.dup(p);
    }

    ~ImageCtx() {
    }

    int snap_set(std::string snap_name)
    {
      std::map<std::string, struct SnapInfo>::iterator it = snaps_by_name.find(snap_name);
      if (it != snaps_by_name.end()) {
	snapname = snap_name;
	snapid = it->second.id;
	return 0;
      }
      snap_unset();
      return -ENOENT;
    }

    void snap_unset()
    {
      snapid = CEPH_NOSNAP;
      snapname = "";
    }

    snap_t get_snapid(std::string snap_name) const
    {
      std::map<std::string, struct SnapInfo>::const_iterator it = snaps_by_name.find(snap_name);
      if (it != snaps_by_name.end())
	return it->second.id;
      return CEPH_NOSNAP;
    }

    void add_snap(std::string snap_name, snap_t id, uint64_t size)
    {
      snapc.snaps.push_back(id);
      snaps.push_back(id);
      struct SnapInfo info(id, size);
      snaps_by_name.insert(std::pair<std::string, struct SnapInfo>(snap_name, info));
    }

    const string md_oid() const
    {
      return name + RBD_SUFFIX;
    }
  };

  class WatchCtx : public librados::WatchCtx {
    ImageCtx *ictx;
    bool valid;
    Mutex lock;
  public:
    uint64_t cookie;
    WatchCtx(ImageCtx *ctx) : ictx(ctx),
			      valid(true),
			      lock("librbd::WatchCtx") {}
    virtual ~WatchCtx() {}
    void invalidate();
    virtual void notify(uint8_t opcode, uint64_t ver, bufferlist& bl);
  };

  struct AioCompletion;

  struct AioBlockCompletion {
    CephContext *cct;
    struct AioCompletion *completion;
    uint64_t ofs;
    size_t len;
    char *buf;
    map<uint64_t,uint64_t> m;
    bufferlist data_bl;

    AioBlockCompletion(CephContext *cct_, AioCompletion *aio_completion, uint64_t _ofs, size_t _len, char *_buf) :
                                            cct(cct_), completion(aio_completion), ofs(_ofs), len(_len), buf(_buf) {}
    void complete(ssize_t r);
  };

  struct AioCompletion {
    Mutex lock;
    Cond cond;
    bool done;
    ssize_t rval;
    callback_t complete_cb;
    void *complete_arg;
    rbd_completion_t rbd_comp;
    int pending_count;
    int ref;
    bool released;

    AioCompletion() : lock("AioCompletion::lock", true),
		      done(false), rval(0), complete_cb(NULL), complete_arg(NULL),
		      rbd_comp(NULL), pending_count(1), ref(1), released(false) {
    }
    ~AioCompletion() {
    }

    int wait_for_complete() {
      lock.Lock();
      while (!done)
	cond.Wait(lock);
      lock.Unlock();
      return 0;
    }

    void add_block_completion(AioBlockCompletion *aio_completion) {
      lock.Lock();
      pending_count++;
      lock.Unlock();
      get();
    }

    void finish_adding_completions() {
      lock.Lock();
      assert(pending_count);
      int count = --pending_count;
      if (!count) {
	complete();
      }
      lock.Unlock();
    }

    void complete() {
      assert(lock.is_locked());
      if (complete_cb) {
	complete_cb(rbd_comp, complete_arg);
      }
      done = true;
      cond.Signal();
    }

    void set_complete_cb(void *cb_arg, callback_t cb) {
      complete_cb = cb;
      complete_arg = cb_arg;
    }

    void complete_block(AioBlockCompletion *block_completion, ssize_t r);

    ssize_t get_return_value() {
      lock.Lock();
      ssize_t r = rval;
      lock.Unlock();
      return r;
    }

    void get() {
      lock.Lock();
      assert(ref > 0);
      ref++;
      lock.Unlock();
    }
    void release() {
      lock.Lock();
      assert(!released);
      released = true;
      put_unlock();
    }
    void put() {
      lock.Lock();
      put_unlock();
    }
    void put_unlock() {
      assert(ref > 0);
      int n = --ref;
      lock.Unlock();
      if (!n)
	delete this;
    }
  };

  void rados_cb(rados_completion_t cb, void *arg);
  void rados_aio_sparse_read_cb(rados_completion_t cb, void *arg);

  int snap_set(ImageCtx *ictx, const char *snap_name);
  int list(IoCtx& io_ctx, std::vector<string>& names);
  int create(IoCtx& io_ctx, const char *imgname, uint64_t size, int *order);
  int rename(IoCtx& io_ctx, const char *srcname, const char *dstname);
  int info(ImageCtx *ictx, image_info_t& info, size_t image_size);
  int remove(IoCtx& io_ctx, const char *imgname);
  int resize(ImageCtx *ictx, uint64_t size);
  int snap_create(ImageCtx *ictx, const char *snap_name);
  int snap_list(ImageCtx *ictx, std::vector<snap_info_t>& snaps);
  int snap_rollback(ImageCtx *ictx, const char *snap_name);
  int snap_remove(ImageCtx *ictx, const char *snap_name);
  int add_snap(ImageCtx *ictx, const char *snap_name);
  int rm_snap(ImageCtx *ictx, const char *snap_name);
  int ictx_check(ImageCtx *ictx);
  int ictx_refresh(ImageCtx *ictx, const char *snap_name);
  int copy(IoCtx& src_md_ctx, const char *srcname, IoCtx& dest_md_ctx, const char *destname);

  int open_image(IoCtx& io_ctx, ImageCtx *ictx, const char *name, const char *snap_name);
  void close_image(ImageCtx *ictx);

  void trim_image(IoCtx& io_ctx, const rbd_obj_header_ondisk &header, uint64_t newsize);
  int read_rbd_info(IoCtx& io_ctx, const string& info_oid, struct rbd_info *info);

  int touch_rbd_info(IoCtx& io_ctx, const string& info_oid);
  int rbd_assign_bid(IoCtx& io_ctx, const string& info_oid, uint64_t *id);
  int read_header_bl(IoCtx& io_ctx, const string& md_oid, bufferlist& header, uint64_t *ver);
  int notify_change(IoCtx& io_ctx, const string& oid, uint64_t *pver, ImageCtx *ictx);
  int read_header(IoCtx& io_ctx, const string& md_oid, struct rbd_obj_header_ondisk *header, uint64_t *ver);
  int write_header(IoCtx& io_ctx, const string& md_oid, bufferlist& header);
  int tmap_set(IoCtx& io_ctx, const string& imgname);
  int tmap_rm(IoCtx& io_ctx, const string& imgname);
  int rollback_image(ImageCtx *ictx, uint64_t snapid);
  void image_info(const rbd_obj_header_ondisk& header, image_info_t& info, size_t info_size);
  string get_block_oid(const rbd_obj_header_ondisk &header, uint64_t num);
  uint64_t get_max_block(const rbd_obj_header_ondisk &header);
  uint64_t get_block_size(const rbd_obj_header_ondisk &header);
  uint64_t get_block_num(const rbd_obj_header_ondisk &header, uint64_t ofs);
  uint64_t get_block_ofs(const rbd_obj_header_ondisk &header, uint64_t ofs);
  int check_io(ImageCtx *ictx, uint64_t off, uint64_t len);
  int init_rbd_info(struct rbd_info *info);
  void init_rbd_header(struct rbd_obj_header_ondisk& ondisk,
			      uint64_t size, int *order, uint64_t bid);

  int64_t read_iterate(ImageCtx *ictx, uint64_t off, size_t len,
		       int (*cb)(uint64_t, size_t, const char *, void *),
		       void *arg);
  ssize_t read(ImageCtx *ictx, uint64_t off, size_t len, char *buf);
  ssize_t write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf);
  int aio_write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf,
                AioCompletion *c);
  int aio_read(ImageCtx *ictx, uint64_t off, size_t len,
               char *buf, AioCompletion *c);
  ssize_t handle_sparse_read(CephContext *cct,
			     bufferlist data_bl,
			     uint64_t block_ofs,
			     const map<uint64_t, uint64_t> &data_map,
			     uint64_t buf_ofs,
			     size_t buf_len,
			     int (*cb)(uint64_t, size_t, const char *, void *),
			     void *arg);

  AioCompletion *aio_create_completion() {
    AioCompletion *c= new AioCompletion();
    return c;
  }
  AioCompletion *aio_create_completion(void *cb_arg, callback_t cb_complete) {
    AioCompletion *c = new AioCompletion();
    c->set_complete_cb(cb_arg, cb_complete);
    return c;
  }

void WatchCtx::invalidate()
{
  Mutex::Locker l(lock);
  valid = false;
}

void WatchCtx::notify(uint8_t opcode, uint64_t ver, bufferlist& bl)
{
  Mutex::Locker l(lock);
  ldout(ictx->cct, 1) <<  " got notification opcode=" << (int)opcode << " ver=" << ver << " cookie=" << cookie << dendl;
  if (valid) {
    Mutex::Locker lictx(ictx->refresh_lock);
    ictx->needs_refresh = true;
  }
}

void init_rbd_header(struct rbd_obj_header_ondisk& ondisk,
					uint64_t size, int *order, uint64_t bid)
{
  uint32_t hi = bid >> 32;
  uint32_t lo = bid & 0xFFFFFFFF;
  memset(&ondisk, 0, sizeof(ondisk));

  memcpy(&ondisk.text, RBD_HEADER_TEXT, sizeof(RBD_HEADER_TEXT));
  memcpy(&ondisk.signature, RBD_HEADER_SIGNATURE, sizeof(RBD_HEADER_SIGNATURE));
  memcpy(&ondisk.version, RBD_HEADER_VERSION, sizeof(RBD_HEADER_VERSION));

  snprintf(ondisk.block_name, sizeof(ondisk.block_name), "rb.%x.%x", hi, lo);

  if (!*order)
    *order = RBD_DEFAULT_OBJ_ORDER;

  ondisk.image_size = size;
  ondisk.options.order = *order;
  ondisk.options.crypt_type = RBD_CRYPT_NONE;
  ondisk.options.comp_type = RBD_COMP_NONE;
  ondisk.snap_seq = 0;
  ondisk.snap_count = 0;
  ondisk.reserved = 0;
  ondisk.snap_names_len = 0;
}

void image_info(const rbd_obj_header_ondisk& header, image_info_t& info, size_t infosize)
{
  int obj_order = header.options.order;
  info.size = header.image_size;
  info.obj_size = 1 << obj_order;
  info.num_objs = header.image_size >> obj_order;
  info.order = obj_order;
  memcpy(&info.block_name_prefix, &header.block_name, RBD_MAX_BLOCK_NAME_SIZE);
  info.parent_pool = -1;
  bzero(&info.parent_name, RBD_MAX_IMAGE_NAME_SIZE);
}

string get_block_oid(const rbd_obj_header_ondisk &header, uint64_t num)
{
  char o[RBD_MAX_BLOCK_NAME_SIZE];
  snprintf(o, RBD_MAX_BLOCK_NAME_SIZE,
       "%s.%012" PRIx64, header.block_name, num);
  return o;
}

uint64_t get_max_block(const rbd_obj_header_ondisk &header)
{
  uint64_t size = header.image_size;
  int obj_order = header.options.order;
  uint64_t block_size = 1 << obj_order;
  uint64_t numseg = (size + block_size - 1) >> obj_order;

  return numseg;
}

uint64_t get_block_ofs(const rbd_obj_header_ondisk &header, uint64_t ofs)
{
  int obj_order = header.options.order;
  uint64_t block_size = 1 << obj_order;
  return ofs & (block_size - 1);
}

uint64_t get_block_size(const rbd_obj_header_ondisk &header)
{
  return 1 << header.options.order;
}

uint64_t get_block_num(const rbd_obj_header_ondisk &header, uint64_t ofs)
{
  int obj_order = header.options.order;
  uint64_t num = ofs >> obj_order;

  return num;
}

int init_rbd_info(struct rbd_info *info)
{
  memset(info, 0, sizeof(*info));
  return 0;
}

void trim_image(IoCtx& io_ctx, const rbd_obj_header_ondisk &header, uint64_t newsize)
{
  CephContext *cct = io_ctx.cct();
  uint64_t numseg = get_max_block(header);
  uint64_t start = get_block_num(header, newsize);
  ldout(cct, 2) << "trimming image data from " << numseg << " to " << start << " objects..." << dendl;
  for (uint64_t i=start; i<numseg; i++) {
    string oid = get_block_oid(header, i);
    io_ctx.remove(oid);
    if ((i & 127) == 0) {
      ldout(cct, 2) << "\t" << i << "/" << numseg << dendl;
    }
  }
}

int read_rbd_info(IoCtx& io_ctx, const string& info_oid, struct rbd_info *info)
{
  int r;
  bufferlist bl;
  r = io_ctx.read(info_oid, bl, sizeof(*info), 0);
  if (r < 0)
    return r;
  if (r == 0) {
    return init_rbd_info(info);
  }

  if (r < (int)sizeof(*info))
    return -EIO;

  memcpy(info, bl.c_str(), r);
  return 0;
}

int touch_rbd_info(IoCtx& io_ctx, const string& info_oid)
{
  bufferlist bl;
  int r = io_ctx.write(info_oid, bl, 0, 0);
  if (r < 0)
    return r;
  return 0;
}

int rbd_assign_bid(IoCtx& io_ctx, const string& info_oid, uint64_t *id)
{
  bufferlist bl, out;
  *id = 0;

  int r = touch_rbd_info(io_ctx, info_oid);
  if (r < 0)
    return r;

  r = io_ctx.exec(info_oid, "rbd", "assign_bid", bl, out);
  if (r < 0)
    return r;

  bufferlist::iterator iter = out.begin();
  ::decode(*id, iter);

  return 0;
}


int read_header_bl(IoCtx& io_ctx, const string& md_oid, bufferlist& header, uint64_t *ver)
{
  int r;
#define READ_SIZE 4096
  do {
    bufferlist bl;
    r = io_ctx.read(md_oid, bl, READ_SIZE, 0);
    if (r < 0)
      return r;
    header.claim_append(bl);
   } while (r == READ_SIZE);

  if (ver)
    *ver = io_ctx.get_last_version();

  return 0;
}

int notify_change(IoCtx& io_ctx, const string& oid, uint64_t *pver, ImageCtx *ictx)
{
  uint64_t ver;

  if (ictx) {
    assert(ictx->lock.is_locked());
    ictx->refresh_lock.Lock();
    ictx->needs_refresh = true;
    ictx->refresh_lock.Unlock();
  }

  if (pver)
    ver = *pver;
  else
    ver = io_ctx.get_last_version();
  bufferlist bl;
  io_ctx.notify(oid, ver, bl);
  return 0;
}

int read_header(IoCtx& io_ctx, const string& md_oid, struct rbd_obj_header_ondisk *header, uint64_t *ver)
{
  bufferlist header_bl;
  int r = read_header_bl(io_ctx, md_oid, header_bl, ver);
  if (r < 0)
    return r;
  if (header_bl.length() < (int)sizeof(*header))
    return -EIO;
  memcpy(header, header_bl.c_str(), sizeof(*header));

  return 0;
}

int write_header(IoCtx& io_ctx, const string& md_oid, bufferlist& header)
{
  bufferlist bl;
  int r = io_ctx.write(md_oid, header, header.length(), 0);

  notify_change(io_ctx, md_oid, NULL, NULL);

  return r;
}

int tmap_set(IoCtx& io_ctx, const string& imgname)
{
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_SET;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  ::encode(emptybl, cmdbl);
  return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
}

int tmap_rm(IoCtx& io_ctx, const string& imgname)
{
  bufferlist cmdbl;
  __u8 c = CEPH_OSD_TMAP_RM;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
}

int rollback_image(ImageCtx *ictx, uint64_t snapid)
{
  assert(ictx->lock.is_locked());
  uint64_t numseg = get_max_block(ictx->header);

  for (uint64_t i = 0; i < numseg; i++) {
    int r;
    string oid = get_block_oid(ictx->header, i);
    r = ictx->data_ctx.selfmanaged_snap_rollback(oid, snapid);
    ldout(ictx->cct, 10) << "selfmanaged_snap_rollback on " << oid << " to " << snapid << " returned " << r << dendl;
    if (r < 0 && r != -ENOENT)
      return r;
  }
  return 0;
}

int list(IoCtx& io_ctx, std::vector<std::string>& names)
{
  CephContext *cct = io_ctx.cct();
  ldout(cct, 20) << "list " << &io_ctx << dendl;

  bufferlist bl;
  int r = io_ctx.read(RBD_DIRECTORY, bl, 0, 0);
  if (r < 0)
    return r;

  bufferlist::iterator p = bl.begin();
  bufferlist header;
  map<string,bufferlist> m;
  ::decode(header, p);
  ::decode(m, p);
  for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); q++)
    names.push_back(q->first);
  return 0;
}

int snap_create(ImageCtx *ictx, const char *snap_name)
{
  ldout(ictx->cct, 20) << "snap_create " << ictx << " " << snap_name << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  r = add_snap(ictx, snap_name);

  if (r < 0)
    return r;

  notify_change(ictx->md_ctx, ictx->md_oid(), NULL, ictx);

  return 0;
}

int snap_remove(ImageCtx *ictx, const char *snap_name)
{
  ldout(ictx->cct, 20) << "snap_remove " << ictx << " " << snap_name << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  snap_t snapid = ictx->get_snapid(snap_name);
  if (snapid == CEPH_NOSNAP)
    return -ENOENT;

  r = rm_snap(ictx, snap_name);
  if (r < 0)
    return r;

  r = ictx->data_ctx.selfmanaged_snap_remove(snapid);

  if (r < 0)
    return r;

  notify_change(ictx->md_ctx, ictx->md_oid(), NULL, ictx);

  return 0;
}

int create(IoCtx& io_ctx, const char *imgname, uint64_t size, int *order)
{
  CephContext *cct = io_ctx.cct();
  ldout(cct, 20) << "create " << &io_ctx << " name = " << imgname << " size = " << size << dendl;

  string md_oid = imgname;
  md_oid += RBD_SUFFIX;

  // make sure it doesn't already exist
  int r = io_ctx.stat(md_oid, NULL, NULL);
  if (r == 0) {
    lderr(cct) << "rbd image header " << md_oid << " already exists" << dendl;
    return -EEXIST;
  }

  uint64_t bid;
  string dir_info = RBD_INFO;
  r = rbd_assign_bid(io_ctx, dir_info, &bid);
  if (r < 0) {
    lderr(cct) << "failed to assign a block name for image" << dendl;
    return r;
  }

  struct rbd_obj_header_ondisk header;
  init_rbd_header(header, size, order, bid);

  bufferlist bl;
  bl.append((const char *)&header, sizeof(header));

  ldout(cct, 2) << "adding rbd image to directory..." << dendl;
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_SET;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  ::encode(emptybl, cmdbl);
  r = io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  if (r < 0) {
    lderr(cct) << "error adding img to directory: " << strerror(-r)<< dendl;
    return r;
  }

  ldout(cct, 2) << "creating rbd image..." << dendl;
  r = io_ctx.write(md_oid, bl, bl.length(), 0);
  if (r < 0) {
    lderr(cct) << "error writing header: " << strerror(-r) << dendl;
    return r;
  }

  ldout(cct, 2) << "done." << dendl;
  return 0;
}

int rename(IoCtx& io_ctx, const char *srcname, const char *dstname)
{
  CephContext *cct = io_ctx.cct();
  ldout(cct, 20) << "rename " << &io_ctx << " " << srcname << " -> " << dstname << dendl;

  string md_oid = srcname;
  md_oid += RBD_SUFFIX;
  string dst_md_oid = dstname;
  dst_md_oid += RBD_SUFFIX;
  string dstname_str = dstname;
  string imgname_str = srcname;
  uint64_t ver;
  bufferlist header;
  int r = read_header_bl(io_ctx, md_oid, header, &ver);
  if (r < 0) {
    lderr(cct) << "error reading header: " << md_oid << ": " << strerror(-r) << dendl;
    return r;
  }
  r = io_ctx.stat(dst_md_oid, NULL, NULL);
  if (r == 0) {
    lderr(cct) << "rbd image header " << dst_md_oid << " already exists" << dendl;
    return -EEXIST;
  }
  r = write_header(io_ctx, dst_md_oid, header);
  if (r < 0) {
    lderr(cct) << "error writing header: " << dst_md_oid << ": " << strerror(-r) << dendl;
    return r;
  }
  r = tmap_set(io_ctx, dstname_str);
  if (r < 0) {
    io_ctx.remove(dst_md_oid);
    lderr(cct) << "can't add " << dst_md_oid << " to directory" << dendl;
    return r;
  }
  r = tmap_rm(io_ctx, imgname_str);
  if (r < 0)
    lderr(cct) << "warning: couldn't remove old entry from directory (" << imgname_str << ")" << dendl;

  r = io_ctx.remove(md_oid);
  if (r < 0 && r != -ENOENT)
    lderr(cct) << "warning: couldn't remove old metadata" << dendl;
  notify_change(io_ctx, md_oid, NULL, NULL);

  return 0;
}


int info(ImageCtx *ictx, image_info_t& info, size_t infosize)
{
  ldout(ictx->cct, 20) << "info " << ictx << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  image_info(ictx->header, info, infosize);
  return 0;
}

int remove(IoCtx& io_ctx, const char *imgname)
{
  CephContext *cct(io_ctx.cct());
  ldout(cct, 20) << "remove " << &io_ctx << " " << imgname << dendl;

  string md_oid = imgname;
  md_oid += RBD_SUFFIX;

  struct rbd_obj_header_ondisk header;
  int r = read_header(io_ctx, md_oid, &header, NULL);
  if (r >= 0) {
    trim_image(io_ctx, header, 0);
    ldout(cct, 2) << "removing header..." << dendl;
    io_ctx.remove(md_oid);
  }

  ldout(cct, 2) << "removing rbd image to directory..." << dendl;
  bufferlist cmdbl;
  __u8 c = CEPH_OSD_TMAP_RM;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  r = io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  if (r < 0) {
    lderr(cct) << "error removing img from directory: " << strerror(-r)<< dendl;
    return r;
  }

  ldout(cct, 2) << "done." << dendl;
  return 0;
}

int resize(ImageCtx *ictx, uint64_t size)
{
  CephContext *cct = ictx->cct;
  ldout(cct, 20) << "resize " << ictx << " " << ictx->header.image_size << " -> " << size << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  // trim
  if (size == ictx->header.image_size) {
    ldout(cct, 2) << "no change in size (" << size << " -> " << ictx->header.image_size << ")" << dendl;
    return 0;
  }

  if (size > ictx->header.image_size) {
    ldout(cct, 2) << "expanding image " << size << " -> " << ictx->header.image_size << " objects" << dendl;
    ictx->header.image_size = size;
  } else {
    ldout(cct, 2) << "shrinking image " << size << " -> " << ictx->header.image_size << " objects" << dendl;
    trim_image(ictx->data_ctx, ictx->header, size);
    ictx->header.image_size = size;
  }

  // rewrite header
  bufferlist bl;
  bl.append((const char *)&(ictx->header), sizeof(ictx->header));
  r = ictx->md_ctx.write(ictx->md_oid(), bl, bl.length(), 0);

  if (r == -ERANGE)
    lderr(cct) << "operation might have conflicted with another client!" << dendl;
  if (r < 0) {
    lderr(cct) << "error writing header: " << strerror(-r) << dendl;
    return r;
  } else {
    notify_change(ictx->md_ctx, ictx->md_oid(), NULL, ictx);
  }

  ldout(cct, 2) << "done." << dendl;

  return 0;
}

int snap_list(ImageCtx *ictx, std::vector<snap_info_t>& snaps)
{
  ldout(ictx->cct, 20) << "snap_list " << ictx << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;
  bufferlist bl, bl2;

  Mutex::Locker l(ictx->lock);
  for (std::map<std::string, struct SnapInfo>::iterator it = ictx->snaps_by_name.begin();
       it != ictx->snaps_by_name.end(); ++it) {
    snap_info_t info;
    info.name = it->first;
    info.id = it->second.id;
    info.size = it->second.size;
    snaps.push_back(info);
  }

  return 0;
}

int add_snap(ImageCtx *ictx, const char *snap_name)
{
  assert(ictx->lock.is_locked());

  bufferlist bl, bl2;
  uint64_t snap_id;

  int r = ictx->md_ctx.selfmanaged_snap_create(&snap_id);
  if (r < 0) {
    lderr(ictx->cct) << "failed to create snap id: " << strerror(-r) << dendl;
    return r;
  }

  ::encode(snap_name, bl);
  ::encode(snap_id, bl);

  r = ictx->md_ctx.exec(ictx->md_oid(), "rbd", "snap_add", bl, bl2);
  if (r < 0) {
    lderr(ictx->cct) << "rbd.snap_add execution failed failed: " << strerror(-r) << dendl;
    return r;
  }
  notify_change(ictx->md_ctx, ictx->md_oid(), NULL, ictx);

  return 0;
}

int rm_snap(ImageCtx *ictx, const char *snap_name)
{
  assert(ictx->lock.is_locked());

  bufferlist bl, bl2;
  ::encode(snap_name, bl);

  int r = ictx->md_ctx.exec(ictx->md_oid(), "rbd", "snap_remove", bl, bl2);
  if (r < 0) {
    lderr(ictx->cct) << "rbd.snap_remove execution failed: " << strerror(-r) << dendl;
    return r;
  }

  return 0;
}

int ictx_check(ImageCtx *ictx)
{
  CephContext *cct = ictx->cct;
  ldout(cct, 20) << "ictx_check " << ictx << dendl;
  ictx->refresh_lock.Lock();
  bool needs_refresh = ictx->needs_refresh;
  ictx->refresh_lock.Unlock();

  if (needs_refresh) {
    Mutex::Locker l(ictx->lock);
    const char *snap = NULL;
    if (ictx->snapid != CEPH_NOSNAP)
      snap = ictx->snapname.c_str();

    int r = ictx_refresh(ictx, snap);
    if (r < 0) {
      lderr(cct) << "Error re-reading rbd header: " << cpp_strerror(-r) << dendl;
      return r;
    }

    // check if the snapshot at which we were reading was removed
    if (snap && ictx->snapname != snap) {
      lderr(cct) << "tried to read from a snapshot that no longer exists: " << snap << dendl;
      return -ENOENT;
    }
  }
  return 0;
}

int ictx_refresh(ImageCtx *ictx, const char *snap_name)
{
  CephContext *cct = ictx->cct;
  assert(ictx->lock.is_locked());
  bufferlist bl, bl2;

  if (snap_name) {
    ldout(cct, 20) << "ictx_refresh " << ictx << " snap = " << snap_name << dendl;
  } else {
    ldout(cct, 20) << "ictx_refresh " << ictx << " no snap" << dendl;
  }

  int r = read_header(ictx->md_ctx, ictx->md_oid(), &(ictx->header), NULL);
  if (r < 0) {
    lderr(cct) << "Error reading header: " << cpp_strerror(-r) << dendl;
    return r;
  }
  r = ictx->md_ctx.exec(ictx->md_oid(), "rbd", "snap_list", bl, bl2);
  if (r < 0) {
    lderr(cct) << "Error listing snapshots: " << cpp_strerror(-r) << dendl;
    return r;
  }

  ictx->snaps.clear();
  ictx->snapc.snaps.clear();
  ictx->snaps_by_name.clear();

  uint32_t num_snaps;
  bufferlist::iterator iter = bl2.begin();
  ::decode(ictx->snapc.seq, iter);
  ::decode(num_snaps, iter);
  for (uint32_t i=0; i < num_snaps; i++) {
    uint64_t id, image_size;
    string s;
    ::decode(id, iter);
    ::decode(image_size, iter);
    ::decode(s, iter);
    ictx->add_snap(s, id, image_size);
  }

  if (!ictx->snapc.is_valid()) {
    lderr(cct) << "image snap context is invalid!" << dendl;
    return -EIO;
  }

  if (snap_name) {
    r = ictx->snap_set(snap_name);
    if (r < 0) {
      lderr(cct) << "could not set snap to " << snap_name << ": " << cpp_strerror(-r) << dendl;
      return r;
    }
    ictx->data_ctx.snap_set_read(ictx->snapid);
  }

  ictx->data_ctx.selfmanaged_snap_set_write_ctx(ictx->snapc.seq, ictx->snaps);

  ictx->refresh_lock.Lock();
  ictx->needs_refresh = false;
  ictx->refresh_lock.Unlock();

  return 0;
}

int snap_rollback(ImageCtx *ictx, const char *snap_name)
{
  CephContext *cct = ictx->cct;
  ldout(cct, 20) << "snap_rollback " << ictx << " snap = " << snap_name << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  snap_t snapid = ictx->get_snapid(snap_name);
  if (snapid == CEPH_NOSNAP) {
    lderr(cct) << "No such snapshot found." << dendl;
    return -ENOENT;
  }

  r = rollback_image(ictx, snapid);
  if (r < 0) {
    lderr(cct) << "Error rolling back image: " << cpp_strerror(-r) << dendl;
    return r;
  }

  // refresh without setting the snapid we read from
  ictx_refresh(ictx, NULL);
  snap_t new_snapid = ictx->get_snapid(snap_name);
  ldout(ictx->cct, 20) << "snapid is " << ictx->snapid << " new snapid is " << new_snapid << dendl;

  notify_change(ictx->md_ctx, ictx->md_oid(), NULL, ictx);

  return 0;
}

int copy(IoCtx& src_md_ctx, const char *srcname, IoCtx& dest_md_ctx, const char *destname)
{
  CephContext *cct = src_md_ctx.cct();
  struct rbd_obj_header_ondisk header, dest_header;
  int64_t ret;
  int r;
  IoCtx src_data_ctx(src_md_ctx);
  IoCtx dest_data_ctx(dest_md_ctx);
  string md_oid, dest_md_oid;
  md_oid = srcname;
  md_oid += RBD_SUFFIX;

  dest_md_oid = destname;
  dest_md_oid += RBD_SUFFIX;

  ret = read_header(src_md_ctx, md_oid, &header, NULL);
  if (ret < 0)
    return ret;

  uint64_t numseg = get_max_block(header);
  uint64_t block_size = get_block_size(header);
  int order = header.options.order;

  r = create(dest_md_ctx, destname, header.image_size, &order);
  if (r < 0) {
    lderr(cct) << "header creation failed" << dendl;
    return r;
  }

  ret = read_header(dest_md_ctx, dest_md_oid, &dest_header, NULL);
  if (ret < 0) {
    lderr(cct) << "failed to read newly created header" << dendl;
    return ret;
  }

  for (uint64_t i = 0; i < numseg; i++) {
    bufferlist bl;
    string oid = get_block_oid(header, i);
    string dest_oid = get_block_oid(dest_header, i);
    map<uint64_t, uint64_t> m;
    map<uint64_t, uint64_t>::iterator iter;
    r = src_data_ctx.sparse_read(oid, m, bl, block_size, 0);
    if (r < 0 && r == -ENOENT)
      r = 0;
    if (r < 0)
      return r;


    for (iter = m.begin(); iter != m.end(); ++iter) {
      uint64_t extent_ofs = iter->first;
      size_t extent_len = iter->second;
      bufferlist wrbl;
      if (extent_ofs + extent_len > bl.length()) {
	lderr(cct) << "data error!" << dendl;
	return -EIO;
      }
      bl.copy(extent_ofs, extent_len, wrbl);
      r = dest_data_ctx.write(dest_oid, wrbl, extent_len, extent_ofs);
      if (r < 0)
	goto done;
    }
  }
  r = 0;

done:
  return r;
}

int snap_set(ImageCtx *ictx, const char *snap_name)
{
  ldout(ictx->cct, 20) << "snap_set " << ictx << " snap = " << (snap_name ? snap_name : "NULL") << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  Mutex::Locker l(ictx->lock);
  if (snap_name)
    ictx->snap_set(snap_name);
  else
    ictx->snap_unset();

  ictx->data_ctx.snap_set_read(ictx->snapid);

  return 0;
}

int open_image(IoCtx& io_ctx, ImageCtx *ictx, const char *name, const char *snap_name)
{
  CephContext *cct = io_ctx.cct();
  string sn = snap_name ? snap_name : "NULL";
  ldout(cct, 20) << "open_image " << &io_ctx << " ictx =  " << ictx
	   << " name =  " << name << " snap_name = " << (snap_name ? snap_name : "NULL") << dendl;

  ictx->lock.Lock();
  int r = ictx_refresh(ictx, snap_name);
  ictx->lock.Unlock();
  if (r < 0)
    return r;

  WatchCtx *wctx = new WatchCtx(ictx);
  if (!wctx)
    return -ENOMEM;
  ictx->wctx = wctx;

  r = ictx->md_ctx.watch(ictx->md_oid(), 0, &(wctx->cookie), wctx);
  return r;
}

void close_image(ImageCtx *ictx)
{
  ldout(ictx->cct, 20) << "close_image " << ictx << dendl;
  ictx->lock.Lock();
  ictx->wctx->invalidate();
  ictx->md_ctx.unwatch(ictx->md_oid(), ictx->wctx->cookie);
  delete ictx->wctx;
  ictx->lock.Unlock();
  delete ictx;
}

int64_t read_iterate(ImageCtx *ictx, uint64_t off, size_t len,
		     int (*cb)(uint64_t, size_t, const char *, void *),
		     void *arg)
{
  ldout(ictx->cct, 20) << "read_iterate " << ictx << " off = " << off << " len = " << len << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  r = check_io(ictx, off, len);
  if (r < 0)
    return r;

  int64_t ret;
  int64_t total_read = 0;
  ictx->lock.Lock();
  uint64_t start_block = get_block_num(ictx->header, off);
  uint64_t end_block = get_block_num(ictx->header, off + len);
  uint64_t block_size = get_block_size(ictx->header);
  ictx->lock.Unlock();
  uint64_t left = len;

  for (uint64_t i = start_block; i <= end_block; i++) {
    bufferlist bl;
    ictx->lock.Lock();
    string oid = get_block_oid(ictx->header, i);
    uint64_t block_ofs = get_block_ofs(ictx->header, off + total_read);
    ictx->lock.Unlock();
    uint64_t read_len = min(block_size - block_ofs, left);

    map<uint64_t, uint64_t> m;
    r = ictx->data_ctx.sparse_read(oid, m, bl, read_len, block_ofs);
    if (r < 0 && r == -ENOENT)
      r = 0;
    if (r < 0) {
      return r;
    }

    r = handle_sparse_read(ictx->cct, bl, block_ofs, m, total_read, read_len, cb, arg);
    if (r < 0) {
      return r;
    }

    total_read += r;
    left -= r;
  }
  ret = total_read;

  return ret;
}

static int simple_read_cb(uint64_t ofs, size_t len, const char *buf, void *arg)
{
  char *dest_buf = (char *)arg;
  if (buf)
    memcpy(dest_buf + ofs, buf, len);
  else
    memset(dest_buf + ofs, 0, len);

  return 0;
}


ssize_t read(ImageCtx *ictx, uint64_t ofs, size_t len, char *buf)
{
  return read_iterate(ictx, ofs, len, simple_read_cb, buf);
}

ssize_t write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf)
{
  ldout(ictx->cct, 20) << "write " << ictx << " off = " << off << " len = " << len << dendl;

  if (!len)
    return 0;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  r = check_io(ictx, off, len);
  if (r < 0)
    return r;

  size_t total_write = 0;
  ictx->lock.Lock();
  uint64_t start_block = get_block_num(ictx->header, off);
  uint64_t end_block = get_block_num(ictx->header, off + len - 1);
  uint64_t block_size = get_block_size(ictx->header);
  ictx->lock.Unlock();
  uint64_t left = len;

  for (uint64_t i = start_block; i <= end_block; i++) {
    bufferlist bl;
    ictx->lock.Lock();
    string oid = get_block_oid(ictx->header, i);
    uint64_t block_ofs = get_block_ofs(ictx->header, off + total_write);
    ictx->lock.Unlock();
    uint64_t write_len = min(block_size - block_ofs, left);
    bl.append(buf + total_write, write_len);
    r = ictx->data_ctx.write(oid, bl, write_len, block_ofs);
    if (r < 0)
      return r;
    if ((uint64_t)r != write_len)
      return -EIO;
    total_write += write_len;
    left -= write_len;
  }
  return total_write;
}

ssize_t handle_sparse_read(CephContext *cct,
			   bufferlist data_bl,
			   uint64_t block_ofs,
			   const map<uint64_t, uint64_t> &data_map,
			   uint64_t buf_ofs,
			   size_t buf_len,
			   int (*cb)(uint64_t, size_t, const char *, void *),
			   void *arg)
{
  int r;
  uint64_t bl_ofs = 0;

  for (map<uint64_t, uint64_t>::const_iterator iter = data_map.begin();
       iter != data_map.end();
       ++iter) {
    uint64_t extent_ofs = iter->first;
    size_t extent_len = iter->second;

    ldout(cct, 10) << "extent_ofs=" << extent_ofs
		   << " extent_len=" << extent_len << dendl;
    ldout(cct, 10) << "block_ofs=" << block_ofs << dendl;

    /* a hole? */
    if (extent_ofs - block_ofs > 0) {
      ldout(cct, 10) << "<1>zeroing " << buf_ofs << "~" << extent_ofs << dendl;
      r = cb(buf_ofs, extent_ofs - block_ofs, NULL, arg);
      if (r < 0) {
	return r;
      }
    }

    if (bl_ofs + extent_len > buf_len) {
      return -EIO;
    }
    buf_ofs += extent_ofs - block_ofs;
    block_ofs = extent_ofs;

    /* data */
    ldout(cct, 10) << "<2>copying " << buf_ofs << "~" << extent_len
		   << " from ofs=" << bl_ofs << dendl;
    r = cb(buf_ofs, extent_len, data_bl.c_str() + bl_ofs, arg);
    if (r < 0) {
      return r;
    }
    bl_ofs += extent_len;
    buf_ofs += extent_len;
    block_ofs += extent_len;
  }

  /* last hole */
  if (buf_len - buf_ofs) {
    ldout(cct, 10) << "<3>zeroing " << buf_ofs << "~" << buf_len - buf_ofs << dendl;
    r = cb(buf_ofs, buf_len - buf_ofs, NULL, arg);
    if (r < 0) {
      return r;
    }
  }

  return buf_len;
}

void AioBlockCompletion::complete(ssize_t r)
{
  ldout(cct, 10) << "AioBlockCompletion::complete()" << dendl;
  if ((r >= 0 || r == -ENOENT) && buf) { // this was a sparse_read operation
    ldout(cct, 10) << "ofs=" << ofs << " len=" << len << dendl;
    r = handle_sparse_read(cct, data_bl, ofs, m, 0, len, simple_read_cb, buf);
  }
  completion->complete_block(this, r);
}

void AioCompletion::complete_block(AioBlockCompletion *block_completion, ssize_t r)
{
  CephContext *cct = block_completion->cct;
  ldout(cct, 20) << "AioCompletion::complete_block() this=" 
	         << (void *)this << " complete_cb=" << (void *)complete_cb << dendl;
  lock.Lock();
  if (rval >= 0) {
    if (r < 0 && r != -EEXIST)
      rval = r;
    else if (r > 0)
      rval += r;
  }
  assert(pending_count);
  int count = --pending_count;
  if (!count) {
    complete();
  }
  put_unlock();
}

void rados_cb(rados_completion_t c, void *arg)
{
  AioBlockCompletion *block_completion = (AioBlockCompletion *)arg;
  block_completion->complete(rados_aio_get_return_value(c));
  delete block_completion;
}

int check_io(ImageCtx *ictx, uint64_t off, uint64_t len)
{
  ictx->lock.Lock();
  uint64_t image_size = ictx->header.image_size;
  ictx->lock.Unlock();

  if ((uint64_t)(off + len) > image_size)
    return -EINVAL;
  return 0;
}

int aio_write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf,
			         AioCompletion *c)
{
  CephContext *cct = ictx->cct;
  ldout(cct, 20) << "aio_write " << ictx << " off = " << off << " len = " << len << dendl;

  if (!len)
    return 0;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  size_t total_write = 0;
  ictx->lock.Lock();
  uint64_t start_block = get_block_num(ictx->header, off);
  uint64_t end_block = get_block_num(ictx->header, off + len - 1);
  uint64_t block_size = get_block_size(ictx->header);
  ictx->lock.Unlock();
  uint64_t left = len;

  r = check_io(ictx, off, len);
  if (r < 0)
    return r;

  c->get();
  for (uint64_t i = start_block; i <= end_block; i++) {
    bufferlist bl;
    ictx->lock.Lock();
    string oid = get_block_oid(ictx->header, i);
    uint64_t block_ofs = get_block_ofs(ictx->header, off + total_write);
    ictx->lock.Unlock();
    uint64_t write_len = min(block_size - block_ofs, left);
    bl.append(buf + total_write, write_len);
    AioBlockCompletion *block_completion = new AioBlockCompletion(cct, c, off, len, NULL);
    c->add_block_completion(block_completion);
    librados::AioCompletion *rados_completion =
      Rados::aio_create_completion(block_completion, NULL, rados_cb);
    r = ictx->data_ctx.aio_write(oid, rados_completion, bl, write_len, block_ofs);
    rados_completion->release();
    if (r < 0)
      goto done;
    total_write += write_len;
    left -= write_len;
  }
  r = 0;
done:
  c->finish_adding_completions();
  c->put();
  /* FIXME: cleanup all the allocated stuff */
  return r;
}

void rados_aio_sparse_read_cb(rados_completion_t c, void *arg)
{
  AioBlockCompletion *block_completion = (AioBlockCompletion *)arg;
  block_completion->complete(rados_aio_get_return_value(c));
  delete block_completion;
}

int aio_read(ImageCtx *ictx, uint64_t off, size_t len,
				char *buf,
                                AioCompletion *c)
{
  ldout(ictx->cct, 20) << "aio_read " << ictx << " off = " << off << " len = " << len << dendl;

  int r = ictx_check(ictx);
  if (r < 0)
    return r;

  r = check_io(ictx, off, len);
  if (r < 0)
    return r;

  int64_t ret;
  int total_read = 0;
  ictx->lock.Lock();
  uint64_t start_block = get_block_num(ictx->header, off);
  uint64_t end_block = get_block_num(ictx->header, off + len - 1);
  uint64_t block_size = get_block_size(ictx->header);
  ictx->lock.Unlock();
  uint64_t left = len;

  c->get();
  for (uint64_t i = start_block; i <= end_block; i++) {
    bufferlist bl;
    ictx->lock.Lock();
    string oid = get_block_oid(ictx->header, i);
    uint64_t block_ofs = get_block_ofs(ictx->header, off + total_read);
    ictx->lock.Unlock();
    uint64_t read_len = min(block_size - block_ofs, left);

    map<uint64_t,uint64_t> m;
    map<uint64_t,uint64_t>::iterator iter;

    AioBlockCompletion *block_completion =
	new AioBlockCompletion(ictx->cct, c, block_ofs, read_len, buf + total_read);
    c->add_block_completion(block_completion);

    librados::AioCompletion *rados_completion =
      Rados::aio_create_completion(block_completion, rados_aio_sparse_read_cb, rados_cb);
    r = ictx->data_ctx.aio_sparse_read(oid, rados_completion,
				       &block_completion->m, &block_completion->data_bl,
				       read_len, block_ofs);
    rados_completion->release();
    if (r < 0 && r == -ENOENT)
      r = 0;
    if (r < 0) {
      ret = r;
      goto done;
    }
    total_read += read_len;
    left -= read_len;
  }
  ret = total_read;
done:
  c->finish_adding_completions();
  c->put();
  return ret;
}

/*
   RBD
*/
RBD::RBD()
{
}

RBD::~RBD()
{
}

void RBD::version(int *major, int *minor, int *extra)
{
  rbd_version(major, minor, extra);
}

int RBD::open(IoCtx& io_ctx, Image& image, const char *name)
{
  return open(io_ctx, image, name, NULL);
}

int RBD::open(IoCtx& io_ctx, Image& image, const char *name, const char *snapname)
{
  ImageCtx *ictx = new ImageCtx(name, io_ctx);
  if (!ictx)
    return -ENOMEM;

  int r = librbd::open_image(io_ctx, ictx, name, snapname);
  if (r < 0)
    return r;

  image.ctx = (image_ctx_t) ictx;
  return 0;
}

int RBD::create(IoCtx& io_ctx, const char *name, uint64_t size, int *order)
{
  int r = librbd::create(io_ctx, name, size, order);
  return r;
}

int RBD::remove(IoCtx& io_ctx, const char *name)
{
  int r = librbd::remove(io_ctx, name);
  return r;
}

int RBD::list(IoCtx& io_ctx, std::vector<std::string>& names)
{
  int r = librbd::list(io_ctx, names);
  return r;
}

int RBD::copy(IoCtx& src_io_ctx, const char *srcname, IoCtx& dest_io_ctx, const char *destname)
{
  int r = librbd::copy(src_io_ctx, srcname, dest_io_ctx, destname);
  return r;
}

int RBD::rename(IoCtx& src_io_ctx, const char *srcname, const char *destname)
{
  int r = librbd::rename(src_io_ctx, srcname, destname);
  return r;
}

RBD::AioCompletion::AioCompletion(void *cb_arg, callback_t complete_cb)
{
  librbd::AioCompletion *c = librbd::aio_create_completion(cb_arg, complete_cb);
  pc = (void *)c;
  c->rbd_comp = this;
}

int RBD::AioCompletion::wait_for_complete()
{
  librbd::AioCompletion *c = (librbd::AioCompletion *)pc;
  return c->wait_for_complete();
}

ssize_t RBD::AioCompletion::get_return_value()
{
  librbd::AioCompletion *c = (librbd::AioCompletion *)pc;
  return c->get_return_value();
}

void RBD::AioCompletion::release()
{
  librbd::AioCompletion *c = (librbd::AioCompletion *)pc;
  c->release();
  delete this;
}

/*
  Image
*/

Image::Image() : ctx(NULL)
{
}

Image::~Image()
{
  if (ctx) {
    ImageCtx *ictx = (ImageCtx *)ctx;
    close_image(ictx);
  }
}

int Image::resize(uint64_t size)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  int r = librbd::resize(ictx, size);
  return r;
}

int Image::stat(image_info_t& info, size_t infosize)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  int r = librbd::info(ictx, info, infosize);
  return r;
}


int Image::snap_create(const char *snap_name)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  int r = librbd::snap_create(ictx, snap_name);
  return r;
}

int Image::snap_remove(const char *snap_name)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  int r = librbd::snap_remove(ictx, snap_name);
  return r;
}

int Image::snap_rollback(const char *snap_name)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  int r = librbd::snap_rollback(ictx, snap_name);
  return r;
}

int Image::snap_list(std::vector<librbd::snap_info_t>& snaps)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  return librbd::snap_list(ictx, snaps);
}

int Image::snap_set(const char *snap_name)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  return librbd::snap_set(ictx, snap_name);
}

ssize_t Image::read(uint64_t ofs, size_t len, bufferlist& bl)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  bufferptr ptr(len);
  bl.push_back(ptr);
  return librbd::read(ictx, ofs, len, bl.c_str());
}

int64_t Image::read_iterate(uint64_t ofs, size_t len,
			    int (*cb)(uint64_t, size_t, const char *, void *), void *arg)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  return librbd::read_iterate(ictx, ofs, len, cb, arg);
}

ssize_t Image::write(uint64_t ofs, size_t len, bufferlist& bl)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  if (bl.length() < len)
    return -EINVAL;
  return librbd::write(ictx, ofs, len, bl.c_str());
}

int Image::aio_write(uint64_t off, size_t len, bufferlist& bl, RBD::AioCompletion *c)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  if (bl.length() < len)
    return -EINVAL;
  return librbd::aio_write(ictx, off, len, bl.c_str(), (librbd::AioCompletion *)c->pc);
}

int Image::aio_read(uint64_t off, size_t len, bufferlist& bl, RBD::AioCompletion *c)
{
  ImageCtx *ictx = (ImageCtx *)ctx;
  bufferptr ptr(len);
  bl.push_back(ptr);
  ldout(ictx->cct, 10) << "Image::aio_read() buf=" << (void *)bl.c_str() << "~" << (void *)(bl.c_str() + len - 1) << dendl;
  return librbd::aio_read(ictx, off, len, bl.c_str(), (librbd::AioCompletion *)c->pc);
}

} // namespace librbd

extern "C" void rbd_version(int *major, int *minor, int *extra)
{
  if (major)
    *major = LIBRBD_VER_MAJOR;
  if (minor)
    *minor = LIBRBD_VER_MINOR;
  if (extra)
    *extra = LIBRBD_VER_EXTRA;
}

/* images */
extern "C" int rbd_list(rados_ioctx_t p, char *names, size_t *size)
{
  librados::IoCtx io_ctx;
  librados::IoCtx::from_rados_ioctx_t(p, io_ctx);
  std::vector<std::string> cpp_names;
  int r = librbd::list(io_ctx, cpp_names);
  if (r == -ENOENT)
    return 0;

  if (r < 0)
    return r;

  size_t expected_size = 0;

  for (size_t i = 0; i < cpp_names.size(); i++) {
    expected_size += cpp_names[i].size() + 1;
  }
  if (*size < expected_size) {
    *size = expected_size;
    return -ERANGE;
  }

  for (int i = 0; i < (int)cpp_names.size(); i++) {
    strcpy(names, cpp_names[i].c_str());
    names += strlen(names) + 1;
  }
  return (int)cpp_names.size();
}

extern "C" int rbd_create(rados_ioctx_t p, const char *name, uint64_t size, int *order)
{
  librados::IoCtx io_ctx;
  librados::IoCtx::from_rados_ioctx_t(p, io_ctx);
  return librbd::create(io_ctx, name, size, order);
}

extern "C" int rbd_remove(rados_ioctx_t p, const char *name)
{
  librados::IoCtx io_ctx;
  librados::IoCtx::from_rados_ioctx_t(p, io_ctx);
  return librbd::remove(io_ctx, name);
}

extern "C" int rbd_copy(rados_ioctx_t src_p, const char *srcname, rados_ioctx_t dest_p, const char *destname)
{
  librados::IoCtx src_io_ctx, dest_io_ctx;
  librados::IoCtx::from_rados_ioctx_t(src_p, src_io_ctx);
  librados::IoCtx::from_rados_ioctx_t(dest_p, dest_io_ctx);
  return librbd::copy(src_io_ctx, srcname, dest_io_ctx, destname);
}

extern "C" int rbd_rename(rados_ioctx_t src_p, const char *srcname, const char *destname)
{
  librados::IoCtx src_io_ctx;
  librados::IoCtx::from_rados_ioctx_t(src_p, src_io_ctx);
  return librbd::rename(src_io_ctx, srcname, destname);
}

extern "C" int rbd_open(rados_ioctx_t p, const char *name, rbd_image_t *image, const char *snap_name)
{
  librados::IoCtx io_ctx;
  librados::IoCtx::from_rados_ioctx_t(p, io_ctx);
  librbd::ImageCtx *ictx = new librbd::ImageCtx(name, io_ctx);
  if (!ictx)
    return -ENOMEM;
  int r = librbd::open_image(io_ctx, ictx, name, snap_name);
  *image = (rbd_image_t)ictx;
  return r;
}

extern "C" int rbd_close(rbd_image_t image)
{
  librbd::ImageCtx *ctx = (librbd::ImageCtx *)image;
  librbd::close_image(ctx);
  return 0; 
}

extern "C" int rbd_resize(rbd_image_t image, uint64_t size)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::resize(ictx, size);
}

extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info, size_t infosize)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::info(ictx, *info, infosize);
}

/* snapshots */
extern "C" int rbd_snap_create(rbd_image_t image, const char *snap_name)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::snap_create(ictx, snap_name);
}

extern "C" int rbd_snap_remove(rbd_image_t image, const char *snap_name)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::snap_remove(ictx, snap_name);
}

extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snap_name)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::snap_rollback(ictx, snap_name);
}

extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps, int *max_snaps)
{
  std::vector<librbd::snap_info_t> cpp_snaps;
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  int r = librbd::snap_list(ictx, cpp_snaps);
  if (r == -ENOENT)
    return 0;
  if (r < 0)
    return r;
  if (!max_snaps)
    return -EINVAL;
  if (*max_snaps < (int)cpp_snaps.size() + 1) {
    *max_snaps = (int)cpp_snaps.size() + 1;
    return -ERANGE;
  }

  int i;

  for (i = 0; i < (int)cpp_snaps.size(); i++) {
    snaps[i].id = cpp_snaps[i].id;
    snaps[i].size = cpp_snaps[i].size;
    snaps[i].name = strdup(cpp_snaps[i].name.c_str());
    if (!snaps[i].name) {
      for (int j = 0; j < i; j++)
	free((void *)snaps[j].name);
      return -ENOMEM;
    }
  }
  snaps[i].id = 0;
  snaps[i].size = 0;
  snaps[i].name = NULL;

  return (int)cpp_snaps.size();
}

extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps)
{
  while (snaps->name) {
    free((void *)snaps->name);
    snaps++;
  }
}

extern "C" int rbd_snap_set(rbd_image_t image, const char *snapname)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::snap_set(ictx, snapname);
}

/* I/O */
extern "C" ssize_t rbd_read(rbd_image_t image, uint64_t ofs, size_t len, char *buf)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::read(ictx, ofs, len, buf);
}

extern "C" int64_t rbd_read_iterate(rbd_image_t image, uint64_t ofs, size_t len,
				    int (*cb)(uint64_t, size_t, const char *, void *), void *arg)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::read_iterate(ictx, ofs, len, cb, arg);
}

extern "C" ssize_t rbd_write(rbd_image_t image, uint64_t ofs, size_t len, const char *buf)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  return librbd::write(ictx, ofs, len, buf);
}

extern "C" int rbd_aio_create_completion(void *cb_arg, rbd_callback_t complete_cb, rbd_completion_t *c)
{
  librbd::RBD::AioCompletion *rbd_comp = new librbd::RBD::AioCompletion(cb_arg, complete_cb);
  *c = (rbd_completion_t) rbd_comp;
  return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf, rbd_completion_t c)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  return librbd::aio_write(ictx, off, len, buf, (librbd::AioCompletion *)comp->pc);
}

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t off, size_t len, char *buf, rbd_completion_t c)
{
  librbd::ImageCtx *ictx = (librbd::ImageCtx *)image;
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  return librbd::aio_read(ictx, off, len, buf, (librbd::AioCompletion *)comp->pc);
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  return comp->wait_for_complete();
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  return comp->get_return_value();
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
  librbd::RBD::AioCompletion *comp = (librbd::RBD::AioCompletion *)c;
  comp->release();
}
