#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include "fix-dtv-realloc.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <limits.h>
#include <link.h>
#include <optional>
#include <regex>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

#define CHECK_EXCEPT_BEGIN try {
#define CHECK_EXCEPT_END                                                       \
  }                                                                            \
  catch (const std::exception &ex) {                                           \
    enabled = false;                                                           \
    fprintf(stderr, "[%s] uncaught exception: %s\n", __func__, ex.what());     \
  }

#define ELFW(x) ELFW_1(x, __ELF_NATIVE_CLASS)
#define ELFW_1(x, y) ELFW_2(x, y)
#define ELFW_2(x, y) ELF##x##y

namespace {
// When we want to ignore a path in la_objsearch, we should return this instead
// of nullptr, so that the search process will not be interrupted.
// See: https://github.com/bminor/glibc/blob/glibc-2.39/elf/dl-load.c#L1918
// If nullptr is returned from la_objsearch, but the parent directory of the
// original path exists, open_path has fd = -1, here_any = 1, errno = 0.
// In such cases, open_path would mistakenly think that there's a fatal error
// and return immediately, ignoring subsequent search items.
char non_existent_path[] = "/proc/-1";

std::optional<YAML::Node> cfg;
bool is_nix_rtld;
bool enabled = true;

struct search_state {
  YAML::Node rule;
  std::string lib_name;
  std::unordered_map<std::string, std::string> block_states;
  bool has_dt_runpath;
};

std::optional<search_state> cur_state;

std::string search_flag_to_str(unsigned flag) {
  switch (flag) {
  case LA_SER_ORIG:
    return "LA_SER_ORIG";
  case LA_SER_RUNPATH:
    return "LA_SER_RUNPATH";
  case LA_SER_LIBPATH:
    return "LA_SER_LIBPATH";
  case LA_SER_CONFIG:
    return "LA_SER_CONFIG";
  case LA_SER_DEFAULT:
    return "LA_SER_DEFAULT";
  case LA_SER_SECURE:
    return "LA_SER_SECURE";
  default:
    return std::to_string(flag);
  }
}

std::string expand_env(const std::string &s) {
  std::string ret;
  bool in_var_name;
  std::string var_name;
  for (size_t i = 0; i < s.size(); i++) {
    if (in_var_name) {
      if (var_name.empty()) {
        if (s[i] == '$') {
          ret += '$';
          in_var_name = false;
        } else if (s[i] == '{') {
          var_name += s[i];
        } else {
          break;
        }
      } else if (s[i] == '}') {
        // var_name[0] is '{'
        const char *env_c = getenv(var_name.c_str() + 1);
        if (env_c != nullptr) {
          ret += env_c;
        }
        var_name.clear();
        in_var_name = false;
      } else {
        var_name += s[i];
      }
    } else {
      if (s[i] == '$') {
        in_var_name = true;
      } else {
        ret += s[i];
      }
    }
  }
  if (in_var_name) {
    SPDLOG_WARN("invalid env substitution string: {}", s);
    return s;
  }
  return ret;
}

template <typename... Args>
std::string read_expandable_str(YAML::Node node, Args &&...args) {
  if (!node || node.IsScalar()) {
    return node.as<std::string>(std::forward<Args>(args)...);
  } else {
    auto s = node["value"].as<std::string>(std::forward<Args>(args)...);
    if (node["expand_env"].as<bool>(false)) {
      return expand_env(s);
    } else {
      return s;
    }
  }
}

bool is_fatal_err(const char *msg) {
  int err = errno;
  auto err_str = strerror(err);
  bool ret = err != ENOENT && err != ENOTDIR && err != EACCES;
  auto log_level = ret ? spdlog::level::err : spdlog::level::debug;
  SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), log_level, "{}: {} ({})",
                     msg, err_str, err);
  return ret;
}

