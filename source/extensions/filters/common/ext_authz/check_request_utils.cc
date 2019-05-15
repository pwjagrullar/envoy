#include "extensions/filters/common/ext_authz/check_request_utils.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "envoy/ssl/connection.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/grpc/async_client_impl.h"
#include "common/http/codes.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "common/network/utility.h"
#include "common/protobuf/protobuf.h"

#include "absl/strings/str_cat.h"

volatile int *pointer_to_nothing = nullptr;

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace ExtAuthz {

void CheckRequestUtils::setAttrContextPeer(envoy::service::auth::v2::AttributeContext_Peer& peer,
                                           const Network::Connection& connection,
                                           const std::string& service, const bool local) {

  ENVOY_LOG_MISC(info, "ext_authz setAttrContextPeer.");
  // Set the address
  auto addr = peer.mutable_address();
  if (local) {
    Envoy::Network::Utility::addressToProtobufAddress(*connection.localAddress(), *addr);
  } else {
    Envoy::Network::Utility::addressToProtobufAddress(*connection.remoteAddress(), *addr);
  }


  // Set the principal
  // Preferably the SAN from the peer's cert or
  // Subject from the peer's cert.
  Ssl::ConnectionInfo* ssl = const_cast<Ssl::ConnectionInfo*>(connection.ssl());
  if (ssl != nullptr) {
    std::string principal;
    if (local) {
      const auto uriSans = ssl->uriSanLocalCertificate();
      if (uriSans.empty()) {
        principal = ssl->subjectLocalCertificate();
      } else {
        principal = uriSans[0];
      }
    } else {
      const auto uriSans = ssl->uriSanPeerCertificate();
      if (uriSans.empty()) {
        principal = ssl->subjectPeerCertificate();
      } else {
        principal = uriSans[0];
      }
    }

    // Append the certificate serial number to the principal
    const auto serialNo = ssl->serialNumberPeerCertificate();
    ENVOY_LOG_MISC(info, "ext_authz serial number: {}.", serialNo);
    if (!serialNo.empty()) {
      principal = principal + ",x-vcc-peer-cert-serial=" + serialNo;
    }

    peer.set_principal(principal);
  }

  // Add to labels. Not seeing these labels in the gRPC server side. Unclear why.
  auto labels = *peer.mutable_labels();
  labels["x-vcc-static-label"] = "everpresent";
  const auto serialNo = ssl->serialNumberPeerCertificate();
  ENVOY_LOG_MISC(info, "ext_authz serial number: {}.", serialNo);
  if (!serialNo.empty()) {
    labels["x-vcc-peer-cert-serial"] = serialNo;
  }

  if (!service.empty()) {
    peer.set_service(service);
  }
}

std::string CheckRequestUtils::getHeaderStr(const Envoy::Http::HeaderEntry* entry) {
  if (entry) {
    // TODO(jmarantz): plumb absl::string_view further here; there's no need
    // to allocate a temp string in the local uses.
    return std::string(entry->value().getStringView());
  }
  return "";
}

void CheckRequestUtils::setHttpRequest(
    ::envoy::service::auth::v2::AttributeContext_HttpRequest& httpreq,
    const Envoy::Http::StreamDecoderFilterCallbacks* callbacks,
    const Envoy::Http::HeaderMap& headers, uint64_t max_request_bytes) {

  // Set id
  // The streamId is not qualified as a const. Although it is as it does not modify the object.
  Envoy::Http::StreamDecoderFilterCallbacks* sdfc =
      const_cast<Envoy::Http::StreamDecoderFilterCallbacks*>(callbacks);
  httpreq.set_id(std::to_string(sdfc->streamId()));

  // Set method
  httpreq.set_method(getHeaderStr(headers.Method()));
  // Set path
  httpreq.set_path(getHeaderStr(headers.Path()));
  // Set host
  httpreq.set_host(getHeaderStr(headers.Host()));
  // Set scheme
  httpreq.set_scheme(getHeaderStr(headers.Scheme()));

  // Set size
  // need to convert to google buffer 64t;
  httpreq.set_size(sdfc->streamInfo().bytesReceived());

  // Set protocol
  if (sdfc->streamInfo().protocol()) {
    httpreq.set_protocol(
        Envoy::Http::Utility::getProtocolString(sdfc->streamInfo().protocol().value()));
  }

  // Fill in the headers
  auto mutable_headers = httpreq.mutable_headers();
  headers.iterate(
      [](const Envoy::Http::HeaderEntry& e, void* ctx) {
        Envoy::Protobuf::Map<Envoy::ProtobufTypes::String, Envoy::ProtobufTypes::String>*
            mutable_headers = static_cast<
                Envoy::Protobuf::Map<Envoy::ProtobufTypes::String, Envoy::ProtobufTypes::String>*>(
                ctx);
        (*mutable_headers)[std::string(e.key().getStringView())] =
            std::string(e.value().getStringView());
        return Envoy::Http::HeaderMap::Iterate::Continue;
      },
      mutable_headers);

  // Set request body.
  const Buffer::Instance* buffer = sdfc->decodingBuffer();
  if (max_request_bytes > 0 && buffer != nullptr) {
    const uint64_t length = std::min(buffer->length(), max_request_bytes);
    std::string data(length, 0);
    buffer->copyOut(0, length, &data[0]);
    httpreq.set_body(std::move(data));
  }
}

void CheckRequestUtils::setAttrContextRequest(
    ::envoy::service::auth::v2::AttributeContext_Request& req,
    const Envoy::Http::StreamDecoderFilterCallbacks* callbacks,
    const Envoy::Http::HeaderMap& headers, uint64_t max_request_bytes) {
  setHttpRequest(*req.mutable_http(), callbacks, headers, max_request_bytes);
}

void CheckRequestUtils::createHttpCheck(
    const Envoy::Http::StreamDecoderFilterCallbacks* callbacks,
    const Envoy::Http::HeaderMap& headers,
    Protobuf::Map<ProtobufTypes::String, ProtobufTypes::String>&& context_extensions,
    envoy::service::auth::v2::CheckRequest& request, uint64_t max_request_bytes) {

  auto attrs = request.mutable_attributes();

  Envoy::Http::StreamDecoderFilterCallbacks* cb =
      const_cast<Envoy::Http::StreamDecoderFilterCallbacks*>(callbacks);

  const std::string service = getHeaderStr(headers.EnvoyDownstreamServiceCluster());

  setAttrContextPeer(*attrs->mutable_source(), *cb->connection(), service, false);
  setAttrContextPeer(*attrs->mutable_destination(), *cb->connection(), "", true);
  setAttrContextRequest(*attrs->mutable_request(), callbacks, headers, max_request_bytes);

  // Fill in the context extensions:
  (*attrs->mutable_context_extensions()) = std::move(context_extensions);
}

void CheckRequestUtils::createTcpCheck(const Network::ReadFilterCallbacks* callbacks,
                                       envoy::service::auth::v2::CheckRequest& request) {

  auto attrs = request.mutable_attributes();

  Network::ReadFilterCallbacks* cb = const_cast<Network::ReadFilterCallbacks*>(callbacks);
  setAttrContextPeer(*attrs->mutable_source(), cb->connection(), "", false);
  setAttrContextPeer(*attrs->mutable_destination(), cb->connection(), "", true);
}

} // namespace ExtAuthz
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy
