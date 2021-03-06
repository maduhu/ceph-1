// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MDSFINDINO_H
#define CEPH_MDSFINDINO_H

#include "msg/Message.h"
#include "include/filepath.h"

struct MMDSFindIno : public Message {
  tid_t tid;
  inodeno_t ino;

  MMDSFindIno() : Message(MSG_MDS_FINDINO) {}
  MMDSFindIno(tid_t t, inodeno_t i) : Message(MSG_MDS_FINDINO), tid(t), ino(i) {}

  const char *get_type_name() { return "findino"; }
  void print(ostream &out) {
    out << "findino(" << tid << " " << ino << ")";
  }

  void encode_payload(CephContext *cct) {
    ::encode(tid, payload);
    ::encode(ino, payload);
  }
  void decode_payload(CephContext *cct) {
    bufferlist::iterator p = payload.begin();
    ::decode(tid, p);
    ::decode(ino, p);
  }
};

#endif
