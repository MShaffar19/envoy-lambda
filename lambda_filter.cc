#include "lambda_filter.h"

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include "envoy/http/header_map.h"

#include "common/common/empty_string.h"
#include "common/common/hex.h"
#include "common/common/utility.h"
#include "common/http/filter_utility.h"

#include "server/config/network/http_connection_manager.h"

#include "solo_filter_utility.h"

namespace Envoy {
namespace Http {

LambdaFilter::LambdaFilter(LambdaFilterConfigSharedPtr config,
                           FunctionRetrieverSharedPtr functionRetriever,
                           ClusterManager &cm)
    : config_(config), functionRetriever_(functionRetriever), cm_(cm),
      active_(false), awsAuthenticator_(awsAccess(), awsSecret()) {}

LambdaFilter::~LambdaFilter() {}

void LambdaFilter::onDestroy() {}

std::string LambdaFilter::functionUrlPath() {

  std::stringstream val;
  val << "/2015-03-31/functions/" << currentFunction_.func_name_
      << "/invocations";
  return val.str();
}

Envoy::Http::FilterHeadersStatus
LambdaFilter::decodeHeaders(Envoy::Http::HeaderMap &headers, bool end_stream) {

  const Envoy::Router::RouteEntry *routeEntry =
      SoloFilterUtility::resolveRouteEntry(decoder_callbacks_);
  Upstream::ClusterInfoConstSharedPtr info =
      FilterUtility::resolveClusterInfo(decoder_callbacks_, cm_);
  if (routeEntry == nullptr || info == nullptr) {
    return Envoy::Http::FilterHeadersStatus::Continue;
  }

  auto optionalFunction = functionRetriever_->getFunction(*routeEntry, *info);
  if (!optionalFunction.valid()) {
    return Envoy::Http::FilterHeadersStatus::Continue;
  }

  active_ = true;
  currentFunction_ = std::move(optionalFunction.value());

  headers.insertMethod().value().setReference(
      Envoy::Http::Headers::get().MethodValues.Post);

  //  headers.removeContentLength();
  headers.insertPath().value(functionUrlPath());
  request_headers_ = &headers;

  ENVOY_LOG(debug, "decodeHeaders called end = {}", end_stream);

  return Envoy::Http::FilterHeadersStatus::StopIteration;
}

Envoy::Http::FilterDataStatus
LambdaFilter::decodeData(Envoy::Buffer::Instance &data, bool end_stream) {

  if (!active_) {
    return Envoy::Http::FilterDataStatus::Continue;
  }
  // calc hash of data
  ENVOY_LOG(debug, "decodeData called end = {} data = {}", end_stream,
            data.length());

  awsAuthenticator_.updatePayloadHash(data);

  if (end_stream) {

    lambdafy();
    // Authorization: AWS4-HMAC-SHA256
    // Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request,
    // SignedHeaders=content-type;host;x-amz-date,
    // Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7
    // add header ?!
    // get stream id
    return Envoy::Http::FilterDataStatus::Continue;
  }

  return Envoy::Http::FilterDataStatus::StopIterationAndBuffer;
}

void LambdaFilter::lambdafy() {
  std::list<Envoy::Http::LowerCaseString> headers;

  headers.push_back(Envoy::Http::LowerCaseString("x-amz-invocation-type"));
  request_headers_->addCopy(
      Envoy::Http::LowerCaseString("x-amz-invocation-type"),
      std::string("RequestResponse"));

  //  headers.push_back(Envoy::Http::LowerCaseString("x-amz-client-context"));
  //  request_headers_->addCopy(Envoy::Http::LowerCaseString("x-amz-client-context"),
  //  std::string(""));

  headers.push_back(Envoy::Http::LowerCaseString("x-amz-log-type"));
  request_headers_->addCopy(Envoy::Http::LowerCaseString("x-amz-log-type"),
                            std::string("None"));

  headers.push_back(Envoy::Http::LowerCaseString("host"));
  request_headers_->insertHost().value(currentFunction_.hostname_);

  headers.push_back(Envoy::Http::LowerCaseString("content-type"));

  awsAuthenticator_.sign(request_headers_, std::move(headers),
                         currentFunction_.region_);
  request_headers_ = nullptr;
  active_ = false;
}

Envoy::Http::FilterTrailersStatus
LambdaFilter::decodeTrailers(Envoy::Http::HeaderMap &) {
  if (active_) {
    lambdafy();
  }

  return Envoy::Http::FilterTrailersStatus::Continue;
}

void LambdaFilter::setDecoderFilterCallbacks(
    Envoy::Http::StreamDecoderFilterCallbacks &callbacks) {
  decoder_callbacks_ = &callbacks;
}

} // namespace Http
} // namespace Envoy
