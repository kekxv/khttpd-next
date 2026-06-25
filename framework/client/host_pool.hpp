#ifndef KHTTPD_FRAMEWORK_CLIENT_HOST_POOL_HPP
#define KHTTPD_FRAMEWORK_CLIENT_HOST_POOL_HPP

#include <string>
#include <vector>
#include <random>
#include <mutex>

namespace khttpd::framework::client
{
  struct HostEntry
  {
    std::string url;
    int weight;
  };

  // Manages multiple base URLs with weighted random selection.
  class HostPool
  {
  public:
    explicit HostPool(std::vector<HostEntry> hosts);

    // Pick one host URL based on weight (weighted random).
    const std::string& pick();

    // All unique host URLs.
    const std::vector<std::string>& all_urls() const;

    // Total weight sum.
    int total_weight() const;

  private:
    std::vector<HostEntry> hosts_;
    std::vector<std::string> urls_;
    std::vector<int> cumulative_weights_;
    int total_weight_;
    std::mt19937 rng_;
    std::mutex rng_mutex_;
  };
}

#endif
