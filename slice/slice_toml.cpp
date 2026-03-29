#include "slice_toml.h"

#include <cstdio>
#include <cstring>
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

template <typename T>
bool require_positive(const std::shared_ptr<cpptoml::table> &table,
                      const char *qualified_key,
                      const char *key,
                      double *out) {
  if (table == nullptr) {
    return true;
  }
  auto value = table->get_as<T>(key);
  if (!value) {
    return true;
  }
  const double parsed = static_cast<double>(*value);
  if (parsed <= 0.0) {
    std::fprintf(stderr, "Invalid value for %s.\n", qualified_key);
    return false;
  }
  *out = parsed;
  return true;
}

bool require_non_negative(const std::shared_ptr<cpptoml::table> &table,
                          const char *qualified_key,
                          const char *key,
                          double *out) {
  if (table == nullptr) {
    return true;
  }
  auto value = table->get_as<double>(key);
  if (!value) {
    return true;
  }
  if (*value < 0.0) {
    std::fprintf(stderr, "Invalid value for %s.\n", qualified_key);
    return false;
  }
  *out = *value;
  return true;
}

bool parse_dimension(const std::shared_ptr<cpptoml::table> &table,
                     const char *qualified_key,
                     const char *key,
                     double *dimension_out,
                     bool *is_set_out) {
  if (table == nullptr) {
    return true;
  }

  if (auto text = table->get_as<std::string>(key)) {
    if (*text == "auto") {
      *dimension_out = 0.0;
      *is_set_out = false;
      return true;
    }
    std::fprintf(stderr, "Invalid value for %s.\n", qualified_key);
    return false;
  }

  if (auto value = table->get_as<double>(key)) {
    if (*value <= 0.0) {
      std::fprintf(stderr, "Invalid value for %s.\n", qualified_key);
      return false;
    }
    *dimension_out = *value;
    *is_set_out = true;
  }
  return true;
}

}  // namespace

