#ifndef CEPH_RGW_OS_AUTH_H
#define CEPH_RGW_OS_AUTH_H

#include "rgw_op.h"

#define RGW_OS_TOKEN_EXPIRATION (15 * 60)

extern int rgw_os_verify_signed_token(const char *token, RGWUserInfo& info);

class RGW_OS_Auth_Get : public RGWOp {
public:
  RGW_OS_Auth_Get() {}
  ~RGW_OS_Auth_Get() {}

  int verify_permission() { return 0; }
  void execute();
};

class RGWHandler_OS_Auth : public RGWHandler {
public:
  RGWHandler_OS_Auth() {}
  ~RGWHandler_OS_Auth() {}
  RGWOp *get_op();
  void put_op(RGWOp *op);

  int authorize();
  int read_permissions() { return 0; }
};

#endif