// mimick the behavior of open_path and open_verify
// https://github.com/bminor/glibc/blob/glibc-2.39/elf/dl-load.c#L1918
// in case of fatal errors, we should return the path back to ld.so so that it
// can detect the error and abort the search process
bool try_path(const char *path) {
  SPDLOG_DEBUG("path={}", path);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return is_fatal_err("open failed");
  }
  ElfW(Ehdr) ehdr;
  size_t off = 0;
  while (off < sizeof(ehdr)) {
    ssize_t ret = read(fd, &ehdr, sizeof(ehdr) - off);
    if (ret == 0) {
      SPDLOG_ERROR("file too short");
      close(fd);
      return true;
    }
    if (ret < 0) {
      close(fd);
      return is_fatal_err("read error");
    }
    off += ret;
  }
  close(fd);

  if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
    SPDLOG_ERROR("invalid ELF magic number");
    return true;
  }

  if (ehdr.e_ident[EI_CLASS] != ELFW(CLASS)) {
    // multilib support
    SPDLOG_DEBUG("word size mismatch");
    return false;
  }

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  constexpr auto cur_data_enc = ELFDATA2LSB;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  constexpr auto cur_data_enc = ELFDATA2MSB;
#else
#error "unsupported byte order"
#endif
  if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
    SPDLOG_ERROR("byte order mismatch");
    return true;
  }

  if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
    SPDLOG_ERROR("ELF version mismatch");
    return true;
  }

  // https://github.com/bminor/glibc/blob/glibc-2.39/sysdeps/gnu/ldsodefs.h
  if (ehdr.e_ident[EI_OSABI] != ELFOSABI_SYSV &&
      ehdr.e_ident[EI_OSABI] != ELFOSABI_GNU) {
    SPDLOG_ERROR("OS ABI mismatch");
    return true;
  }
  if (!(ehdr.e_ident[EI_ABIVERSION] == 0 ||
        (ehdr.e_ident[EI_OSABI] == ELFOSABI_GNU &&
         // no way to retrieve LIBC_ABI_MAX at runtime
         ehdr.e_ident[EI_ABIVERSION] < 4))) {
    SPDLOG_ERROR("ABI version mismatch");
    return true;
  }

  for (int i = EI_PAD; i < EI_NIDENT; i++) {
    if (ehdr.e_ident[i]) {
      SPDLOG_ERROR("non-zero padding in e_ident");
      return true;
    }
  }

#ifdef __x86_64__
  constexpr auto cur_machine = EM_X86_64;
#elif __aarch64__
  constexpr auto cur_machine = EM_AARCH64;
#else
#error "unsupported architecture"
#endif
  if (ehdr.e_machine != cur_machine) {
    SPDLOG_DEBUG("arch mismatch");
    return false;
  }

  // we don't care about other fatal errors
  // the next and last non-fatal error check is elf_machine_reject_phdr_p,
  // which is only defined for MIPS

  return true;
}