extern "C" int load_slice_config_file(const char *path, SliceOptions *options) {
  if (path == nullptr || options == nullptr) {
    return -1;
  }

  SliceOptions parsed = {};
  parsed.dpi = SLICE_DEFAULT_DPI;
  parsed.layer_height_nm = SLICE_DEFAULT_LAYER_HEIGHT_NM;
  parsed.multiplier = SLICE_DEFAULT_MULTIPLIER;
  parsed.x_in = SLICE_BASE_X_IN * SLICE_DEFAULT_MULTIPLIER;
  parsed.y_in = SLICE_BASE_Y_IN * SLICE_DEFAULT_MULTIPLIER;
  parsed.z_in = SLICE_BASE_Z_IN * SLICE_DEFAULT_MULTIPLIER;
  parsed.x_in_set = true;
  parsed.y_in_set = true;
  parsed.z_in_set = true;
  parsed.longest_side_in = SLICE_DEFAULT_LONGEST_SIDE_IN;
  parsed.voxel_radius_x_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.voxel_radius_x_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.voxel_radius_y_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.voxel_radius_y_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.voxel_radius_z_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.voxel_radius_z_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  parsed.padding_ratio = SLICE_DEFAULT_PADDING_RATIO;
  parsed.log_interval = SLICE_DEFAULT_LOG_INTERVAL;

  bool has_multiplier = false;
  bool has_x_in = false;
  bool has_y_in = false;
  bool has_z_in = false;
  bool has_radius_x_positive = false;
  bool has_radius_x_negative = false;
  bool has_radius_y_positive = false;
  bool has_radius_y_negative = false;
  bool has_radius_z_positive = false;
  bool has_radius_z_negative = false;

  try {
    const auto config = cpptoml::parse_file(path);
    const auto input = config->get_table("input");
    const auto output = config->get_table("output");
    const auto raster = config->get_table("raster");
    const auto physical = config->get_table("physical");
    const auto sampling = config->get_table_qualified("sampling.radius_inches");
    const auto logging = config->get_table("logging");

    if (input != nullptr) {
      if (auto value = input->get_as<std::string>("path")) {
        parsed.input_path = dup_string(*value);
      }
    }
    if (output != nullptr) {
      if (auto value = output->get_as<std::string>("directory")) {
        parsed.output_dir = dup_string(*value);
      }
    }
    if (auto value = config->get_as<std::string>("kd_cache")) {
      parsed.kd_cache_path = dup_string(*value);
    }

    if (!require_positive<double>(raster, "raster.dpi", "dpi", &parsed.dpi) ||
        !require_positive<double>(
            raster,
            "raster.layer_height_nm",
            "layer_height_nm",
            &parsed.layer_height_nm) ||
        !require_positive<double>(
            physical,
            "physical.longest_side_in",
            "longest_side_in",
            &parsed.longest_side_in) ||
        !require_non_negative(
            physical,
            "physical.padding_ratio",
            "padding_ratio",
            &parsed.padding_ratio) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.x_positive",
            "x_positive",
            &parsed.voxel_radius_x_positive_inches) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.x_negative",
            "x_negative",
            &parsed.voxel_radius_x_negative_inches) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.y_positive",
            "y_positive",
            &parsed.voxel_radius_y_positive_inches) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.y_negative",
            "y_negative",
            &parsed.voxel_radius_y_negative_inches) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.z_positive",
            "z_positive",
            &parsed.voxel_radius_z_positive_inches) ||
        !require_positive<double>(
            sampling,
            "sampling.radius_inches.z_negative",
            "z_negative",
            &parsed.voxel_radius_z_negative_inches) ||
        !parse_dimension(physical, "physical.x_in", "x_in", &parsed.x_in, &parsed.x_in_set) ||
        !parse_dimension(physical, "physical.y_in", "y_in", &parsed.y_in, &parsed.y_in_set) ||
        !parse_dimension(physical, "physical.z_in", "z_in", &parsed.z_in, &parsed.z_in_set)) {
      return -1;
    }

    if (physical != nullptr) {
      if (auto value = physical->get_as<double>("multiplier")) {
        if (*value <= 0.0) {
          std::fprintf(stderr, "Invalid value for physical.multiplier.\n");
          return -1;
        }
        parsed.multiplier = *value;
        has_multiplier = true;
      }
      has_x_in = physical->contains("x_in");
      has_y_in = physical->contains("y_in");
      has_z_in = physical->contains("z_in");
    }

    if (sampling != nullptr) {
      has_radius_x_positive = sampling->contains("x_positive");
      has_radius_x_negative = sampling->contains("x_negative");
      has_radius_y_positive = sampling->contains("y_positive");
      has_radius_y_negative = sampling->contains("y_negative");
      has_radius_z_positive = sampling->contains("z_positive");
      has_radius_z_negative = sampling->contains("z_negative");
    }

    if (logging != nullptr) {
      if (auto value = logging->get_as<int64_t>("interval")) {
        if (*value < 0) {
          std::fprintf(stderr, "Invalid value for logging.interval.\n");
          return -1;
        }
        parsed.log_interval = static_cast<uint64_t>(*value);
      }
    }
  } catch (const cpptoml::parse_exception &error) {
    std::fprintf(stderr, "Failed to parse %s: %s\n", path, error.what());
    return -1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Failed to load %s: %s\n", path, error.what());
    return -1;
  }

  if (has_multiplier) {
    if (!has_x_in && parsed.x_in_set) {
      parsed.x_in = SLICE_BASE_X_IN * parsed.multiplier;
    }
    if (!has_y_in && parsed.y_in_set) {
      parsed.y_in = SLICE_BASE_Y_IN * parsed.multiplier;
    }
    if (!has_z_in && parsed.z_in_set) {
      parsed.z_in = SLICE_BASE_Z_IN * parsed.multiplier;
    }

    const double default_radius = SLICE_DEFAULT_VOXEL_RADIUS_INCHES * parsed.multiplier;
    if (!has_radius_x_positive) {
      parsed.voxel_radius_x_positive_inches = default_radius;
    }
    if (!has_radius_x_negative) {
      parsed.voxel_radius_x_negative_inches = default_radius;
    }
    if (!has_radius_y_positive) {
      parsed.voxel_radius_y_positive_inches = default_radius;
    }
    if (!has_radius_y_negative) {
      parsed.voxel_radius_y_negative_inches = default_radius;
    }
    if (!has_radius_z_positive) {
      parsed.voxel_radius_z_positive_inches = default_radius;
    }
    if (!has_radius_z_negative) {
      parsed.voxel_radius_z_negative_inches = default_radius;
    }
  }

  *options = parsed;
  return 0;
}
