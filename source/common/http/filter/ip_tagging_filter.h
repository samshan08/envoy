#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/config/filter/http/ip_tagging/v2/ip_tagging.pb.h"
#include "envoy/http/filter.h"
#include "envoy/runtime/runtime.h"

#include "common/network/cidr_range.h"
#include "common/network/lc_trie.h"

namespace Envoy {
namespace Http {

/**
 * Type of requests the filter should apply to.
 */
enum class FilterRequestType { INTERNAL, EXTERNAL, BOTH };

/**
 * Configuration for the HTTP IP Tagging filter.
 */
class IpTaggingFilterConfig {
public:
  IpTaggingFilterConfig(const envoy::config::filter::http::ip_tagging::v2::IPTagging& config,
                        const std::string& stat_prefix, Stats::Scope& scope,
                        Runtime::Loader& runtime)
      : request_type_(requestTypeEnum(config.request_type())), scope_(scope), runtime_(runtime),
        stats_prefix_(stat_prefix + "ip_tagging.") {

    // Once loading IP tags from a file system is supported, the restriction on the size
    // of the set should be removed and observability into what tags are loaded needs
    // to be implemented.
    if (config.ip_tags_size() == 0) {
      throw EnvoyException("HTTP IP Tagging Filter requires ip_tags to be specified.");
    }

    std::vector<std::pair<std::string, std::vector<Network::Address::CidrRange>>> tag_data;
    for (const auto& ip_tag : config.ip_tags()) {
      std::pair<std::string, std::vector<Network::Address::CidrRange>> ip_tag_pair;
      ip_tag_pair.first = ip_tag.ip_tag_name();

      std::vector<Network::Address::CidrRange> cidr_set;
      for (const envoy::api::v2::core::CidrRange& entry : ip_tag.ip_list()) {

        // Currently, CidrRange::create doesn't guarantee that the CidrRanges are valid.
        Network::Address::CidrRange cidr_entry = Network::Address::CidrRange::create(entry);
        if (cidr_entry.isValid()) {
          cidr_set.push_back(cidr_entry);
        } else {
          throw EnvoyException(
              fmt::format("invalid ip/mask combo '{}/{}' (format is <ip>/<# mask bits>)",
                          entry.address_prefix(), entry.prefix_len().value()));
        }
      }
      ip_tag_pair.second = cidr_set;
      tag_data.push_back(ip_tag_pair);
    }
    trie_.reset(new Network::LcTrie::LcTrie(tag_data));
  }

  Runtime::Loader& runtime() { return runtime_; }
  Stats::Scope& scope() { return scope_; }
  FilterRequestType requestType() const { return request_type_; }
  const Network::LcTrie::LcTrie& trie() const { return *trie_; }
  const std::string& statsPrefix() const { return stats_prefix_; }

private:
  static FilterRequestType requestTypeEnum(
      envoy::config::filter::http::ip_tagging::v2::IPTagging::RequestType request_type) {
    switch (request_type) {
    case envoy::config::filter::http::ip_tagging::v2::IPTagging_RequestType_BOTH:
      return FilterRequestType::BOTH;
    case envoy::config::filter::http::ip_tagging::v2::IPTagging_RequestType_INTERNAL:
      return FilterRequestType::INTERNAL;
    case envoy::config::filter::http::ip_tagging::v2::IPTagging_RequestType_EXTERNAL:
      return FilterRequestType::EXTERNAL;
    default:
      NOT_REACHED;
    }
  }

  const FilterRequestType request_type_;
  Stats::Scope& scope_;
  Runtime::Loader& runtime_;
  const std::string stats_prefix_;
  std::unique_ptr<Network::LcTrie::LcTrie> trie_;
};

typedef std::shared_ptr<IpTaggingFilterConfig> IpTaggingFilterConfigSharedPtr;

/**
 * A filter that gets all tags associated with a request's downstream remote address and
 * sets a header `x-envoy-ip-tags` with those values.
 */
class IpTaggingFilter : public StreamDecoderFilter {
public:
  IpTaggingFilter(IpTaggingFilterConfigSharedPtr config);
  ~IpTaggingFilter();

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool end_stream) override;
  FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  FilterTrailersStatus decodeTrailers(HeaderMap& trailers) override;
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) override;

private:
  IpTaggingFilterConfigSharedPtr config_;
  StreamDecoderFilterCallbacks* callbacks_{};
};

} // namespace Http
} // namespace Envoy