__attribute__((constructor)) void init() {
  CHECK_EXCEPT_BEGIN

  spdlog::set_default_logger(spdlog::stderr_color_st("ld-audit-search-mod"));
  spdlog::set_pattern("[%!] %v");

  // load config
  auto cfg_path = getenv("LD_AUDIT_SEARCH_MOD_CONFIG");
  if (cfg_path) {
    cfg = YAML::LoadFile(cfg_path);
  }
  spdlog::set_level(
      spdlog::level::from_str((*cfg)["log_level"].as<std::string>("warning")));
  SPDLOG_DEBUG("cfg_path={}", cfg_path);

  is_nix_rtld = dl_iterate_phdr(
      [](dl_phdr_info *info, size_t, void *) -> int {
        CHECK_EXCEPT_BEGIN
        std::filesystem::path name = info->dlpi_name;
        if (name.is_relative()) {
          return 0;
        }
        name = std::filesystem::weakly_canonical(name);
        auto nix_store_dir = std::filesystem::weakly_canonical(NIX_STORE_DIR);
        if (std::mismatch(nix_store_dir.begin(), nix_store_dir.end(),
                          name.begin(), name.end())
                    .first == nix_store_dir.end() &&
            name.filename() == NIX_RTLD_NAME) {
          SPDLOG_DEBUG("nix rtld found: {}", name.string());
          return 1;
        }
        CHECK_EXCEPT_END
        return 0;
      },
      nullptr);

  // rtld loads audit modules in separate namespaces, where the main executable
  // is not visible.
  // To find out if we are loaded as an audit module, we check if the main
  // executable is missing in the current link namespace.
  // The executable is distinguishable from normal shared libraries because its
  // dlpi_name is an empty string, as documented in dl_iterate_phdr(3).
  // Theoretically, we can also get the ID of the current link namespace using
  // dlinfo. However, calling dlfcn functions in a constructor function of an
  // audit module causes crashes on old versions of glibc.
  bool is_auditing = !dl_iterate_phdr(
      [](dl_phdr_info *info, size_t, void *) -> int {
        return strcmp(info->dlpi_name, "") == 0;
      },
      nullptr);

  if (!enabled || is_auditing) {
    return;
  }

  fix_dtv_realloc();

  std::map<std::string, std::optional<std::string>> env_data;
  std::string setenv_prefix = "LASM_SETENV_";
  std::string unsetenv_prefix = "LASM_UNSETENV_";
  for (size_t i = 0; environ[i] != nullptr; i++) {
    std::string data = environ[i];
    auto eq_pos = data.find("=");
    if (eq_pos == std::string::npos) {
      continue;
    }
    auto name = data.substr(0, eq_pos);
    if (name.compare(0, setenv_prefix.size(), setenv_prefix) == 0) {
      env_data.emplace(name.substr(setenv_prefix.size()),
                       data.substr(eq_pos + 1));
    } else if (name.compare(0, unsetenv_prefix.size(), unsetenv_prefix) == 0) {
      env_data.emplace(name.substr(unsetenv_prefix.size()), std::nullopt);
    }
  }
  for (auto &&x : env_data) {
    auto name = (x.second ? setenv_prefix : unsetenv_prefix) + x.first;
    SPDLOG_DEBUG("unsetenv {}", name);
    unsetenv(name.c_str());
  }

  auto exe_name = std::filesystem::read_symlink("/proc/self/exe");
  SPDLOG_DEBUG("executable name: {}", exe_name.c_str());
  for (auto env_rule : (*cfg)["env"]) {
    if (auto rtld_type = env_rule["cond"]["rtld"].as<std::string>("any");
        !(rtld_type == "nix" && is_nix_rtld ||
          rtld_type == "normal" && !is_nix_rtld || rtld_type == "any")) {
      continue;
    }
    if (!std::regex_match(
            exe_name.c_str(),
            std::regex(env_rule["cond"]["exe"].as<std::string>(".*")))) {
      continue;
    }

    for (auto setenv_node : env_rule["setenv"]) {
      auto name = setenv_node.first.as<std::string>();

      std::string old_value;
      bool has_old_value = false;
      if (auto it = env_data.find(name); it != env_data.end()) {
        has_old_value = bool(it->second);
        if (has_old_value) {
          old_value = *it->second;
        }
      } else if (auto ptr = getenv(name.c_str()); ptr != nullptr) {
        has_old_value = true;
        old_value = ptr;
      }

      std::stringstream ss;
      if (setenv_node.second.IsScalar()) {
        ss << setenv_node.second;
      } else {
        auto type = setenv_node.second["type"].as<std::string>("set");
        auto splitter = setenv_node.second["splitter"].as<std::string>(":");
        auto new_value = read_expandable_str(setenv_node.second);
        if (type == "prepend") {
          ss << new_value;
          if (old_value.size() > 0 && old_value.front() != splitter[0]) {
            ss << splitter;
          }
          ss << old_value;
        } else if (type == "append") {
          ss << old_value;
          if (old_value.size() > 0 && old_value.back() != splitter[0]) {
            ss << splitter;
          }
          ss << new_value;
        } else if (type == "set") {
          ss << new_value;
        }
      }

      auto value = ss.str();
      SPDLOG_DEBUG("setenv {}={}", name, value);
      env_data[name] = value;
      if (has_old_value) {
        setenv((setenv_prefix + name).c_str(), old_value.c_str(), true);
      } else {
        setenv((unsetenv_prefix + name).c_str(), "1", true);
      }
    }

    for (auto unsetenv_node : env_rule["unsetenv"]) {
      auto name = unsetenv_node.as<std::string>();
      SPDLOG_DEBUG("unsetenv {}", name);
      env_data[name] = std::nullopt;
      setenv((unsetenv_prefix + name).c_str(), "1", true);
    }
  }

  for (auto &&x : env_data) {
    if (x.second) {
      setenv(x.first.c_str(), x.second->c_str(), true);
    } else {
      unsetenv(x.first.c_str());
    }
  }

  CHECK_EXCEPT_END
}
} // namespace

