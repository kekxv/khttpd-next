#include "router/openapi.hpp"

#include "router/http_router.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace khttpd::framework
{
  namespace
  {
    struct DocumentedPath
    {
      std::string path;
      std::vector<std::string> parameters;
    };

    DocumentedPath document_path(const std::string& route_path)
    {
      static const std::regex parameter_pattern(":([a-zA-Z_][a-zA-Z0-9_]*)");
      DocumentedPath result;
      auto current = route_path.cbegin();
      const std::sregex_iterator end;
      for (auto it = std::sregex_iterator(route_path.cbegin(), route_path.cend(), parameter_pattern);
           it != end; ++it)
      {
        result.path.append(current, it->prefix().second);
        result.path += "{" + (*it)[1].str() + "}";
        result.parameters.push_back((*it)[1].str());
        current = it->suffix().first;
      }
      result.path.append(current, route_path.cend());
      return result;
    }

    std::string method_name(const boost::beast::http::verb method)
    {
      std::string result(boost::beast::http::to_string(method));
      std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      return result;
    }

    boost::json::object make_operation(const RouteDescriptor& descriptor,
                                       const std::vector<std::string>& path_parameters)
    {
      boost::json::object operation;
      if (!descriptor.documentation.summary.empty())
        operation.emplace("summary", descriptor.documentation.summary);
      if (!descriptor.documentation.description.empty())
        operation.emplace("description", descriptor.documentation.description);
      if (!path_parameters.empty())
      {
        boost::json::array parameters;
        for (std::size_t index = 0; index < path_parameters.size(); ++index)
        {
          boost::json::object parameter;
          parameter.emplace("name", path_parameters[index]);
          parameter.emplace("in", "path");
          parameter.emplace("required", true);
          parameter.emplace("schema", boost::json::object{{"type", "string"}});
          if (index + 1 == path_parameters.size()) parameter.emplace("x-khttpd-greedy", true);
          parameters.emplace_back(std::move(parameter));
        }
        operation.emplace("parameters", std::move(parameters));
      }

      if (descriptor.request_schema)
      {
        boost::json::object media_type;
        media_type.emplace("schema", *descriptor.request_schema);
        boost::json::object content;
        content.emplace("application/json", std::move(media_type));
        boost::json::object request_body;
        request_body.emplace("required", true);
        request_body.emplace("content", std::move(content));
        operation.emplace("requestBody", std::move(request_body));
      }

      boost::json::object response;
      response.emplace("description", "Successful response");
      if (descriptor.response_schema)
      {
        boost::json::object media_type;
        media_type.emplace("schema", *descriptor.response_schema);
        boost::json::object content;
        content.emplace("application/json", std::move(media_type));
        response.emplace("content", std::move(content));
      }
      boost::json::object responses;
      responses.emplace("200", std::move(response));
      operation.emplace("responses", std::move(responses));
      return operation;
    }

    void validate_documentation_path(const std::string& path, const char* name)
    {
      const auto literal_character = [](const unsigned char character)
      {
        return std::isalnum(character) != 0 || character == '/' || character == '.' ||
          character == '_' || character == '-' || character == '~';
      };
      if (path.empty() || path.front() != '/' ||
          !std::all_of(path.begin(), path.end(), literal_character))
        throw std::invalid_argument(std::string(name) + " must be an absolute literal HTTP path");
    }

    std::string escape_html(const std::string& input)
    {
      std::string escaped;
      escaped.reserve(input.size());
      for (const char character : input)
      {
        switch (character)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '\"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += character; break;
        }
      }
      return escaped;
    }

    std::string uppercase(const std::string& input)
    {
      std::string result(input);
      std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character)
      {
        return static_cast<char>(std::toupper(character));
      });
      return result;
    }

    std::string shell_single_quote(const std::string& input)
    {
      std::string result = "'";
      for (const char character : input)
      {
        if (character == '\'') result += "'\\''";
        else result += character;
      }
      return result + "'";
    }

    std::string operation_anchor(const std::string& method, const std::string& path)
    {
      std::string result(method);
      bool last_was_separator = false;
      for (const unsigned char character : path)
      {
        if (std::isalnum(character) != 0)
        {
          result += static_cast<char>(std::tolower(character));
          last_was_separator = false;
        }
        else if (!last_was_separator)
        {
          result += '-';
          last_was_separator = true;
        }
      }
      while (!result.empty() && result.back() == '-') result.pop_back();
      return result;
    }

    std::string method_class(const std::string& method)
    {
      if (method == "get" || method == "post" || method == "put" || method == "patch" ||
          method == "delete" || method == "options" || method == "head")
        return method;
      return "other";
    }

    bool schema_property_is_required(const boost::json::object& schema,
                                     const boost::json::string_view property_name)
    {
      const auto* required = schema.if_contains("required");
      if (required == nullptr || !required->is_array()) return false;
      return std::any_of(required->as_array().begin(), required->as_array().end(),
        [property_name](const boost::json::value& value)
        {
          return value.is_string() && value.as_string() == property_name;
        });
    }

    std::string schema_type_name(const boost::json::object& schema)
    {
      const auto* type = schema.if_contains("type");
      if (type != nullptr && type->is_string()) return std::string(type->as_string());
      if (schema.contains("properties")) return "object";
      if (schema.contains("items")) return "array";
      return "value";
    }

    std::string render_schema_view(const boost::json::value& schema_value,
                                   const bool include_raw = true,
                                   const std::size_t depth = 0)
    {
      if (!schema_value.is_object() || depth > 8)
        return "<pre class=\"schema\"><code>" +
          escape_html(boost::json::serialize(schema_value)) + "</code></pre>";

      const auto& schema = schema_value.as_object();
      const auto type = schema_type_name(schema);
      std::string result = "<div class=\"schema-view\"><div class=\"schema-summary\">"
        "<span class=\"schema-type\">" + escape_html(type) + "</span>";
      const auto* format = schema.if_contains("format");
      if (format != nullptr && format->is_string())
        result += "<span class=\"schema-format\">" + escape_html(std::string(format->as_string())) + "</span>";
      result += "</div>";

      const auto* properties = schema.if_contains("properties");
      if (type == "object" && properties != nullptr && properties->is_object())
      {
        if (properties->as_object().empty())
        {
          result += "<p class=\"empty schema-empty\">Object fields are not reflected.</p>";
        }
        else
        {
          result += "<div class=\"schema-fields\">";
          for (const auto& property : properties->as_object())
          {
            const auto required = schema_property_is_required(schema, property.key());
            result += "<div class=\"schema-property\"><div class=\"property-head\"><code class=\"property-name\">" +
              escape_html(std::string(property.key())) + "</code>";
            if (property.value().is_object())
              result += "<span class=\"property-type\">" +
                escape_html(schema_type_name(property.value().as_object())) + "</span>";
            if (required) result += "<span class=\"required\">required</span>";
            result += "</div>";
            if (property.value().is_object())
            {
              const auto& property_schema = property.value().as_object();
              const auto* description = property_schema.if_contains("description");
              if (description != nullptr && description->is_string())
                result += "<p class=\"property-description\">" +
                  escape_html(std::string(description->as_string())) + "</p>";
              if (property_schema.contains("properties"))
                result += render_schema_view(property.value(), false, depth + 1);
              const auto* items = property_schema.if_contains("items");
              if (items != nullptr)
                result += "<div class=\"array-items\"><span>Items</span>" +
                  render_schema_view(*items, false, depth + 1) + "</div>";
            }
            result += "</div>";
          }
          result += "</div>";
        }
      }
      else
      {
        const auto* items = schema.if_contains("items");
        if (type == "array" && items != nullptr)
          result += "<div class=\"array-items\"><span>Items</span>" +
            render_schema_view(*items, false, depth + 1) + "</div>";
      }

      if (include_raw)
        result += "<details class=\"schema-raw\"><summary>JSON Schema</summary><pre><code>" +
          escape_html(boost::json::serialize(schema_value)) + "</code></pre></details>";
      return result + "</div>";
    }

    boost::json::value schema_example(const boost::json::value& schema_value,
                                      const std::size_t depth = 0)
    {
      if (!schema_value.is_object() || depth > 8) return nullptr;
      const auto& schema = schema_value.as_object();
      const auto* example = schema.if_contains("example");
      if (example != nullptr) return *example;
      const auto* default_value = schema.if_contains("default");
      if (default_value != nullptr) return *default_value;
      const auto* enum_values = schema.if_contains("enum");
      if (enum_values != nullptr && enum_values->is_array() && !enum_values->as_array().empty())
        return enum_values->as_array().front();

      const auto type = schema_type_name(schema);
      if (type == "object")
      {
        boost::json::object result;
        const auto* properties = schema.if_contains("properties");
        if (properties != nullptr && properties->is_object())
          for (const auto& property : properties->as_object())
            result.emplace(property.key(), schema_example(property.value(), depth + 1));
        return result;
      }
      if (type == "array") return boost::json::array{};
      if (type == "string") return "";
      if (type == "integer" || type == "number") return 0;
      if (type == "boolean") return false;
      return nullptr;
    }

    const boost::json::value* json_request_schema(const boost::json::object& operation)
    {
      const auto* request_body = operation.if_contains("requestBody");
      if (request_body == nullptr || !request_body->is_object()) return nullptr;
      const auto* content = request_body->as_object().if_contains("content");
      if (content == nullptr || !content->is_object()) return nullptr;
      const auto* media_type = content->as_object().if_contains("application/json");
      if (media_type == nullptr || !media_type->is_object()) return nullptr;
      return media_type->as_object().if_contains("schema");
    }

    std::string render_parameters(const boost::json::object& operation)
    {
      std::string result = "<section class=\"operation-section\"><h3>Parameters</h3>";
      const auto* parameters = operation.if_contains("parameters");
      if (parameters == nullptr || !parameters->is_array() || parameters->as_array().empty())
        return result + "<p class=\"empty\">No parameters.</p></section>";

      result += "<div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>Location</th>"
        "<th>Required</th><th>Schema</th></tr></thead><tbody>";
      for (const auto& parameter_value : parameters->as_array())
      {
        if (!parameter_value.is_object()) continue;
        const auto& parameter = parameter_value.as_object();
        const auto* name = parameter.if_contains("name");
        const auto* location = parameter.if_contains("in");
        const auto* required = parameter.if_contains("required");
        const auto* schema = parameter.if_contains("schema");
        result += "<tr><td><code>" +
          escape_html(name != nullptr && name->is_string() ? std::string(name->as_string()) : "") +
          "</code></td><td>" +
          escape_html(location != nullptr && location->is_string() ?
            std::string(location->as_string()) : "") + "</td><td>" +
          (required != nullptr && required->is_bool() && required->as_bool() ? "Yes" : "No") +
          "</td><td>" +
          (schema != nullptr ? "<code>" + escape_html(boost::json::serialize(*schema)) + "</code>" : "—") +
          "</td></tr>";
      }
      return result + "</tbody></table></div></section>";
    }

    std::string render_request_body(const boost::json::object& operation)
    {
      std::string result = "<section class=\"operation-section\"><h3>Request body</h3>";
      const auto* request_body_value = operation.if_contains("requestBody");
      if (request_body_value == nullptr || !request_body_value->is_object())
        return result + "<p class=\"empty\">No request body.</p></section>";

      const auto& request_body = request_body_value->as_object();
      const auto* required = request_body.if_contains("required");
      result += "<p class=\"section-meta\">";
      result += required != nullptr && required->is_bool() && required->as_bool() ? "Required" : "Optional";
      result += "</p>";
      const auto* content_value = request_body.if_contains("content");
      if (content_value != nullptr && content_value->is_object())
      {
        for (const auto& media_type : content_value->as_object())
        {
          result += "<div class=\"media-type\"><span>" +
            escape_html(std::string(media_type.key())) + "</span></div>";
          if (media_type.value().is_object())
          {
            const auto* schema = media_type.value().as_object().if_contains("schema");
            if (schema != nullptr) result += render_schema_view(*schema);
          }
        }
      }
      return result + "</section>";
    }

    std::string render_responses(const boost::json::object& operation)
    {
      std::string result = "<section class=\"operation-section\"><h3>Responses</h3>";
      const auto* responses_value = operation.if_contains("responses");
      if (responses_value == nullptr || !responses_value->is_object() ||
          responses_value->as_object().empty())
        return result + "<p class=\"empty\">No responses documented.</p></section>";

      for (const auto& response_entry : responses_value->as_object())
      {
        result += "<div class=\"response\"><div class=\"response-heading\"><code>" +
          escape_html(std::string(response_entry.key())) + "</code>";
        if (response_entry.value().is_object())
        {
          const auto& response = response_entry.value().as_object();
          const auto* description = response.if_contains("description");
          if (description != nullptr && description->is_string())
            result += "<span>" + escape_html(std::string(description->as_string())) + "</span>";
          result += "</div>";
          const auto* content_value = response.if_contains("content");
          if (content_value != nullptr && content_value->is_object())
          {
            for (const auto& media_type : content_value->as_object())
            {
              result += "<div class=\"media-type\"><span>" +
                escape_html(std::string(media_type.key())) + "</span></div>";
              if (media_type.value().is_object())
              {
                const auto* schema = media_type.value().as_object().if_contains("schema");
                if (schema != nullptr) result += render_schema_view(*schema);
              }
            }
          }
        }
        else
        {
          result += "</div>";
        }
        result += "</div>";
      }
      return result + "</section>";
    }

    std::string render_operation_description(const boost::json::object& operation)
    {
      std::string result;
      const auto* summary = operation.if_contains("summary");
      if (summary != nullptr && summary->is_string())
        result += "<p class=\"operation-summary\">" +
          escape_html(std::string(summary->as_string())) + "</p>";
      const auto* description = operation.if_contains("description");
      if (description != nullptr && description->is_string())
        result += "<p class=\"operation-description\">" +
          escape_html(std::string(description->as_string())) + "</p>";
      return result;
    }

    std::string render_try_panel(const boost::json::object& operation,
                                 const std::string& method,
                                 const std::string& path)
    {
      const auto upper_method = uppercase(method);
      const auto* request_schema = json_request_schema(operation);
      const std::string request_body = request_schema != nullptr ?
        boost::json::serialize(schema_example(*request_schema)) : "";
      std::string curl_command = "curl -i -X " + upper_method + " " + shell_single_quote(path);
      if (!request_body.empty() && upper_method != "GET" && upper_method != "HEAD")
        curl_command += " \\\n  -H " + shell_single_quote("Content-Type: application/json") +
          " \\\n  --data-binary " + shell_single_quote(request_body);

      std::string result = "<details class=\"try-panel\" data-try-panel data-method=\"" +
        escape_html(upper_method) + "\" data-path=\"" + escape_html(path) + "\">";
      result += "<summary class=\"try-toggle\">Try request</summary><div class=\"try-content\">"
        "<p class=\"try-note\">Requests are sent to this server without browser credentials.</p>";

      const auto* parameters = operation.if_contains("parameters");
      if (parameters != nullptr && parameters->is_array())
      {
        for (const auto& parameter_value : parameters->as_array())
        {
          if (!parameter_value.is_object()) continue;
          const auto& parameter = parameter_value.as_object();
          const auto* location = parameter.if_contains("in");
          const auto* name = parameter.if_contains("name");
          if (location == nullptr || !location->is_string() || location->as_string() != "path" ||
              name == nullptr || !name->is_string()) continue;
          const std::string parameter_name(name->as_string());
          result += "<label class=\"try-field\"><span>" + escape_html(parameter_name) +
            " <small>path</small></span><input type=\"text\" data-path-param=\"" +
            escape_html(parameter_name) + "\" placeholder=\"Enter " + escape_html(parameter_name) +
            "\" autocomplete=\"off\"></label>";
        }
      }

      if (request_schema != nullptr)
        result += "<label class=\"try-field\"><span>JSON request body</span><textarea rows=\"7\" "
          "data-request-body spellcheck=\"false\">" + escape_html(request_body) + "</textarea></label>";

      result += "<div class=\"try-actions\"><button type=\"button\" class=\"button secondary\" "
        "data-action=\"copy-curl\">Copy cURL</button><button type=\"button\" class=\"button primary\" "
        "data-action=\"send-request\">Send request</button><span class=\"try-feedback\" data-feedback "
        "role=\"status\" aria-live=\"polite\"></span></div><pre class=\"curl-preview\"><code data-curl-output>" +
        escape_html(curl_command) + "</code></pre><div class=\"try-response\" data-response hidden>"
        "<div class=\"response-result-head\"><strong data-response-status></strong></div>"
        "<pre><code data-response-headers></code><code data-response-body></code></pre></div></div></details>";
      return result;
    }

    std::string documentation_script()
    {
      return R"SCRIPT((() => {
  const shellQuote = value => "'" + value.replace(/'/g, "'\\''") + "'";

  function requestFor(panel, requireParameters) {
    let path = panel.dataset.path;
    let missingParameter = "";
    panel.querySelectorAll("[data-path-param]").forEach(input => {
      const marker = "{" + input.dataset.pathParam + "}";
      if (input.value.length === 0) {
        if (requireParameters) missingParameter = input.dataset.pathParam;
      } else {
        path = path.replace(marker, encodeURIComponent(input.value));
      }
    });
    if (missingParameter) throw new Error("Enter path parameter: " + missingParameter);

    const method = panel.dataset.method;
    const url = window.location.origin + (path.startsWith("/") ? path : "/" + path);
    const bodyInput = panel.querySelector("[data-request-body]");
    const body = bodyInput ? bodyInput.value.trim() : "";
    let command = "curl -i -X " + method + " " + shellQuote(url);
    if (body && method !== "GET" && method !== "HEAD") {
      command += " \\\n  -H " + shellQuote("Content-Type: application/json") +
        " \\\n  --data-binary " + shellQuote(body);
    }
    panel.querySelector("[data-curl-output]").textContent = command;
    return {method, url, body, command};
  }

  function feedback(panel, message, error = false) {
    const output = panel.querySelector("[data-feedback]");
    output.textContent = message;
    output.classList.toggle("error", error);
  }

  async function copyText(value) {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(value);
      return;
    }
    const temporary = document.createElement("textarea");
    temporary.value = value;
    temporary.setAttribute("readonly", "");
    document.body.appendChild(temporary);
    temporary.select();
    const copied = document.execCommand("copy");
    temporary.remove();
    if (!copied) throw new Error("Clipboard is unavailable");
  }

  document.querySelectorAll("[data-try-panel]").forEach(panel => {
    requestFor(panel, false);
    panel.addEventListener("input", () => {
      try {
        requestFor(panel, false);
        feedback(panel, "");
      } catch (error) {
        feedback(panel, error.message, true);
      }
    });
  });

  document.addEventListener("click", async event => {
    const button = event.target.closest("[data-action]");
    if (!button) return;
    const panel = button.closest("[data-try-panel]");
    if (!panel) return;

    try {
      const request = requestFor(panel, button.dataset.action === "send-request");
      if (button.dataset.action === "copy-curl") {
        await copyText(request.command);
        feedback(panel, "cURL copied");
        return;
      }

      if (request.body) JSON.parse(request.body);
      button.disabled = true;
      feedback(panel, "Sending…");
      const options = {
        method: request.method,
        credentials: "omit",
        cache: "no-store",
        redirect: "manual",
        headers: {Accept: "application/json"}
      };
      if (request.body && request.method !== "GET" && request.method !== "HEAD") {
        options.headers["Content-Type"] = "application/json";
        options.body = request.body;
      }
      const response = await fetch(request.url, options);
      const responseBody = await response.text();
      const responsePanel = panel.querySelector("[data-response]");
      responsePanel.hidden = false;
      panel.querySelector("[data-response-status]").textContent =
        response.status + " " + response.statusText;
      panel.querySelector("[data-response-headers]").textContent =
        Array.from(response.headers.entries()).map(entry => entry[0] + ": " + entry[1]).join("\n");
      panel.querySelector("[data-response-body]").textContent = responseBody;
      feedback(panel, "Request completed");
    } catch (error) {
      feedback(panel, error instanceof SyntaxError ? "Request body is not valid JSON" : error.message, true);
    } finally {
      button.disabled = false;
    }
  });
})();)SCRIPT";
    }

    std::string make_csp_nonce()
    {
      static constexpr char hexadecimal[] = "0123456789abcdef";
      std::random_device source;
      std::string nonce;
      nonce.reserve(32);
      for (std::size_t index = 0; index < 16; ++index)
      {
        const auto value = source();
        nonce += hexadecimal[(value >> 4U) & 0x0fU];
        nonce += hexadecimal[value & 0x0fU];
      }
      return nonce;
    }

    std::string make_documentation_page(const boost::json::object& document,
                                        const OpenApiInfo& info,
                                        const std::string& spec_path,
                                        const std::string& script_nonce)
    {
      const auto& paths = document.at("paths").as_object();
      struct OperationView
      {
        std::string path;
        std::string method;
        std::string anchor;
        const boost::json::object* operation;
      };
      std::vector<OperationView> operations;
      std::map<std::string, std::size_t> anchors;
      for (const auto& path : paths)
      {
        for (const auto& operation : path.value().as_object())
        {
          const std::string method(operation.key());
          std::string anchor = operation_anchor(method, std::string(path.key()));
          const auto occurrence = ++anchors[anchor];
          if (occurrence > 1) anchor += "-" + std::to_string(occurrence);
          operations.push_back({std::string(path.key()), method, std::move(anchor),
                                &operation.value().as_object()});
        }
      }

      std::string page =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\"><meta name=\"color-scheme\" content=\"light\">"
        "<title>" + escape_html(info.title) + " · API documentation</title><style>"
        ":root{--ink:#162033;--muted:#657085;--line:#dce3ed;--surface:#fff;--soft:#f5f7fb;"
        "--brand:#3157d5}*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--soft);"
        "color:var(--ink);font:15px/1.55 Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"
        "\"Segoe UI\",sans-serif}a{color:var(--brand)}code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,"
        "Consolas,monospace}.topbar{background:linear-gradient(120deg,#eff6ff,#dbeafe 60%,#bfdbfe);color:#172554;"
        "padding:46px max(24px,calc((100vw - 1320px)/2)) 38px}.eyebrow{margin:0 0 8px;color:#3157d5;"
        "font-size:12px;font-weight:800;letter-spacing:.13em;text-transform:uppercase}.topbar h1{margin:0;"
        "font-size:clamp(30px,4vw,44px);letter-spacing:-.035em}.topbar-copy{display:flex;align-items:center;"
        "justify-content:space-between;gap:24px}.version{margin:9px 0 0;color:#475569}.spec-link{display:inline-flex;"
        "align-items:center;gap:8px;border:1px solid #93c5fd;border-radius:9px;padding:10px 14px;"
        "color:#1e40af;text-decoration:none;font-weight:700;background:rgba(255,255,255,.72)}.layout{display:grid;"
        "grid-template-columns:260px minmax(0,1fr);gap:30px;max-width:1320px;margin:0 auto;padding:30px 24px 70px}"
        ".sidebar{position:sticky;top:20px;align-self:start;max-height:calc(100vh - 40px);overflow:auto}.overview{display:grid;"
        "grid-template-columns:1fr 1fr;gap:10px;margin-bottom:22px}.stat{background:var(--surface);border:1px solid var(--line);"
        "border-radius:12px;padding:14px}.stat strong{display:block;font-size:24px;line-height:1.1}.stat span{color:var(--muted);"
        "font-size:12px}.sidebar h2{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);"
        "margin:0 0 8px}.sidebar nav{display:flex;flex-direction:column;gap:4px}.nav-item{display:flex;align-items:center;"
        "gap:9px;padding:8px 10px;border-radius:8px;color:#334155;text-decoration:none;min-width:0}.nav-item:hover{"
        "background:#e9edf7;color:#172554}.nav-method{width:47px;flex:none;border:1px solid currentColor;border-radius:5px;"
        "padding:1px 3px;text-align:center;font-size:9px;font-weight:900}.nav-method.get{color:#147d64}.nav-method.post{"
        "color:#2563eb}.nav-method.put{color:#a76208}.nav-method.patch{color:#7c3aed}.nav-method.delete{color:#dc3545}"
        ".nav-method.options,.nav-method.head,.nav-method.other{color:#64748b}.nav-path{"
        "overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font:12px ui-monospace,monospace}.content{min-width:0}"
        ".intro{margin:0 0 18px}.intro h2{font-size:22px;margin:0 0 4px}.intro p{margin:0;color:var(--muted)}"
        ".operation{--method:#64748b;background:var(--surface);border:1px solid var(--line);border-left:5px solid var(--method);"
        "border-radius:14px;margin:0 0 20px;overflow:hidden;box-shadow:0 6px 22px rgba(23,37,84,.045);scroll-margin-top:18px}"
        ".operation-head{display:flex;align-items:center;gap:13px;padding:18px 20px;border-bottom:1px solid var(--line);"
        "background:linear-gradient(90deg,color-mix(in srgb,var(--method) 7%,white),white 55%)}.operation-head h2{"
        "font-size:16px;margin:0;min-width:0}.operation-head code{overflow-wrap:anywhere}.method{display:inline-flex;"
        "justify-content:center;min-width:68px;padding:6px 10px;border-radius:7px;background:var(--method);color:#fff;"
        "font-size:11px;font-weight:900;letter-spacing:.06em}.operation-summary{margin:16px 0 0;font-size:15px;font-weight:750;}"
        ".operation-description{margin:7px 0 0;color:var(--muted);font-size:13px}.operation-body{padding:4px 20px 20px}.operation-section{"
        "padding:15px 0;border-bottom:1px solid #edf0f5}.operation-section h3{font-size:13px;margin:0 0 10px;"
        "letter-spacing:.02em}.empty,.section-meta{color:var(--muted);margin:0;font-size:13px}.section-meta{margin-bottom:8px}"
        ".table-wrap{overflow:auto}table{width:100%;border-collapse:collapse;font-size:13px}th{text-align:left;color:var(--muted);"
        "font-size:11px;text-transform:uppercase;letter-spacing:.05em}th,td{padding:8px 10px;border-bottom:1px solid #edf0f5;"
        "vertical-align:top}td code{overflow-wrap:anywhere}.media-type{margin:8px 0 6px}.media-type span{display:inline-block;"
        "border-radius:999px;background:#e8eefc;color:#2949a3;padding:3px 9px;font:11px ui-monospace,monospace}.schema{"
        "margin:6px 0 10px;padding:13px 14px;border:1px solid #e2e8f0;border-radius:9px;background:#f8fafc;color:#334155;"
        "overflow:auto;font-size:12px}.schema-view{margin:8px 0 12px;border:1px solid #e2e8f0;border-radius:10px;"
        "background:#fbfcfe;overflow:hidden}.schema-summary{display:flex;align-items:center;gap:8px;padding:10px 12px;background:#f1f5f9}"
        ".schema-type,.property-type{font:700 11px ui-monospace,monospace;color:#334155}.schema-format{font:11px ui-monospace,"
        "monospace;color:var(--muted)}.schema-fields{padding:0 12px}.schema-property{padding:11px 0;border-top:1px solid #e8edf4}"
        ".schema-property:first-child{border-top:0}.property-head{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.property-name{"
        "font-weight:750;color:#172554}.property-type{border-radius:5px;background:#e8eefc;color:#2949a3;padding:2px 6px}"
        ".required{border-radius:5px;background:#fff1f2;color:#be123c;padding:2px 6px;font-size:10px;font-weight:800}"
        ".property-description{margin:6px 0 0;color:var(--muted);font-size:12px}.schema-property .schema-view{margin:9px 0 0}"
        ".array-items{margin:8px 0 0;padding-left:12px;border-left:2px solid #dbe3f0}.array-items>span{color:var(--muted);"
        "font-size:11px;font-weight:700}.schema-empty{padding:10px 12px}.schema-raw{margin:0;border-top:1px solid #e2e8f0;"
        "padding:8px 12px}.schema-raw summary{font-size:11px}.response{margin-top:10px}.response-heading{display:flex;"
        "align-items:center;gap:10px}.response-heading code{"
        "border-radius:6px;background:#dcfce7;color:#166534;padding:3px 8px;font-weight:800}.response-heading span{color:var(--muted);"
        "font-size:13px}.try-panel{margin-top:18px;border:1px solid #cbd8fa;border-radius:12px;background:#f5f8ff;overflow:hidden}"
        ".try-toggle{padding:13px 18px;color:#2949a3;font-size:13px;font-weight:800}.try-panel[open]>.try-toggle{"
        "border-bottom:1px solid #d6e0f7}.try-content{padding:5px 18px 18px}.try-note{margin:8px 0 13px;color:var(--muted);"
        "font-size:12px}.try-field{display:block;margin:10px 0}.try-field>span{display:block;"
        "margin-bottom:5px;font-size:12px;font-weight:750}.try-field small{color:var(--muted);font-weight:500}.try-field input,"
        ".try-field textarea{width:100%;border:1px solid #bdc8da;border-radius:8px;background:#fff;color:var(--ink);padding:9px 11px;"
        "font:13px ui-monospace,monospace;outline:none}.try-field textarea{resize:vertical;min-height:90px}.try-field input:focus,"
        ".try-field textarea:focus{border-color:#4f6fde;box-shadow:0 0 0 3px rgba(79,111,222,.13)}.try-actions{display:flex;"
        "align-items:center;gap:8px;flex-wrap:wrap;margin-top:12px}.button{border:0;border-radius:8px;padding:8px 12px;font:700 12px system-ui;"
        "cursor:pointer}.button:disabled{cursor:wait;opacity:.65}.button.primary{background:#3157d5;color:#fff}.button.secondary{"
        "border:1px solid #9aabd0;background:#fff;color:#2949a3}.try-feedback{color:#166534;font-size:12px}.try-feedback.error{"
        "color:#b91c1c}.curl-preview,.try-response pre{margin:12px 0 0;padding:12px;border:1px solid #d8e2ef;border-radius:9px;"
        "background:#f8fafc;color:#334155;overflow:auto;font-size:11px;white-space:pre-wrap;overflow-wrap:anywhere}.try-response{"
        "margin-top:14px}.response-result-head{font-size:13px}.try-response code{display:block}.try-response "
        "[data-response-headers]{color:#1e40af}"
        "details{margin-top:14px}summary{cursor:pointer;color:var(--muted);font-size:12px;font-weight:700}"
        "details pre{margin:8px 0 0;padding:13px;border:1px solid #d8e2ef;border-radius:9px;background:#f8fafc;color:#334155;"
        "overflow:auto;font-size:11px}"
        ".operation.get{--method:#147d64}.operation.post{--method:#2563eb}.operation.put{--method:#c8790a}"
        ".operation.patch{--method:#7c3aed}.operation.delete{--method:#dc3545}.operation.options{--method:#64748b}"
        ".operation.head{--method:#0891b2}@media(max-width:780px){.topbar{padding:32px 20px}.topbar-copy{align-items:flex-start;"
        "flex-direction:column}.layout{grid-template-columns:1fr;padding:22px 16px 50px}.sidebar{position:static;max-height:none}.sidebar nav{"
        "display:flex;flex-direction:row;gap:8px;overflow-x:auto;padding:2px 0 10px;scrollbar-width:thin}.nav-item{flex:0 0 auto;"
        "max-width:260px;border:1px solid var(--line);background:var(--surface)}.operation-head{align-items:flex-start;flex-direction:column;"
        "gap:9px}.operation-body{padding-left:15px;padding-right:15px}}"
        "</style></head><body><header class=\"topbar\"><div class=\"topbar-copy\"><div><p class=\"eyebrow\">"
        "OpenAPI 3.1</p><h1>" + escape_html(info.title) + "</h1><p class=\"version\">Version " +
        escape_html(info.version) + "</p></div><a class=\"spec-link\" href=\"" + escape_html(spec_path) +
        "\">View raw specification&nbsp; ↗</a></div></header><div class=\"layout\"><aside class=\"sidebar\">"
        "<div class=\"overview\"><div class=\"stat\"><strong>" + std::to_string(paths.size()) +
        "</strong><span>Paths</span></div><div class=\"stat\"><strong>" + std::to_string(operations.size()) +
        "</strong><span>Operations</span></div></div><h2>Endpoints</h2><nav aria-label=\"Endpoints\">";

      for (const auto& operation : operations)
      {
        page += "<a class=\"nav-item\" href=\"#" + operation.anchor + "\"><span class=\"nav-method " +
          method_class(operation.method) + "\">" +
          escape_html(uppercase(operation.method)) + "</span><span class=\"nav-path\">" +
          escape_html(operation.path) + "</span></a>";
      }
      page += "</nav></aside><main class=\"content\"><div class=\"intro\"><h2>API reference</h2>"
        "<p>Select an endpoint to inspect its contract and schemas.</p></div>";

      for (const auto& operation : operations)
      {
        const auto css_class = method_class(operation.method);
        page += "<article class=\"operation " + css_class + "\" id=\"" + operation.anchor +
          "\"><header class=\"operation-head\"><span class=\"method\">" +
          escape_html(uppercase(operation.method)) + "</span><h2><code>" + escape_html(operation.path) +
          "</code></h2></header><div class=\"operation-body\">" +
          render_operation_description(*operation.operation) + render_parameters(*operation.operation) +
          render_request_body(*operation.operation) + render_responses(*operation.operation) +
          render_try_panel(*operation.operation, operation.method, operation.path) +
          "<details><summary>Raw operation JSON</summary><pre><code>" +
          escape_html(boost::json::serialize(*operation.operation)) +
          "</code></pre></details></div></article>";
      }
      page += "</main></div><script nonce=\"" + script_nonce + "\">" +
        documentation_script() + "</script></body></html>";
      return page;
    }
  }

  boost::json::object generate_openapi(const HttpRouter& router, const OpenApiInfo& info)
  {
    using Operations = std::map<std::string, boost::json::object>;
    std::map<std::string, Operations> documented_paths;

    for (const auto& descriptor : router.route_descriptors())
    {
      auto path = document_path(descriptor.path);
      documented_paths[path.path][method_name(descriptor.method)] =
        make_operation(descriptor, path.parameters);
    }

    boost::json::object paths;
    for (auto& [path, methods] : documented_paths)
    {
      boost::json::object path_item;
      for (auto& [method, operation] : methods)
        path_item.emplace(method, std::move(operation));
      paths.emplace(path, std::move(path_item));
    }

    boost::json::object document;
    document.emplace("openapi", "3.1.0");
    document.emplace("info", boost::json::object{{"title", info.title}, {"version", info.version}});
    document.emplace("paths", std::move(paths));
    return document;
  }

  void install_openapi_routes(HttpRouter& router, const OpenApiInfo& info,
                              const std::string& spec_path, const std::string& docs_path,
                              const bool enabled)
  {
    if (!enabled) return;

    validate_documentation_path(spec_path, "OpenAPI specification path");
    validate_documentation_path(docs_path, "OpenAPI documentation path");
    if (spec_path == docs_path)
      throw std::invalid_argument("OpenAPI specification and documentation paths must differ");

    for (const auto& descriptor : router.route_descriptors())
    {
      if (descriptor.method == boost::beast::http::verb::get &&
          (descriptor.path == spec_path || descriptor.path == docs_path))
        throw std::invalid_argument("OpenAPI documentation path conflicts with an existing GET route: " +
                                    descriptor.path);
    }

    const auto document = generate_openapi(router, info);
    auto serialized_document = std::make_shared<const std::string>(boost::json::serialize(document));
    router.add_route(spec_path, boost::beast::http::verb::get,
      [serialized_document](HttpContext& context)
      {
        context.set_status(boost::beast::http::status::ok);
        context.set_content_type("application/json");
        context.set_header("X-Content-Type-Options", "nosniff");
        context.set_body(*serialized_document);
      }, std::nullopt, std::nullopt, false);

    auto script_nonce = std::make_shared<const std::string>(make_csp_nonce());
    auto page = std::make_shared<const std::string>(
      make_documentation_page(document, info, spec_path, *script_nonce));
    router.add_route(docs_path, boost::beast::http::verb::get,
      [page, script_nonce](HttpContext& context)
      {
        context.set_status(boost::beast::http::status::ok);
        context.set_content_type("text/html");
        context.set_header("X-Content-Type-Options", "nosniff");
        context.set_header("Content-Security-Policy",
                           "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; "
                           "script-src 'nonce-" + *script_nonce + "'; connect-src 'self'; "
                           "form-action 'none'; frame-ancestors 'none'");
        context.set_header("Referrer-Policy", "no-referrer");
        context.set_body(*page);
      }, std::nullopt, std::nullopt, false);
  }

  void export_openapi(const HttpRouter& router, const std::string& output_path,
                      const OpenApiInfo& info)
  {
    if (output_path.empty()) throw std::invalid_argument("OpenAPI output path must not be empty");

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
      throw std::runtime_error("Unable to open OpenAPI output path: " + output_path);

    output << boost::json::serialize(generate_openapi(router, info)) << '\n';
    output.close();
    if (!output)
      throw std::runtime_error("Unable to write OpenAPI output path: " + output_path);
  }
}
