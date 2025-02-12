// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DIAL_DIAL_UDP_SERVER_H
#define NET_DIAL_DIAL_UDP_SERVER_H

#include <string>

#include "base/compiler_specific.h"
#include "base/gtest_prod_util.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "net/base/net_export.h"
#include "net/udp/udp_listen_socket.h"

namespace net {

class IPEndPoint;
class UdpSocketFactory;
class HttpServerRequestInfo;

class NET_EXPORT DialUdpServer : public UDPListenSocket::Delegate {
 public:
  DialUdpServer();
  virtual ~DialUdpServer();

  bool Start(const std::string& location_url,
             const std::string& server_agent);
  bool Stop();

  // UDPListenSocket::Delegate
  virtual void DidRead(UDPListenSocket* server,
                       const char* data,
                       int len,
                       const IPEndPoint* address) OVERRIDE;

  virtual void DidClose(UDPListenSocket* sock) OVERRIDE;

 private:
  FRIEND_TEST_ALL_PREFIXES(DialUdpServerTest, ParseSearchRequest);

  // Construct the appropriate search response.
  const std::string ConstructSearchResponse() const;

  // Parse a request to make sure it is a M-Search.
  bool ParseSearchRequest(const std::string& request) const;

  // Is the valid SSDP request a valid M-Search request too ?
  bool IsValidMSearchRequest(const HttpServerRequestInfo& info) const;

  scoped_refptr<UDPListenSocket> socket_;
  scoped_ptr<UdpSocketFactory> factory_;

  // Value to pass in LOCATION: header
  std::string location_url_;

  // Value to pass in SERVER: header
  std::string server_agent_;
};

} // namespace net

#endif // NET_DIAL_DIAL_UDP_SERVER_H

