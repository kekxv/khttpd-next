#ifndef KHTTPD_FRAMEWORK_ROUTER_OPENAPI_SCHEMA_HPP_
#define KHTTPD_FRAMEWORK_ROUTER_OPENAPI_SCHEMA_HPP_

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/mp11.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace khttpd::framework
{
  // Specialize this trait for a described DTO to add OpenAPI descriptions to its fields.
  template <class T>
  struct OpenApiFieldDocumentation
  {
    static std::string_view description(std::string_view) { return {}; }
  };
}

namespace khttpd::framework::detail
{
  template <class T>
  struct is_optional : std::false_type {};

  template <class T>
  struct is_optional<std::optional<T>> : std::true_type
  {
    using value_type = T;
  };

  template <class T>
  inline constexpr bool is_optional_v = is_optional<T>::value;

  template <class T>
  struct is_vector : std::false_type {};

  template <class T, class Allocator>
  struct is_vector<std::vector<T, Allocator>> : std::true_type
  {
    using value_type = T;
  };

  template <class T>
  boost::json::value openapi_schema()
  {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    boost::json::object schema;

    if constexpr (is_optional_v<Value>)
    {
      return openapi_schema<typename is_optional<Value>::value_type>();
    }
    else if constexpr (std::is_same_v<Value, bool>)
    {
      schema.emplace("type", "boolean");
    }
    else if constexpr (std::is_integral_v<Value>)
    {
      schema.emplace("type", "integer");
    }
    else if constexpr (std::is_floating_point_v<Value>)
    {
      schema.emplace("type", "number");
    }
    else if constexpr (std::is_same_v<Value, std::string> || std::is_enum_v<Value>)
    {
      schema.emplace("type", "string");
    }
    else if constexpr (is_vector<Value>::value)
    {
      schema.emplace("type", "array");
      schema.emplace("items", openapi_schema<typename is_vector<Value>::value_type>());
    }
    else if constexpr (boost::describe::has_describe_members<Value>::value)
    {
      boost::json::object properties;
      boost::json::array required;
      using Members = boost::describe::describe_members<Value, boost::describe::mod_public>;
      boost::mp11::mp_for_each<Members>([&](auto descriptor)
      {
        using Member = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<Value>().*descriptor.pointer)>>;
        auto property_schema = openapi_schema<Member>().as_object();
        const auto description = OpenApiFieldDocumentation<Value>::description(descriptor.name);
        if (!description.empty()) property_schema.emplace("description", description);
        properties.emplace(descriptor.name, std::move(property_schema));
        if constexpr (!is_optional_v<Member>) required.emplace_back(descriptor.name);
      });
      schema.emplace("type", "object");
      schema.emplace("properties", std::move(properties));
      if (!required.empty()) schema.emplace("required", std::move(required));
    }
    else
    {
      // C++17 cannot reflect arbitrary tag_invoke converters. They remain valid typed
      // routes, but their generated schema is intentionally conservative.
      schema.emplace("type", "object");
      schema.emplace("properties", boost::json::object{});
    }

    return schema;
  }
}

#endif // KHTTPD_FRAMEWORK_ROUTER_OPENAPI_SCHEMA_HPP_
