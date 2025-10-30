// amdgpu_trace_cpp.cpp
// Build (Linux): g++ -std=c++17 amdgpu_tracer.cpp -lamd-dbgapi -lelf -o amdgpu_trace_cpp
// Usage: ./amdgpu_trace_cpp <pid> "my_kernel" <max_steps=0>
// example usage: ./amdgpu_trace_cpp <exe|pid> <kernel_symbol> <max_steps> out=/tmp/trace.jsonl [args...]


#include <amd-dbgapi/amd-dbgapi.h>
#include <hsa/hsa.h>
#include <hsa/hsa_api_trace.h>
#include <hsa/hsa_ext_amd.h>

#include <elf.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <spawn.h>

extern char **environ;

// ---- Platform memory IO (Linux) ----
static bool read_process_mem_linux(pid_t pid, uint64_t addr, void* buf, size_t len) {
  struct iovec local{buf, len}, remote{(void*)addr, len};
  ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
  if (n == (ssize_t)len) return true;
  // Fallback: /proc/<pid>/mem (requires ptrace attach or CAP_SYS_PTRACE)
  char path[64]; std::snprintf(path, sizeof(path), "/proc/%d/mem", pid);
  int fd = ::open(path, O_RDONLY);
  if (fd < 0) return false;
  if (::lseek(fd, (off_t)addr, SEEK_SET) < 0) { ::close(fd); return false; }
  ssize_t rn = ::read(fd, buf, len);
  ::close(fd);
  return rn == (ssize_t)len;
}

static bool write_process_mem_linux(pid_t pid, uint64_t addr, const void* buf, size_t len) {
  struct iovec local{(void*)buf, len}, remote{(void*)addr, len};
  ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
  if (n == (ssize_t)len) return true;
  char path[64]; std::snprintf(path, sizeof(path), "/proc/%d/mem", pid);
  int fd = ::open(path, O_WRONLY);
  if (fd < 0) return false;
  if (::lseek(fd, (off_t)addr, SEEK_SET) < 0) { ::close(fd); return false; }
  ssize_t wn = ::write(fd, buf, len);
  ::close(fd);
  return wn == (ssize_t)len;
}

// ---- dbgapi callbacks ----
static void* cb_malloc(size_t n) { return std::malloc(n); }
static void cb_free(void* p) { std::free(p); }

static amd_dbgapi_status_t cb_client_process_get_info(
  amd_dbgapi_client_process_id_t, amd_dbgapi_client_process_info_t q, void* v)
{
  if (q == AMD_DBGAPI_CLIENT_PROCESS_INFO_NAME) {
    static const char* name = "amdgpu_trace_cpp";
    *(const char**)v = name;
    return AMD_DBGAPI_STATUS_SUCCESS;
  }
  return AMD_DBGAPI_STATUS_ERROR_NOT_SUPPORTED;
}

// Global: target pid for memory xfer
static pid_t g_pid = -1;

static amd_dbgapi_status_t cb_xfer_global_memory(
  amd_dbgapi_client_process_id_t, amd_dbgapi_global_address_t global_address,
  amd_dbgapi_size_t* value_size, void* read_buffer, const void* write_buffer)
{
  if (!value_size) return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT;
  if ((read_buffer != nullptr) == (write_buffer != nullptr))
    return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT_COMPATIBILITY;

  size_t len = *value_size;
  bool ok = false;
  if (read_buffer)
    ok = read_process_mem_linux(g_pid, global_address, read_buffer, len);
  else
    ok = write_process_mem_linux(g_pid, global_address, write_buffer, len);

  return ok ? AMD_DBGAPI_STATUS_SUCCESS : AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
}

static void cb_log(amd_dbgapi_log_level_t level, const char* msg) {
  if (level <= AMD_DBGAPI_LOG_LEVEL_WARNING) std::fprintf(stderr, "%s\n", msg);
}

