#include "host_pool.hpp"
#include <numeric>
#include <algorithm>

namespace khttpd::framework::client
{
  HostPool::HostPool(std::vector<HostEntry> hosts)
    : hosts_(std::move(hosts))
    , total_weight_(0)
    , rng_(std::random_device{}())
  {
    for (const auto& h : hosts_)
    {
      urls_.push_back(h.url);
      total_weight_ += std::max(1, h.weight);
      cumulative_weights_.push_back(total_weight_);
    }
  }

  const std::string& HostPool::pick()
  {
    if (cumulative_weights_.empty())
    {
      static const std::string empty;
      return empty;
    }
    if (cumulative_weights_.size() == 1)
    {
      return urls_[0];
    }

    std::uniform_int_distribution<int> dist(1, total_weight_);
    std::lock_guard<std::mutex> lock{rng_mutex_};
    int r = dist(rng_);

    auto it = std::lower_bound(cumulative_weights_.begin(), cumulative_weights_.end(), r);
    size_t idx = std::distance(cumulative_weights_.begin(), it);
    return urls_[idx];
  }

  const std::vector<std::string>& HostPool::all_urls() const
  {
    return urls_;
  }

  int HostPool::total_weight() const
  {
    return total_weight_;
  }
}
