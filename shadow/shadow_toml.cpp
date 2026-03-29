#include "shadow_toml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <exception>
#include <memory>
#include <string>

#include <cpptoml.h>

namespace {

char *dup_string(const std::string &value) {
  char *copy = static_cast<char *>(std::malloc(value.size() + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

bool parse_color_component(const cpptoml::base &value, uint8_t *out) {
  if (const auto *as_int = dynamic_cast<const cpptoml::value<int64_t> *>(&value)) {
    if (as_int->get() < 0 || as_int->get() > 255) {
      return false;
    }
    *out = static_cast<uint8_t>(as_int->get());
    return true;
  }

  if (const auto *as_double = dynamic_cast<const cpptoml::value<double> *>(&value)) {
    const double parsed = as_double->get();
    const double rounded = std::round(parsed);
    if (parsed < 0.0 || parsed > 255.0 || std::fabs(parsed - rounded) > 1e-9) {
      return false;
    }
    *out = static_cast<uint8_t>(rounded);
    return true;
  }

  return false;
}

bool load_color_array(
    const std::shared_ptr<cpptoml::array> &array,
    const char *label,
    ShadowColor *out) {
  if (array == nullptr || (array->get().size() != 3u && array->get().size() != 4u)) {
    std::fprintf(stderr, "%s must be an array with 3 or 4 components.\n", label);
    return false;
  }

  uint8_t components[4] = {0u, 0u, 0u, 255u};
  for (size_t i = 0; i < array->get().size(); ++i) {
    if (!parse_color_component(*array->get()[i], &components[i])) {
      std::fprintf(stderr, "Invalid color component at %s[%zu].\n", label, i);
      return false;
    }
  }

  out->r = components[0];
  out->g = components[1];
  out->b = components[2];
  out->a = components[3];
  return true;
}

}  // namespace

extern "C" int load_shadow_config_file(const char *path, ShadowOptions *options) {
  if (path == nullptr || options == nullptr) {
    return -1;
  }

  ShadowOptions parsed = {};
  parsed.set_color.a = 255u;
  parsed.replace_color.a = 255u;
  parsed.direction = SHADOW_FROM_BOTTOM;

  try {
    const auto config = cpptoml::parse_file(path);
    const auto input = config->get_table("input");
    const auto output = config->get_table("output");

    if (input != nullptr) {
      if (auto value = input->get_as<std::string>("directory")) {
        parsed.input_dir = dup_string(*value);
      }
    }
    if (output != nullptr) {
      if (auto value = output->get_as<std::string>("directory")) {
        parsed.output_dir = dup_string(*value);
      }
    }

    if (parsed.input_dir == nullptr || parsed.output_dir == nullptr) {
      std::fprintf(stderr, "input.directory and output.directory are required.\n");
      free_shadow_options(&parsed);
      return -1;
    }

    const auto set_color =
        output != nullptr ? output->get_array("set_color") : config->get_array("set_color");
    const auto replace_color = output != nullptr
                                   ? output->get_array("replace_color")
                                   : config->get_array("replace_color");
    if (!load_color_array(set_color, "set_color", &parsed.set_color) ||
        !load_color_array(replace_color, "replace_color", &parsed.replace_color)) {
      free_shadow_options(&parsed);
      return -1;
    }

    std::string direction_text = "bottom";
    if (input != nullptr) {
      if (auto value = input->get_as<std::string>("from")) {
        direction_text = *value;
      }
    }
    if (output != nullptr) {
      if (auto value = output->get_as<std::string>("from")) {
        direction_text = *value;
      }
    }
    if (auto value = config->get_as<std::string>("from")) {
      direction_text = *value;
    }
    if (direction_text == "bottom") {
      parsed.direction = SHADOW_FROM_BOTTOM;
    } else if (direction_text == "top") {
      parsed.direction = SHADOW_FROM_TOP;
    } else {
      std::fprintf(stderr, "from must be \"top\" or \"bottom\".\n");
      free_shadow_options(&parsed);
      return -1;
    }
  } catch (const cpptoml::parse_exception &error) {
    std::fprintf(stderr, "Failed to parse %s: %s\n", path, error.what());
    free_shadow_options(&parsed);
    return -1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Failed to load %s: %s\n", path, error.what());
    free_shadow_options(&parsed);
    return -1;
  }

  *options = parsed;
  return 0;
}
