#ifndef STORAGE_LAYOUT_H
#define STORAGE_LAYOUT_H

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "config.h"
#include "debug_tool.h"

namespace cfd::storage {
namespace fs = std::filesystem;

inline fs::path storage_root() { return fs::path(cfd::TEST_INFO_PATH); }

inline fs::path dataset_root() { return storage_root() / "datasets"; }

inline fs::path analysis_root() { return storage_root() / "analysis"; }

inline std::string path_string(fs::path path) {
  path = path.lexically_normal();
  path.make_preferred();
  return path.string();
}

inline void ensure_directory(const fs::path& dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
}

inline void ensure_parent_directory(const fs::path& file_path) {
  if (file_path.has_parent_path()) {
    ensure_directory(file_path.parent_path());
  }
}

inline fs::path analysis_batch_dir(const std::string& run_tag) {
  fs::path batch_dir = analysis_root() / run_tag;
  ensure_directory(batch_dir);
  return batch_dir;
}

inline fs::path analysis_compare_dir(const std::string& run_tag) {
  fs::path compare_dir = analysis_batch_dir(run_tag) / "comparison_reports";
  ensure_directory(compare_dir);
  return compare_dir;
}

inline fs::path analysis_retry_dir(const std::string& run_tag) {
  fs::path retry_dir = analysis_batch_dir(run_tag) / "retry_probability_reports";
  ensure_directory(retry_dir);
  return retry_dir;
}

inline bool starts_with_dir(const fs::path& path, const std::string& dir_name) {
  const auto it = path.begin();
  return it != path.end() && it->string() == dir_name;
}

inline std::vector<fs::path> dataset_candidates(const fs::path& base_path) {
  if (base_path.has_extension()) {
    return {base_path};
  }

  return {
      fs::path(base_path.string() + "_tab.txt"),
      fs::path(base_path.string() + ".txt"),
      fs::path(base_path.string() + ".csv"),
      fs::path(base_path.string() + ".json"),
  };
}

inline std::string resolve_dataset_input_path(const std::string& dataset_file) {
  fs::path input_path(dataset_file);
  std::vector<fs::path> search_roots;

  if (input_path.is_absolute()) {
    search_roots.push_back(input_path);
  } else {
    search_roots.push_back(storage_root() / input_path);
    if (!starts_with_dir(input_path, "datasets")) {
      search_roots.push_back(dataset_root() / input_path);
    }
  }

  for (const auto& search_root : search_roots) {
    for (const auto& candidate : dataset_candidates(search_root)) {
      if (fs::exists(candidate)) {
        return path_string(candidate);
      }
    }
  }

  if (!search_roots.empty()) {
    return path_string(dataset_candidates(search_roots.front()).front());
  }
  return path_string(dataset_candidates(dataset_root() / ("msg_" + get_time_stamp())).front());
}

inline std::string dataset_output_path(const std::string& dataset_file) {
  fs::path output_path;
  if (dataset_file.empty()) {
    output_path = dataset_root() / ("msg_" + get_time_stamp() + "_tab.txt");
  } else {
    const fs::path input_path(dataset_file);
    if (input_path.is_absolute()) {
      output_path = input_path;
    } else if (starts_with_dir(input_path, "datasets")) {
      output_path = storage_root() / input_path;
    } else {
      output_path = dataset_root() / input_path;
    }

    if (!output_path.has_extension()) {
      output_path += ".txt";
    }
  }

  ensure_parent_directory(output_path);
  return path_string(output_path);
}

inline std::string dataset_tag_from_file(const std::string& dataset_file) {
  std::string tag = fs::path(resolve_dataset_input_path(dataset_file)).stem().string();
  if (tag.rfind("msg_", 0) == 0) {
    tag.erase(0, 4);
  }
  constexpr std::string_view kLegacySuffix = "_tab";
  if (tag.size() > kLegacySuffix.size() &&
      tag.compare(tag.size() - kLegacySuffix.size(), kLegacySuffix.size(), kLegacySuffix) == 0) {
    tag.erase(tag.size() - kLegacySuffix.size());
  }
  return tag;
}

inline std::string compare_report_path(const std::string& run_tag, const std::string& dataset_tag) {
  return path_string(analysis_compare_dir(run_tag) / (dataset_tag + ".txt"));
}

inline std::string retry_report_path(const std::string& run_tag, const std::string& dataset_tag) {
  return path_string(analysis_retry_dir(run_tag) / (dataset_tag + ".txt"));
}

inline std::string compare_summary_report_path(const std::string& run_tag) {
  return path_string(analysis_batch_dir(run_tag) / "compare_summary_tab.txt");
}

inline std::string normalize_retry_report_output_path(const std::string& output_hint) {
  if (output_hint.empty()) {
    return retry_report_path(get_time_stamp(), "ad_hoc");
  }

  fs::path output_path(output_hint);
  if (output_path.is_absolute() || output_path.has_parent_path() || output_path.has_extension()) {
    if (!output_path.is_absolute()) {
      output_path = storage_root() / output_path;
    }
    if (!output_path.has_extension()) {
      output_path += ".txt";
    }
    ensure_parent_directory(output_path);
    return path_string(output_path);
  }

  return retry_report_path(get_time_stamp(), output_hint);
}

}  // namespace cfd::storage

#endif  // STORAGE_LAYOUT_H
