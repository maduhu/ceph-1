// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_MGETPOOLSTATSREPLY_H
#define CEPH_MGETPOOLSTATSREPLY_H

class MGetPoolStatsReply : public PaxosServiceMessage {
public:
  ceph_fsid_t fsid;
  map<string,pool_stat_t> pool_stats;

  MGetPoolStatsReply() : PaxosServiceMessage(MSG_GETPOOLSTATSREPLY, 0) {}
  MGetPoolStatsReply(ceph_fsid_t& f, tid_t t, version_t v) :
    PaxosServiceMessage(MSG_GETPOOLSTATSREPLY, v),
    fsid(f) {
    set_tid(t);
  }

private:
  ~MGetPoolStatsReply() {}

public:
  const char *get_type_name() { return "getpoolstats"; }
  void print(ostream& out) {
    out << "getpoolstatsreply(" << get_tid() << " v" << version <<  ")";
  }

  void encode_payload(CephContext *cct) {
    paxos_encode();
    ::encode(fsid, payload);
    ::encode(pool_stats, payload);
  }
  void decode_payload(CephContext *cct) {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(fsid, p);
    ::decode(pool_stats, p);
  }
};

#endif
