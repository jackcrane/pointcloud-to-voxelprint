#include "hollow_toml.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

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

bool parse_u32_value(const cpptoml::base &value, uint32_t *out) {
  if (const auto *as_int = dynamic_cast<const cpptoml::value<int64_t> *>(&value)) {
    if (as_int->get() < 0 || as_int->get() > static_cast<int64_t>(UINT32_MAX)) {
      return false;
    }
    *out = static_cast<uint32_t>(as_int->get());
    return true;
  }

  if (const auto *as_double = dynamic_cast<const cpptoml::value<double> *>(&value)) {
    const double parsed = as_double->get();
    const double rounded = std::round(parsed);
    if (parsed < 0.0 || parsed > static_cast<double>(UINT32_MAX) ||
        std::fabs(parsed - rounded) > 1e-9) {
      return false;
    }
    *out = static_cast<uint32_t>(rounded);
    return true;
  }

  return false;
}

bool load_color_array(
    const std::shared_ptr<cpptoml::array> &array,
    const char *label,
    HollowColor *out) {
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

bool load_required_u32(
    const std::shared_ptr<cpptoml::table> &primary,
    const std::shared_ptr<cpptoml::table> &fallback,
    const char *key,
    uint32_t *out) {
  std::shared_ptr<cpptoml::base> value;
  if (primary != nullptr && primary->contains(key)) {
    value = primary->get(key);
  } else if (fallback != nullptr) {
    value = fallback->get(key);
  }
  if (value == nullptr || !parse_u32_value(*value, out)) {
    std::fprintf(stderr, "%s must be a whole number between 0 and %u.\n", key, UINT32_MAX);
    return false;
  }
  return true;
}

}  // namespace

extern "C" int load_hollow_config_file(const char *path, HollowOptions *options) {
  if (path == nullptr || options == nullptr) {
    return -1;
  }

  HollowOptions parsed = {};
  parsed.destination_color.a = 255u;
  parsed.log_interval = HOLLOW_DEFAULT_LOG_INTERVAL;

  try {
    const auto config = cpptoml::parse_file(path);
    const auto input = config->get_table("input");
    const auto output = config->get_table("output");
    const auto logging = config->get_table("logging");

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
      free_hollow_options(&parsed);
      return -1;
    }

    if (!load_required_u32(output, config, "dist_positive_x", &parsed.dist_positive_x) ||
        !load_required_u32(output, config, "dist_negative_x", &parsed.dist_negative_x) ||
        !load_required_u32(output, config, "dist_positive_y", &parsed.dist_positive_y) ||
        !load_required_u32(output, config, "dist_negative_y", &parsed.dist_negative_y) ||
        !load_required_u32(output, config, "dist_positive_z", &parsed.dist_positive_z) ||
        !load_required_u32(output, config, "dist_negative_z", &parsed.dist_negative_z)) {
      free_hollow_options(&parsed);
      return -1;
    }

    const auto destination_color = output != nullptr && output->contains("destination_color")
                                       ? output->get_array("destination_color")
                                       : config->get_array("destination_color");
    if (!load_color_array(destination_color,
                          "destination_color",
                          &parsed.destination_color)) {
      free_hollow_options(&parsed);
      return -1;
    }

    const auto removal_colors = output != nullptr && output->contains("colors_for_removal")
                                    ? output->get_array_of<cpptoml::array>("colors_for_removal")
                                    : config->get_array_of<cpptoml::array>("colors_for_removal");
    if (!removal_colors || removal_colors->empty()) {
      std::fprintf(stderr, "colors_for_removal must contain at least one color.\n");
      free_hollow_options(&parsed);
      return -1;
    }

    std::vector<HollowColor> colors;
    colors.reserve(removal_colors->size());
    for (size_t i = 0; i < removal_colors->size(); ++i) {
      HollowColor color = {};
      char label[64];
      std::snprintf(label, sizeof(label), "colors_for_removal[%zu]", i);
      if (!load_color_array((*removal_colors)[i], label, &color)) {
        free_hollow_options(&parsed);
        return -1;
      }
      colors.push_back(color);
    }

    parsed.colors_for_removal_count = colors.size();
    parsed.colors_for_removal = static_cast<HollowColor *>(
        std::malloc(colors.size() * sizeof(HollowColor)));
    if (parsed.colors_for_removal == nullptr) {
      std::fprintf(stderr, "Failed to allocate colors_for_removal.\n");
      free_hollow_options(&parsed);
      return -1;
    }
    std::memcpy(parsed.colors_for_removal, colors.data(), colors.size() * sizeof(HollowColor));

    if (logging != nullptr) {
      if (auto value = logging->get_as<int64_t>("interval")) {
        if (*value < 0) {
          std::fprintf(stderr, "Invalid value for logging.interval.\n");
          free_hollow_options(&parsed);
          return -1;
        }
        parsed.log_interval = static_cast<uint64_t>(*value);
      }
    }
  } catch (const cpptoml::parse_exception &error) {
    std::fprintf(stderr, "Failed to parse %s: %s\n", path, error.what());
    free_hollow_options(&parsed);
    return -1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Failed to load %s: %s\n", path, error.what());
    free_hollow_options(&parsed);
    return -1;
  }

  *options = parsed;
  return 0;
}
