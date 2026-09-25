#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern int parser_error_pos;

/**
 * Initialize library path.
 */
void parser_init();

/**
 * Rebuild the library path after a BOSL2 update, keeping the OPENSCADPATH
 * entries parser_init() resolved.
 */
void refresh_library_path();

fs::path search_libs(const fs::path& localpath);
fs::path find_valid_path(const fs::path& sourcepath, const fs::path& localpath,
                         const std::vector<std::string> *openfilenames = nullptr);
fs::path get_library_for_path(const fs::path& localpath);

const std::vector<std::string>& get_library_path();