//
// Created by Caesar on 2025/6/4.
//

#ifndef DI_CONTAINER_HPP
#define DI_CONTAINER_HPP
#include <iostream>
#include <memory>        // For std::shared_ptr
#include <typeindex>     // For std::type_index
#include <map>           // For std::map
#include <set>           // For std::set
#include <functional>    // For std::function
#include <stdexcept>     // For std::runtime_error
#include <string>        // For typeid(T).name()
#include <mutex>         // For std::mutex

namespace khttpd
{
  namespace framework
  {
    class ComponentBase
    {
    public:
      virtual ~ComponentBase() = default;
    };

    class DI_Container
    {
    public:
      static DI_Container& instance()
      {
        static DI_Container container;
        return container;
      }

      template <typename T, typename... Args>
      void register_component()
      {
        const auto type_idx = std::type_index(typeid(T));

        std::unique_lock<std::mutex> lock(mtx_);
        if (component_factories_.count(type_idx))
        {
          std::cerr << "Warning: Component " << typeid(T).name() << " already registered. Overwriting." << std::endl;
        }

        auto factory = [](const DI_Container& container) -> std::shared_ptr<void>
        {
          return std::make_shared<T>(container.resolve<Args>()...);
        };

        component_factories_[type_idx] = factory;
      }

      template <typename T>
      std::shared_ptr<T> resolve() const
      {
        const auto type_idx = std::type_index(typeid(T));

        std::unique_lock<std::mutex> lock(mtx_);

        // Check singleton cache
        if (const auto it_singleton = singletons_.find(type_idx); it_singleton != singletons_.end())
        {
          return std::static_pointer_cast<T>(it_singleton->second);
        }

        // Circular dependency detection
        if (resolving_.count(type_idx))
        {
          throw std::runtime_error("Circular dependency detected while resolving: " + std::string(typeid(T).name()));
        }

        // Find factory
        const auto it_factory = component_factories_.find(type_idx);
        if (it_factory == component_factories_.end())
        {
          throw std::runtime_error("Component not registered or dependency missing: " + std::string(typeid(T).name()));
        }

        // Mark as resolving
        resolving_.insert(type_idx);

        try
        {
          // Temporarily release lock during factory execution to avoid deadlocks
          // if factories call resolve() recursively (for different types)
          lock.unlock();

          std::shared_ptr<T> instance = std::static_pointer_cast<T>(it_factory->second(*this));

          lock.lock();
          resolving_.erase(type_idx);
          singletons_[type_idx] = instance;
          return instance;
        }
        catch (...)
        {
          // Clean up resolving set on exception
          lock.lock();
          resolving_.erase(type_idx);
          throw;
        }
      }

      void clear()
      {
        std::unique_lock<std::mutex> lock(mtx_);
        component_factories_.clear();
        singletons_.clear();
        resolving_.clear();
      }

      DI_Container(const DI_Container&) = delete;
      DI_Container& operator=(const DI_Container&) = delete;

    private:
      DI_Container() = default;

      mutable std::mutex mtx_;
      std::map<std::type_index, std::function<std::shared_ptr<void>(const DI_Container&)>> component_factories_;
      mutable std::map<std::type_index, std::shared_ptr<void>> singletons_;
      mutable std::set<std::type_index> resolving_;
    };
  }

  using DI_Container = framework::DI_Container;
}

#endif //DI_CONTAINER_HPP