// ---- Helpers ----
static void check(amd_dbgapi_status_t s, const char* what) {
  if (s != AMD_DBGAPI_STATUS_SUCCESS) {
    std::fprintf(stderr, "Error: %s failed (%d)\n", what, s);
    std::exit(1);
  }
}

// Query code objects via dbgapi (you’ll need the exact API names from your dbgapi version).
// Pseudocode signature; replace with your amd-dbgapi version’s code object listing APIs.
struct CodeObjectInfo {
  uint64_t load_base;
  std::string file_path;
};

// TODO: implement using amd_dbgapi_process_code_object_list + amd_dbgapi_code_object_get_info
static std::vector<CodeObjectInfo> list_code_objects(amd_dbgapi_process_id_t process) {
  std::vector<CodeObjectInfo> out;
  // Fill with {load_base, file_path} for each code object
  return out;
}

// ELF symbol lookup: find "my_kernel", return load_base + sym_value
static std::optional<uint64_t> resolve_kernel_symbol(const CodeObjectInfo& co, const char* name) {
  int fd = ::open(co.file_path.c_str(), O_RDONLY);
  if (fd < 0) return std::nullopt;

  // Minimal ELF64 reader (no error hardening). Prefer libelf/libbfd in production.
  Elf64_Ehdr eh{}; ::read(fd, &eh, sizeof(eh));
  if (!(eh.e_ident[0]==0x7f && eh.e_ident[1]=='E' && eh.e_ident[2]=='L' && eh.e_ident[3]=='F')) {
    ::close(fd); return std::nullopt;
  }
  ::lseek(fd, eh.e_shoff, SEEK_SET);
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  ::read(fd, sh.data(), eh.e_shnum * sizeof(Elf64_Shdr));

  // Read section header string table
  Elf64_Shdr shstr = sh[eh.e_shstrndx];
  std::vector<char> shstrtab(shstr.sh_size);
  ::lseek(fd, shstr.sh_offset, SEEK_SET);
  ::read(fd, shstrtab.data(), shstr.sh_size);

  // Find symtab or dynsym and corresponding strtab
  int symidx = -1, stridx = -1;
  for (size_t i=0;i<sh.size();++i) {
    const char* sname = &shstrtab[sh[i].sh_name];
    if (sh[i].sh_type == SHT_SYMTAB) { symidx = (int)i; }
    if (sh[i].sh_type == SHT_STRTAB && std::strcmp(sname,".strtab")==0) { stridx = (int)i; }
  }
  if (symidx < 0 || stridx < 0) { ::close(fd); return std::nullopt; }

  // Load strtab
  std::vector<char> strtab(sh[stridx].sh_size);
  ::lseek(fd, sh[stridx].sh_offset, SEEK_SET);
  ::read(fd, strtab.data(), strtab.size());

  // Scan symbols
  size_t nsyms = sh[symidx].sh_size / sizeof(Elf64_Sym);
  std::vector<Elf64_Sym> syms(nsyms);
  ::lseek(fd, sh[symidx].sh_offset, SEEK_SET);
  ::read(fd, syms.data(), sh[symidx].sh_size);

  std::optional<uint64_t> result;
  for (const auto& sym : syms) {
    const char* sname = &strtab[sym.st_name];
    if (sname && std::strcmp(sname, name) == 0) {
      result = co.load_base + sym.st_value;
      break;
    }
  }
  ::close(fd);
  return result;
}

// -------------------- New: process launcher (Linux) --------------------
static pid_t spawn_target_linux(const char* path, char* const argv[]) {
  pid_t child = 0;
  int rc = posix_spawn(&child, path, /*file_actions*/nullptr, /*attrp*/nullptr,
                       argv, environ);
  if (rc != 0) {
    std::fprintf(stderr, "posix_spawn failed: %d\n", rc);
    std::exit(1);
  }
  return child;
}

// -------------------- New: register catalog and dumping --------------------
struct RegDesc {
  amd_dbgapi_register_id_t id;
  std::string name;
  uint64_t size_bytes;
};

