#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bonk/conformance/conformance.hpp"

namespace bonk::conformance::detail {

bool directory_exists(std::string_view path) {
  std::error_code ignored;
  return std::filesystem::is_directory(std::filesystem::path(std::string(path)), ignored);
}

std::vector<std::string> list_files(std::string_view directory, std::string_view suffix, bool recursive) {
  std::vector<std::string> out;
  std::error_code ignored;
  const std::filesystem::path root{std::string(directory)};
  if (!std::filesystem::is_directory(root, ignored)) return out;
  if (recursive) {
    for (auto it = std::filesystem::recursive_directory_iterator(root, ignored);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ignored)) {
      if (ignored) break;
      if (!it->is_regular_file()) continue;
      const std::string path = it->path().string();
      if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out.push_back(path);
      }
    }
  } else {
    for (auto it = std::filesystem::directory_iterator(root, ignored); it != std::filesystem::directory_iterator();
         it.increment(ignored)) {
      if (ignored) break;
      if (!it->is_regular_file()) continue;
      const std::string path = it->path().string();
      if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out.push_back(path);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string read_file(std::string_view path) {
  std::ifstream file{std::string(path), std::ios::binary};
  if (!file) return {};
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string collapse(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool space = false;
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

}  // namespace bonk::conformance::detail
