// Copyright (c) 2019 Tim Perkins

// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the “Software”), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
// to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.

// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
// FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#include "trollauncher/launcher_profiles_editor.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "trollauncher/error_codes.hpp"
#include "trollauncher/utils.hpp"

#ifndef ITS_A_UNIX_SYSTEM
#ifndef _WIN32
#define ITS_A_UNIX_SYSTEM true
#else
#define ITS_A_UNIX_SYSTEM false
#endif
#endif

namespace tl {

namespace {

namespace fs = std::filesystem;
namespace nl = nlohmann;

std::string GetCurrentTimeAsString(
    std::optional<std::chrono::seconds> time_modifier_opt = std::nullopt);
bool IsFileWritable(const fs::path& path);
fs::path AddFilenamePrefix(const fs::path& path, const std::string& prefix);
bool WriteLauncherProfilesJson(const fs::path& launcher_profiles_path,
                               const nl::json& new_launcher_profiles_json, std::error_code* ec);

}  // namespace

struct LauncherProfilesEditor::Data_ {
  fs::path launcher_profiles_path;
  nl::json launcher_profiles_json;
};

LauncherProfilesEditor::LauncherProfilesEditor()
    : data_(std::make_unique<LauncherProfilesEditor::Data_>())
{
  // Do nothing
}

LauncherProfilesEditor::Ptr LauncherProfilesEditor::Create(
    const std::filesystem::path& launcher_profiles_path, std::error_code* ec)
{
  auto lpe_ptr = Ptr(new LauncherProfilesEditor());
  lpe_ptr->data_->launcher_profiles_path = launcher_profiles_path;
  lpe_ptr->data_->launcher_profiles_json = nl::json::object();
  if (!lpe_ptr->Refresh(ec)) {
    return nullptr;
  }
  return lpe_ptr;
}

bool LauncherProfilesEditor::Refresh(std::error_code* ec)
{
  if (!fs::exists(data_->launcher_profiles_path)) {
    SetError(ec, Error::LAUNCHER_PROFILES_NONEXISTENT);
    return false;
  }
  std::ifstream launcher_profiles_ifs(data_->launcher_profiles_path);
  nl::json new_launcher_profiles_json = nl::json::parse(launcher_profiles_ifs, nullptr, false);
  launcher_profiles_ifs.close();
  if (new_launcher_profiles_json.is_discarded()) {
    SetError(ec, Error::LAUNCHER_PROFILES_PARSE_FAILED);
    return false;
  }
  data_->launcher_profiles_json = new_launcher_profiles_json;
  return true;
}

bool LauncherProfilesEditor::HasProfileWithId(const std::string& id) const
{
  const nl::json profiles_json =
      data_->launcher_profiles_json.value("profiles", nl::json::object());
  for (const auto& [profile_id, _] : profiles_json.items()) {
    if (profile_id == id) {
      return true;
    }
  }
  return false;
}

bool LauncherProfilesEditor::HasProfileWithName(const std::string& name) const
{
  const nl::json profiles_json =
      data_->launcher_profiles_json.value("profiles", nl::json::object());
  for (const auto& [profile_id, profile_json] : profiles_json.items()) {
    const nl::json profile_name = profile_json.value("name", nl::json(nullptr));
    if (profile_name.is_string() && profile_name.get<std::string>() == name) {
      return true;
    }
  }
  return false;
}

std::string LauncherProfilesEditor::GetNewUniqueId() const
{
  // There's an extremely slim chance it's not actually unique
  std::string id = GetRandomId();
  for (std::size_t ii = 0; HasProfileWithId(id) && ii < 1000; ++ii) {
    id = GetRandomId();
  }
  return id;
}

std::string LauncherProfilesEditor::GetNewUniqueName() const
{
  // There's an extremely slim chance it's not actually unique
  std::string name = GetRandomName();
  for (std::size_t ii = 0; HasProfileWithName(name) && ii < 1000; ++ii) {
    name = GetRandomName();
  }
  return name;
}

bool LauncherProfilesEditor::PatchForgeProfile(std::error_code* ec)
{
  if (!Refresh(ec)) {
    return false;
  }
  nl::json forge_profile =
      data_->launcher_profiles_json["profiles"].value("forge", nl::json(nullptr));
  if (!forge_profile.is_object()) {
    SetError(ec, Error::LAUNCHER_PROFILES_NO_FORGE_PROFILE);
    return false;
  }
  // Add a "lastUsed" time because Forge is lazy and doesn't do this!
  forge_profile["lastUsed"] = GetCurrentTimeAsString(std::chrono::seconds(-1));
  nl::json new_launcher_profiles_json = data_->launcher_profiles_json;
  new_launcher_profiles_json["profiles"]["forge"] = forge_profile;
  if (!WriteLauncherProfilesJson(data_->launcher_profiles_path, new_launcher_profiles_json, ec)) {
    return false;
  }
  return true;
}

bool LauncherProfilesEditor::WriteProfile(const std::string& id, const std::string& name,
                                          const std::string& icon, const std::string& version,
                                          const fs::path& game_path,
                                          const std::optional<fs::path>& java_path_opt,
                                          std::error_code* ec)
{
  if (!Refresh(ec)) {
    return false;
  }
  if (HasProfileWithId(id)) {
    SetError(ec, Error::LAUNCHER_PROFILES_ID_USED);
    return false;
  }
  if (HasProfileWithName(name)) {
    SetError(ec, Error::LAUNCHER_PROFILES_NAME_USED);
    return false;
  }
  // Formatting example (as of format 21):
  // "mjrianz5n6o0ntue4gvzfu9zi7i8lg4y": {
  //   "created": "2019-12-12T03:11:18.000Z",
  //   "gameDir" : "/home/tim/.minecraft/trollauncher/Adakite 58",
  //   "icon": "TNT",
  //   "javaDir" : "/usr/lib/jvm/java-8-openjdk-amd64/bin/java",
  //   "lastUsed": "2019-12-12T03:11:18.000Z",
  //   "lastVersionId": "1.14.4-forge-28.1.106",
  //   "name": "Adakite 58",
  //   "type": "custom"
  // },
  const nl::json java_path =
      (java_path_opt ? nl::json(java_path_opt.value().string()) : nl::json(nullptr));
  const std::string current_time = GetCurrentTimeAsString();
  const nl::json new_profile_json = {
      {"created", current_time},        // The current time
      {"gameDir", game_path.string()},  // The directory of the modpack
      {"icon", icon},                   // The icon (such as "TNT")
      {"javaDir", java_path},           // The path to the "java" executable
      {"lastUsed", current_time},       // Also the current time
      {"lastVersionId", version},       // The Forge version string
      {"name", name},                   // The name
      {"type", "custom"},               // The type (always "custom")
  };
  nl::json new_launcher_profiles_json = data_->launcher_profiles_json;
  new_launcher_profiles_json["profiles"][id] = new_profile_json;
  if (!WriteLauncherProfilesJson(data_->launcher_profiles_path, new_launcher_profiles_json, ec)) {
    return false;
  }
  return true;
}

namespace {

std::string GetCurrentTimeAsString(std::optional<std::chrono::seconds> time_modifier_opt)
{
  const auto now_chrono = std::chrono::system_clock::now();
  std::time_t now_time_t = std::chrono::system_clock::to_time_t(
      now_chrono + time_modifier_opt.value_or(std::chrono::seconds(0)));
  char time_c_str[sizeof "0000-00-00T00:00:00.000Z"];
  std::strftime(time_c_str, sizeof time_c_str, "%FT%T.000Z", std::gmtime(&now_time_t));
  return time_c_str;
}

bool IsFileWritable(const fs::path& path)
{
  if (!fs::is_regular_file(path)) {
    return false;
  }
  std::ofstream file(path, std::ios_base::app);
  return file.good();
}

fs::path AddFilenamePrefix(const fs::path& path, const std::string& prefix)
{
  const fs::path new_filename = fs::path(prefix) += path.filename();
  return fs::path(path).replace_filename(new_filename);
}

bool WriteLauncherProfilesJson(const fs::path& launcher_profiles_path,
                               const nl::json& new_launcher_profiles_json, std::error_code* ec)
{
  if (!IsFileWritable(launcher_profiles_path)) {
    SetError(ec, Error::LAUNCHER_PROFILES_NOT_WRITABLE);
    return false;
  }
  const std::string new_launcher_profiles_txt =
      new_launcher_profiles_json.dump(2, ' ', false, nl::json::error_handler_t::replace);
  const fs::path orig_lp_path = launcher_profiles_path;
  const fs::path backup_lp_path = AddFilenamePrefix(orig_lp_path, "backup_");
  const fs::path new_lp_path = AddFilenamePrefix(orig_lp_path, "new_");
  std::error_code fs_ec;
  if (!ITS_A_UNIX_SYSTEM) {
    // Apparently overwrite doesn't work on Windoze!
    fs::remove(backup_lp_path, fs_ec);
  }
  fs::copy_file(orig_lp_path, backup_lp_path, fs::copy_options::overwrite_existing, fs_ec);
  if (fs_ec) {
    SetError(ec, Error::LAUNCHER_PROFILES_BACKUP_FAILED);
    return false;
  }
  std::ofstream new_launcher_profiles_file(new_lp_path);
  if (!new_launcher_profiles_file.good()) {
    SetError(ec, Error::LAUNCHER_PROFILES_WRITE_FAILED);
    return false;
  }
  new_launcher_profiles_file << new_launcher_profiles_txt;
  new_launcher_profiles_file.close();
  if (!ITS_A_UNIX_SYSTEM) {
    // Apparently overwrite doesn't work on Windoze!
    fs::remove(orig_lp_path, fs_ec);
  }
  fs::copy_file(new_lp_path, orig_lp_path, fs::copy_options::overwrite_existing, fs_ec);
  if (fs_ec) {
    SetError(ec, Error::LAUNCHER_PROFILES_WRITE_FAILED);
    return false;
  }
  fs::remove(new_lp_path, fs_ec);
  return true;
}

}  // namespace

}  // namespace tl