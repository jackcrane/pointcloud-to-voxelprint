#include "addImages_toml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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

char *dup_path(const std::string &value) {
  if (value == "~" || value.rfind("~/", 0) == 0) {
    const char *home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return nullptr;
    }

    const std::string expanded =
        value == "~" ? std::string(home) : std::string(home) + value.substr(1);
    return dup_string(expanded);
  }

  return dup_string(value);
}

bool parse_u32_field(const std::shared_ptr<cpptoml::table> &table,
                     const char *key,
                     uint32_t *out) {
  if (auto value = table->get_as<int64_t>(key)) {
    if (*value < 0 || *value > static_cast<int64_t>(UINT32_MAX)) {
      return false;
    }
    *out = static_cast<uint32_t>(*value);
    return true;
  }

  if (auto value = table->get_as<double>(key)) {
    const double parsed = *value;
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

bool parse_positive_u32_field(const std::shared_ptr<cpptoml::table> &table,
                              const char *key,
                              uint32_t *out) {
  return parse_u32_field(table, key, out) && *out > 0u;
}

}  // namespace

extern "C" void free_add_images_options(AddImagesOptions *options) {
  if (options == nullptr) {
    return;
  }

  std::free(const_cast<char *>(options->input_dir));
  std::free(const_cast<char *>(options->output_dir));
  if (options->overlays != nullptr) {
    for (size_t i = 0; i < options->overlay_count; ++i) {
      std::free(const_cast<char *>(options->overlays[i].src_path));
    }
  }
  std::free(options->overlays);
  std::memset(options, 0, sizeof(*options));
}

extern "C" int load_add_images_config_file(const char *path, AddImagesOptions *options) {
  if (path == nullptr || options == nullptr) {
    return -1;
  }

  AddImagesOptions parsed = {};

  try {
    const auto config = cpptoml::parse_file(path);
    const auto input = config->get_table("input");
    const auto output = config->get_table("output");
    const auto layer = config->get_table("layer");
    const auto image_tables = config->get_table_array("image");

    if (input != nullptr) {
      if (auto value = input->get_as<std::string>("directory")) {
        parsed.input_dir = dup_path(*value);
      }
    }
    if (output != nullptr) {
      if (auto value = output->get_as<std::string>("directory")) {
        parsed.output_dir = dup_path(*value);
      }
    }

    if (parsed.input_dir == nullptr || parsed.output_dir == nullptr) {
      std::fprintf(stderr, "input.directory and output.directory are required.\n");
      free_add_images_options(&parsed);
      return -1;
    }

    if (layer != nullptr) {
      uint32_t first = 0u;
      uint32_t last = 0u;
      const bool has_first = layer->contains("first");
      const bool has_last = layer->contains("last");

      if (has_first && !parse_u32_field(layer, "first", &first)) {
        std::fprintf(stderr, "layer.first must be a whole number between 0 and %u.\n", UINT32_MAX);
        free_add_images_options(&parsed);
        return -1;
      }
      if (has_last && !parse_u32_field(layer, "last", &last)) {
        std::fprintf(stderr, "layer.last must be a whole number between 0 and %u.\n", UINT32_MAX);
        free_add_images_options(&parsed);
        return -1;
      }
      if (has_first && has_last && first > last) {
        std::fprintf(stderr, "layer.first must be less than or equal to layer.last.\n");
        free_add_images_options(&parsed);
        return -1;
      }

      parsed.has_layer_first = has_first;
      parsed.has_layer_last = has_last;
      parsed.layer_first = first;
      parsed.layer_last = last;
    }

    if (image_tables == nullptr || image_tables->get().empty()) {
      std::fprintf(stderr, "At least one [[image]] entry is required.\n");
      free_add_images_options(&parsed);
      return -1;
    }

    const auto &tables = image_tables->get();
    std::vector<AddImagesOverlay> overlays;
    overlays.reserve(tables.size());

    for (size_t i = 0; i < tables.size(); ++i) {
      const auto &table = tables[i];
      AddImagesOverlay overlay = {};

      if (auto value = table->get_as<std::string>("src")) {
        overlay.src_path = dup_path(*value);
      }
      if (overlay.src_path == nullptr) {
        std::fprintf(stderr, "Each [[image]] entry must include src.\n");
        free_add_images_options(&parsed);
        return -1;
      }

      if (!parse_u32_field(table, "x", &overlay.x) ||
          !parse_u32_field(table, "y", &overlay.y)) {
        std::fprintf(stderr, "Each [[image]] entry must include whole-number x and y values.\n");
        std::free(const_cast<char *>(overlay.src_path));
        free_add_images_options(&parsed);
        return -1;
      }

      overlay.has_width = table->contains("width");
      overlay.has_height = table->contains("height");
      if (!overlay.has_width && !overlay.has_height) {
        std::fprintf(stderr, "Each [[image]] entry must include width or height.\n");
        std::free(const_cast<char *>(overlay.src_path));
        free_add_images_options(&parsed);
        return -1;
      }
      if (overlay.has_width && !parse_positive_u32_field(table, "width", &overlay.width)) {
        std::fprintf(stderr, "image.width must be a positive whole number.\n");
        std::free(const_cast<char *>(overlay.src_path));
        free_add_images_options(&parsed);
        return -1;
      }
      if (overlay.has_height && !parse_positive_u32_field(table, "height", &overlay.height)) {
        std::fprintf(stderr, "image.height must be a positive whole number.\n");
        std::free(const_cast<char *>(overlay.src_path));
        free_add_images_options(&parsed);
        return -1;
      }

      if (auto value = table->get_as<bool>("invert")) {
        overlay.invert = *value;
      }

      overlays.push_back(overlay);
    }

    parsed.overlay_count = overlays.size();
    parsed.overlays = static_cast<AddImagesOverlay *>(
        std::malloc(overlays.size() * sizeof(AddImagesOverlay)));
    if (parsed.overlays == nullptr) {
      std::fprintf(stderr, "Failed to allocate image overlay storage.\n");
      for (const auto &overlay : overlays) {
        std::free(const_cast<char *>(overlay.src_path));
      }
      free_add_images_options(&parsed);
      return -1;
    }
    std::memcpy(parsed.overlays, overlays.data(), overlays.size() * sizeof(AddImagesOverlay));
  } catch (const cpptoml::parse_exception &error) {
    std::fprintf(stderr, "Failed to parse %s: %s\n", path, error.what());
    free_add_images_options(&parsed);
    return -1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Failed to load %s: %s\n", path, error.what());
    free_add_images_options(&parsed);
    return -1;
  }

  *options = parsed;
  return 0;
}
