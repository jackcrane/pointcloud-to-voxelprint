#include "fillRegion_toml.h"

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

bool parse_double_field(const std::shared_ptr<cpptoml::table> &table,
                        const char *key,
                        double *out) {
  if (auto value = table->get_as<double>(key)) {
    *out = *value;
    return true;
  }
  if (auto value = table->get_as<int64_t>(key)) {
    *out = static_cast<double>(*value);
    return true;
  }
  return false;
}

}  // namespace

extern "C" void free_fill_region_options(FillRegionOptions *options) {
  if (options == nullptr) {
    return;
  }

  std::free(const_cast<char *>(options->input_dir));
  std::free(const_cast<char *>(options->output_dir));
  std::free(options->points);
  std::memset(options, 0, sizeof(*options));
}

extern "C" int load_fill_region_config_file(const char *path, FillRegionOptions *options) {
  if (path == nullptr || options == nullptr) {
    return -1;
  }

  FillRegionOptions parsed = {};
  parsed.color.a = 255u;

  try {
    const auto config = cpptoml::parse_file(path);
    const auto input = config->get_table("input");
    const auto output = config->get_table("output");
    const auto color = config->get_array("color");
    const auto point_tables = config->get_table_array("points");

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
      free_fill_region_options(&parsed);
      return -1;
    }

    if (color == nullptr || (color->get().size() != 3u && color->get().size() != 4u)) {
      std::fprintf(stderr, "color must be an array with 3 or 4 components.\n");
      free_fill_region_options(&parsed);
      return -1;
    }

    uint8_t components[4] = {0u, 0u, 0u, 255u};
    for (size_t i = 0; i < color->get().size(); ++i) {
      if (!parse_color_component(*color->get()[i], &components[i])) {
        std::fprintf(stderr, "Invalid color component at color[%zu].\n", i);
        free_fill_region_options(&parsed);
        return -1;
      }
    }
    parsed.color.r = components[0];
    parsed.color.g = components[1];
    parsed.color.b = components[2];
    parsed.color.a = components[3];

    if (point_tables == nullptr || point_tables->get().empty()) {
      std::fprintf(stderr, "At least three [[points]] entries are required.\n");
      free_fill_region_options(&parsed);
      return -1;
    }

    const auto &tables = point_tables->get();
    std::vector<FillRegionPoint> points;
    points.reserve(tables.size());
    bool saw_z = false;
    bool saw_missing_z = false;

    for (size_t i = 0; i < tables.size(); ++i) {
      const auto &table = tables[i];
      FillRegionPoint point = {};

      if (!parse_double_field(table, "x", &point.x) || !parse_double_field(table, "y", &point.y)) {
        std::fprintf(stderr, "Each [[points]] entry must include numeric x and y values.\n");
        free_fill_region_options(&parsed);
        return -1;
      }

      if (parse_double_field(table, "z", &point.z)) {
        point.has_z = true;
        saw_z = true;
      } else {
        saw_missing_z = true;
      }

      points.push_back(point);
    }

    if (points.size() < 3u) {
      std::fprintf(stderr, "At least three points are required.\n");
      free_fill_region_options(&parsed);
      return -1;
    }

    if (saw_z && saw_missing_z) {
      std::fprintf(stderr, "Points must be either all XY or all XYZ.\n");
      free_fill_region_options(&parsed);
      return -1;
    }

    parsed.mode = saw_z ? FILL_REGION_MODE_XYZ : FILL_REGION_MODE_XY;
    parsed.point_count = points.size();
    parsed.points =
        static_cast<FillRegionPoint *>(std::malloc(points.size() * sizeof(FillRegionPoint)));
    if (parsed.points == nullptr) {
      std::fprintf(stderr, "Failed to allocate point storage.\n");
      free_fill_region_options(&parsed);
      return -1;
    }
    std::memcpy(parsed.points, points.data(), points.size() * sizeof(FillRegionPoint));
  } catch (const cpptoml::parse_exception &error) {
    std::fprintf(stderr, "Failed to parse %s: %s\n", path, error.what());
    free_fill_region_options(&parsed);
    return -1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Failed to load %s: %s\n", path, error.what());
    free_fill_region_options(&parsed);
    return -1;
  }

  *options = parsed;
  return 0;
}