struct ArchRegCatalog {
  std::vector<RegDesc> regs;
};

static std::unordered_map<uint64_t, ArchRegCatalog> g_arch_catalog;

static std::string to_hex(const uint8_t *data, size_t n) {
  static const char *hex = "0123456789abcdef";
  std::string s;
  s.reserve(2 * n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = data[i];
    s.push_back(hex[(b >> 4) & 0xF]);
    s.push_back(hex[b & 0xF]);
  }
  return s;
}

static std::string json_escape(const char *in) {
  std::string out;
  for (const char *p = in; p && *p; ++p) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

static ArchRegCatalog &get_arch_reg_catalog(amd_dbgapi_architecture_id_t arch) {
  uint64_t key = arch.handle;
  auto it = g_arch_catalog.find(key);
  if (it != g_arch_catalog.end()) return it->second;

  ArchRegCatalog cat;

  size_t register_count = 0;
  amd_dbgapi_register_id_t *register_ids = nullptr;
  if (amd_dbgapi_architecture_register_list(arch, &register_count, &register_ids)
      != AMD_DBGAPI_STATUS_SUCCESS) {
    g_arch_catalog[key] = std::move(cat);
    return g_arch_catalog[key];
  }

  for (size_t i = 0; i < register_count; ++i) {
    RegDesc d{};
    d.id = register_ids[i];
    // Name
    char *bytes = nullptr;
    if (amd_dbgapi_register_get_info(d.id, AMD_DBGAPI_REGISTER_INFO_NAME,
                                     sizeof(bytes), &bytes) == AMD_DBGAPI_STATUS_SUCCESS && bytes) {
      d.name = bytes;
      std::free(bytes);
    } else {
      d.name = "reg" + std::to_string(i);
    }
    // Size (bytes)
    uint64_t sz = 0;
    if (amd_dbgapi_register_get_info(d.id, AMD_DBGAPI_REGISTER_INFO_SIZE,
                                     sizeof(sz), &sz) == AMD_DBGAPI_STATUS_SUCCESS && sz > 0) {
      d.size_bytes = sz;
    } else {
      // Fallback if size query unsupported
      d.size_bytes = 16;
    }
    cat.regs.push_back(std::move(d));
  }
  std::free(register_ids);
  g_arch_catalog[key] = std::move(cat);
  return g_arch_catalog[key];
}

static void read_all_registers_json(std::ostream &os,
                                    amd_dbgapi_wave_id_t wid,
                                    amd_dbgapi_architecture_id_t arch) {
  ArchRegCatalog &cat = get_arch_reg_catalog(arch);
  os << "\"regs\":{";
  bool first = true;
  for (const auto &r : cat.regs) {
    if (r.size_bytes == 0) continue;
    std::vector<uint8_t> buf(r.size_bytes);
    amd_dbgapi_status_t st = amd_dbgapi_read_register(wid, r.id, /*lane*/0,
                                                      r.size_bytes, buf.data());
    if (st != AMD_DBGAPI_STATUS_SUCCESS) continue;
    if (!first) os << ",";
    first = false;
    os << "\"" << r.name << "\":\"0x" << to_hex(buf.data(), buf.size()) << "\"";
  }
  os << "}";
}

static void read_insn_bytes_json(std::ostream &os, uint64_t pc,
                                 amd_dbgapi_architecture_id_t arch) {
  // Try to read up to the architecture's largest instruction size
  amd_dbgapi_size_t max_len = 16;
  amd_dbgapi_architecture_get_info(arch,
    AMD_DBGAPI_ARCHITECTURE_INFO_LARGEST_INSTRUCTION_SIZE,
    sizeof(max_len), &max_len);
  std::vector<uint8_t> bytes(max_len);
  size_t got = read_process_mem_linux(g_pid, pc, bytes.data(), bytes.size()) ? bytes.size() : 0;
  os << "\"insn_bytes\":\"0x" << to_hex(bytes.data(), got) << "\"";
}

static void disassemble_insn_json(std::ostream &os, uint64_t pc,
                                  amd_dbgapi_architecture_id_t arch) {
  amd_dbgapi_size_t max_len = 16;
  amd_dbgapi_architecture_get_info(arch,
    AMD_DBGAPI_ARCHITECTURE_INFO_LARGEST_INSTRUCTION_SIZE,
    sizeof(max_len), &max_len);
  std::vector<uint8_t> buf(max_len);
  size_t got = read_process_mem_linux(g_pid, pc, buf.data(), buf.size()) ? buf.size() : 0;
  amd_dbgapi_size_t insn_size = got;
  char *text = nullptr;
  amd_dbgapi_status_t st = amd_dbgapi_disassemble_instruction(
      arch, pc, &insn_size, buf.data(), &text, nullptr, nullptr);
  if (st == AMD_DBGAPI_STATUS_SUCCESS && text != nullptr) {
    std::string esc = json_escape(text);
    os << "\"insn\":\"" << esc << "\",\"insn_len\":" << (unsigned long long)insn_size;
    cb_free(text);
  } else {
    os << "\"insn\":null,\"insn_len\":0";
  }
}

// -------------------- New: NDJSON output support --------------------
static std::ofstream g_out;
static inline bool starts_with(const char *s, const char *pfx) {
  return std::strncmp(s, pfx, std::strlen(pfx)) == 0;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "Usage: %s <exe|pid> <kernel_symbol> <max_steps=0> [out=path] [args...]\n", argv[0]);
    return 2;
  }

  // Args: <exe|pid> <kernel_symbol> <max_steps=0> [args for exe...]
  bool launch = ::access(argv[1], X_OK) == 0; // crude check
  if (launch) {
    // Build argv array for child: path plus tail args
    std::vector<char*> child_argv;
    child_argv.push_back(argv[1]);
    for (int i = 4; i < argc; ++i) child_argv.push_back(argv[i]);
    child_argv.push_back(nullptr);

    g_pid = spawn_target_linux(argv[1], child_argv.data()); // on macOS: same posix_spawn
  } else {
    g_pid = std::atoi(argv[1]);
  }
  
  const char* kernel = argv[2];
  uint64_t max_steps = std::strtoull(argv[3], nullptr, 0);

  // Optional: out=path
  for (int i = 4; i < argc; ++i) {
    if (starts_with(argv[i], "out=")) {
      const char *path = argv[i] + 4;
      g_out.open(path, std::ios::out | std::ios::trunc);
    }
  }

  amd_dbgapi_callbacks_t cbs{};
  cbs.allocate_memory = cb_malloc;
  cbs.deallocate_memory = cb_free;
  cbs.client_process_get_info = cb_client_process_get_info;
  cbs.insert_breakpoint = nullptr;
  cbs.remove_breakpoint = nullptr;
  cbs.xfer_global_memory = cb_xfer_global_memory;
  cbs.log_message = cb_log;
  check(amd_dbgapi_initialize(&cbs), "initialize");

  amd_dbgapi_process_id_t process{AMD_DBGAPI_PROCESS_NONE};
  check(amd_dbgapi_process_attach((amd_dbgapi_client_process_id_t)g_pid, &process),
        "process_attach");

  // Resolve kernel symbol to PC by scanning code objects (mirrors ROCgdb conceptually).
  uint64_t kernel_pc = 0;
  for (const auto& co : list_code_objects(process)) {
    if (auto pc = resolve_kernel_symbol(co, kernel)) { kernel_pc = *pc; break; }
  }
  if (kernel_pc == 0) {
    std::fprintf(stderr, "Kernel symbol '%s' not found in code objects.\n", kernel);
    return 1;
  }
  std::fprintf(stdout, "Resolved %s => 0x%llx\n", kernel, (unsigned long long)kernel_pc);

  // Enumerate waves
  size_t wave_count=0; amd_dbgapi_wave_id_t* waves=nullptr;
  check(amd_dbgapi_process_wave_list(process, &wave_count, &waves), "process_wave_list");
  std::vector<amd_dbgapi_wave_id_t> wave_ids(waves, waves+wave_count);
  std::free(waves);

  uint64_t steps_total = 0;
  while (!wave_ids.empty() && (max_steps==0 || steps_total<max_steps)) {
    for (size_t i=0; i<wave_ids.size();) {
      auto wid = wave_ids[i];

      uint64_t pc=0;
      if (amd_dbgapi_wave_get_info(wid, AMD_DBGAPI_WAVE_INFO_PC, sizeof(pc), &pc)
          != AMD_DBGAPI_STATUS_SUCCESS) {
        wave_ids.erase(wave_ids.begin()+i); continue;
      }
      if (pc != kernel_pc) { ++i; continue; }

      // Architecture + breakpoint instruction size
      amd_dbgapi_architecture_id_t arch{};
      check(amd_dbgapi_wave_get_info(wid, AMD_DBGAPI_WAVE_INFO_ARCHITECTURE,
                                     sizeof(arch), &arch), "wave_get_info(arch)");
      size_t bp_size=0;
      check(amd_dbgapi_architecture_get_info(
              arch, AMD_DBGAPI_ARCHITECTURE_INFO_BREAKPOINT_INSTRUCTION_SIZE,
              sizeof(bp_size), &bp_size),
            "arch_get_info(BP_SIZE)");

      // Read original bytes at PC (dbgapi will call our xfer_global_memory)
      std::vector<uint8_t> orig(bp_size);
      if (!read_process_mem_linux(g_pid, pc, orig.data(), bp_size)) {
        ++i; continue;
      }

      uint64_t pc_before = pc;

      // Emit JSON (pre-step): wave, pc_before, regs, insn bytes
      if (g_out.is_open()) {
        std::ostringstream line;
        line << "{";
        line << "\"wave_id\":" << wid.handle << ",";
        line << "\"pc_before\":\"0x" << std::hex << pc_before << std::dec << "\",";
        disassemble_insn_json(line, pc_before, arch);
        line << ",";
        read_insn_bytes_json(line, pc_before, arch);
        line << ",";
        read_all_registers_json(line, wid, arch);
        line << "}" << '\n';
        g_out << line.str();
        g_out.flush();
      }

      // Displaced step
      amd_dbgapi_displaced_stepping_id_t step_id{};
      amd_dbgapi_status_t s
        = amd_dbgapi_displaced_stepping_start(wid, orig.data(), &step_id);
      if (s != AMD_DBGAPI_STATUS_SUCCESS) { ++i; continue; }

      // Query PC after (optional)
      uint64_t pc_after=0;
      amd_dbgapi_wave_get_info(wid, AMD_DBGAPI_WAVE_INFO_PC, sizeof(pc_after), &pc_after);

      if (!g_out.is_open()) {
        std::printf("wave=%" PRIu64 " pc=0x%llx -> 0x%llx\n", wid.handle,
                    (unsigned long long)pc_before, (unsigned long long)pc_after);
      }

      check(amd_dbgapi_displaced_stepping_complete(wid, step_id),
            "displaced_stepping_complete");

      steps_total++;
      if (max_steps && steps_total>=max_steps) break;
      ++i;
    }

    if (max_steps && steps_total>=max_steps) break;

    // Refresh waves
    wave_ids.clear();
    size_t wc=0; amd_dbgapi_wave_id_t* ws=nullptr;
    check(amd_dbgapi_process_wave_list(process, &wc, &ws), "process_wave_list(refresh)");
    wave_ids.assign(ws, ws+wc);
    std::free(ws);
  }

  amd_dbgapi_finalize();
  return 0;
}