extern "C" {
unsigned la_version(unsigned version) {
  // current version is 2, version 1 only differs in la_symbind
  // https://github.com/bminor/glibc/commit/32612615c58b394c3eb09f020f31310797ad3854
  unsigned ret;
  if (version <= 2) {
    ret = version;
  } else {
    ret = LAV_CURRENT;
  }

  if (!enabled) {
    // returning 0 causes crashes on old versions of glibc
    // https://sourceware.org/bugzilla/show_bug.cgi?id=24122
    return ret;
  }

  SPDLOG_DEBUG("version={}", version);
  return ret;
}

char *la_objsearch(const char *name_const, uintptr_t *cookie,
                   unsigned int flag) {
  // it has to be non-const when returned
  auto name = const_cast<char *>(name_const);
  if (!enabled) {
    return name;
  }

  CHECK_EXCEPT_BEGIN

  SPDLOG_DEBUG("cookie={} flag={} name={}", fmt::ptr(cookie),
               search_flag_to_str(flag), name_const);

  // initialization
  if (flag == LA_SER_ORIG) {
    cur_state.emplace();

    // An undocumented way to retrieve link_map of the dependent library.
    // https://github.com/bminor/glibc/blob/glibc-2.39/elf/dl-object.c#L141

    // The cookie is obtained from the "loader" argument of _dl_map_object.
    // However, the argument will be NULL if all of the following conditions are
    // met:
    // - There are no '$' signs in the file name. (e.g. $ORIGIN)
    // - The library is requested by dlmopen and will be loaded into the
    // specified namespace rather than the same one as the caller.
    // - There are '/' characters in the file name, which means the path will be
    // used directly, skipping the search process.
    // In old versions of glibc, there's no null check before retrieving the
    // cookie, so the cookie passed to us will be an invalid pointer very close
    // to NULL.
    // Since glibc 2.35, a null check has been added as part of a refactor
    // commit, and the call to la_objsearch will be simply skipped in such
    // cases. This might not be the desired behavior, but at least does not
    // cause problems here.
    // See:
    // https://github.com/bminor/glibc/blob/glibc-2.39/elf/dl-open.c#L545
    // https://github.com/bminor/glibc/commit/c91008d3490e4e3ce29520068405f081f0d368ca#diff-ef795de39b8938f8a53c695ad13dc97ca639a93eaf18a770745b3633670490acR50
    // The maximum value of bad cookies should be sizeof(link_map) +
    // sizeof(auditstate) * DL_NNS. Anyway we just test against some larger
    // value here, as any value that low should not be a valid pointer on a
    // sanely configured system.
    const char *dep_lib_path;
    if ((uintptr_t)cookie < 65536) {
      // empty string "" indicates the main executable, just put something else
      // that can't be a file path here
      dep_lib_path = "/";
      SPDLOG_DEBUG("invalid cookie, can't read loader link_map");
    } else {
      auto lm = *(const link_map *const *)cookie;
      dep_lib_path = lm->l_name;
      cur_state->has_dt_runpath = false;
      for (auto dyn = lm->l_ld; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_RUNPATH) {
          cur_state->has_dt_runpath = true;
        }
      }
      SPDLOG_DEBUG("l_name={} has_dt_runpath={}", lm->l_name,
                   cur_state->has_dt_runpath);
    }

    cur_state->rule.reset(YAML::Node(YAML::NodeType::Undefined));
    auto rules = (*cfg)["rules"];
    std::cmatch lib_match_result;
    for (size_t i = 0; i < rules.size(); i++) {
      auto rule = rules[i];
      auto rtld_type = rule["cond"]["rtld"].as<std::string>("any");
      if (!(rtld_type == "nix" && is_nix_rtld ||
            rtld_type == "normal" && !is_nix_rtld || rtld_type == "any")) {
        continue;
      }
      if (!std::regex_match(
              name, lib_match_result,
              std::regex(read_expandable_str(rule["cond"]["lib"], ".*")))) {
        continue;
      }
      if (!std::regex_match(dep_lib_path,
                            std::regex(read_expandable_str(
                                rule["cond"]["dependent_lib"], ".*")))) {
        continue;
      }
      SPDLOG_DEBUG("rule {} matched", i);
      cur_state->rule.reset(rule);
      break;
    }

    if (cur_state->rule) {
      if (auto rename_node = cur_state->rule["rename"]; rename_node) {
        cur_state->lib_name =
            lib_match_result.format(rename_node.as<std::string>());
      } else {
        cur_state->lib_name = name;
      }
      return cur_state->lib_name.data();
    } else {
      SPDLOG_DEBUG("no matching rule, skipping");
      return name;
    }
  }

  if (!cur_state->rule) {
    return name;
  }

  std::string rule_block_name;
  switch (flag) {
  case LA_SER_RUNPATH:
    rule_block_name = cur_state->has_dt_runpath ? "runpath" : "rpath";
    break;
  case LA_SER_LIBPATH:
    rule_block_name = "libpath";
    break;
  case LA_SER_CONFIG:
    rule_block_name = "config";
    break;
  case LA_SER_DEFAULT:
    rule_block_name = "default";
    break;
  }
  YAML::Node cur_rule_block;
  if (rule_block_name.empty() ||
      !((cur_rule_block = cur_state->rule[rule_block_name]))) {
    SPDLOG_DEBUG("rule block {} not found, skipping", rule_block_name);
    return name;
  }

  if (cur_state->block_states.emplace(rule_block_name, "").second) {
    for (auto prepend_node : cur_rule_block["prepend"]) {
      auto saved_node = prepend_node["saved"];
      if (saved_node) {
        auto saved_from = saved_node.as<std::string>();
        SPDLOG_DEBUG("saved_from={}", saved_from);
        auto it = cur_state->block_states.find(saved_from);
        if (saved_from == rule_block_name ||
            it == cur_state->block_states.end()) {
          SPDLOG_DEBUG("block {} not searched yet", saved_from);
          continue;
        }
        if (it->second.empty()) {
          SPDLOG_DEBUG("block {} has no saved path", saved_from);
          continue;
        }
        SPDLOG_DEBUG("returning saved path: {}", it->second);
        return it->second.data();
      }

      std::string file_path;
      if (auto file_node = prepend_node["file"]; file_node) {
        file_path = read_expandable_str(file_node);
      } else if (auto dir_node = prepend_node["dir"]; dir_node) {
        std::filesystem::path dir_path = read_expandable_str(dir_node);
        file_path = dir_path / cur_state->lib_name;
      } else {
        continue;
      }
      if (try_path(file_path.c_str())) {
        return ((cur_state->block_states[rule_block_name] = file_path)).data();
      }
    }
  }

  auto filters = cur_rule_block["filter"];
  for (size_t i = 0; i < filters.size(); i++) {
    auto filter = filters[i];
    if (auto inc_node = filter["include"];
        inc_node &&
        std::regex_match(name, std::regex(read_expandable_str(inc_node)))) {
      SPDLOG_DEBUG("filter {} matched", i);
      break;
    }
    if (auto exc_node = filter["exclude"];
        exc_node &&
        std::regex_match(name, std::regex(read_expandable_str(exc_node)))) {
      SPDLOG_DEBUG("filter {} matched", i);
      return non_existent_path;
    }
  }

  if (cur_rule_block["save"].as<bool>(false)) {
    if (try_path(name)) {
      if (auto &saved_path = cur_state->block_states[rule_block_name];
          saved_path.empty()) {
        SPDLOG_DEBUG("saving matched path: {}", name);
        saved_path = name;
      }
    }
    return non_existent_path;
  }

  CHECK_EXCEPT_END
  return name;
}
}
