/*
Copyright (c) 2015 - present Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <hip/hip_runtime.h>

#include "hip_internal.hpp"
#include "platform/program.hpp"
#include "platform/runtime.hpp"

#include <unordered_map>
#include "elfio.hpp"

constexpr unsigned __hipFatMAGIC2 = 0x48495046; // "HIPF"

struct __CudaFatBinaryWrapper {
  unsigned int magic;
  unsigned int version;
  void*        binary;
  void*        dummy1;
};

#define CLANG_OFFLOAD_BUNDLER_MAGIC_STR "__CLANG_OFFLOAD_BUNDLE__"
#define HIP_AMDGCN_AMDHSA_TRIPLE "hip-amdgcn-amd-amdhsa"
#define HCC_AMDGCN_AMDHSA_TRIPLE "hcc-amdgcn-amd-amdhsa-"

struct __ClangOffloadBundleDesc {
  uint64_t offset;
  uint64_t size;
  uint64_t tripleSize;
  const char triple[1];
};

struct __ClangOffloadBundleHeader {
  const char magic[sizeof(CLANG_OFFLOAD_BUNDLER_MAGIC_STR) - 1];
  uint64_t numBundles;
  __ClangOffloadBundleDesc desc[1];
};

extern "C" hipModule_t __hipRegisterFatBinary(const void* data)
{
  HIP_INIT();

  const __CudaFatBinaryWrapper* fbwrapper = reinterpret_cast<const __CudaFatBinaryWrapper*>(data);
  if (fbwrapper->magic != __hipFatMAGIC2 || fbwrapper->version != 1) {
    return nullptr;
  }
  std::string magic((char*)fbwrapper->binary, sizeof(CLANG_OFFLOAD_BUNDLER_MAGIC_STR) - 1);
  if (magic.compare(CLANG_OFFLOAD_BUNDLER_MAGIC_STR)) {
    return nullptr;
  }

  amd::Program* program = new amd::Program(*hip::getCurrentContext());
  if (!program)
    return nullptr;

  const auto obheader = reinterpret_cast<const __ClangOffloadBundleHeader*>(fbwrapper->binary);
  const auto* desc = &obheader->desc[0];
  for (uint64_t i = 0; i < obheader->numBundles; ++i,
       desc = reinterpret_cast<const __ClangOffloadBundleDesc*>(
           reinterpret_cast<uintptr_t>(&desc->triple[0]) + desc->tripleSize)) {

    std::string triple(desc->triple, sizeof(HIP_AMDGCN_AMDHSA_TRIPLE) - 1);
    if (triple.compare(HIP_AMDGCN_AMDHSA_TRIPLE))
      continue;

    std::string target(desc->triple + sizeof(HIP_AMDGCN_AMDHSA_TRIPLE),
                       desc->tripleSize - sizeof(HIP_AMDGCN_AMDHSA_TRIPLE));
    if (target.compare(hip::getCurrentContext()->devices()[0]->info().name_))
      continue;

    const void *image = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(obheader) + desc->offset);
    size_t size = desc->size;

    if (CL_SUCCESS == program->addDeviceProgram(*hip::getCurrentContext()->devices()[0], image, size) &&
        CL_SUCCESS == program->build(hip::getCurrentContext()->devices(), nullptr, nullptr, nullptr))
      break;
  }

  return reinterpret_cast<hipModule_t>(as_cl(program));
}

struct ihipExec_t {
  dim3 gridDim_;
  dim3 blockDim_;
  size_t sharedMem_;
  hipStream_t hStream_;
  std::vector<char> arguments_;
};

class PlatformState {
  amd::Monitor lock_;

  std::stack<ihipExec_t> execStack_;
  std::map<const void*, hipFunction_t> functions_;

  struct RegisteredVar {
    char* var;
    char* hostVar;
    char* deviceVar;
    int   size;
    bool  constant;
  };

  std::map<hipModule_t, RegisteredVar> vars_;

  static PlatformState* platform_;

  PlatformState() : lock_("Guards global function map") {}
public:
  static PlatformState& instance() {
    return *platform_;
  }

  void registerVar(hipModule_t modules,
                   char* var,
                   char* hostVar,
                   char* deviceVar,
                   int   size,
                   bool  constant) {
    amd::ScopedLock lock(lock_);

    const RegisteredVar rvar = { var, hostVar, deviceVar, size, constant != 0 };

    vars_.insert(std::make_pair(modules, rvar));
  }

  void registerFunction(const void* hostFunction, hipFunction_t func) {
    amd::ScopedLock lock(lock_);

    functions_.insert(std::make_pair(hostFunction, func));
  }

  hipFunction_t getFunc(const void* hostFunction) {
    amd::ScopedLock lock(lock_);
    const auto it = functions_.find(hostFunction);
    if (it != functions_.cend()) {
      return it->second;
    } else {
      return nullptr;
    }
  }

  void setupArgument(const void *arg,
                     size_t size,
                     size_t offset) {
    amd::ScopedLock lock(lock_);

    auto& arguments = execStack_.top().arguments_;

    if (arguments.size() < offset + size) {
      arguments.resize(offset + size);
    }

    ::memcpy(&arguments[offset], arg, size);
  }

  void configureCall(dim3 gridDim,
                     dim3 blockDim,
                     size_t sharedMem,
                     hipStream_t stream) {
    amd::ScopedLock lock(lock_);
    execStack_.push(ihipExec_t{gridDim, blockDim, sharedMem, stream});
  }

  void popExec(ihipExec_t& exec) {
    amd::ScopedLock lock(lock_);
    exec = std::move(execStack_.top());
    execStack_.pop();
  }
};
PlatformState* PlatformState::platform_ = new PlatformState();

extern "C" void __hipRegisterFunction(
  hipModule_t  module,
  const void*  hostFunction,
  char*        deviceFunction,
  const char*  deviceName,
  unsigned int threadLimit,
  uint3*       tid,
  uint3*       bid,
  dim3*        blockDim,
  dim3*        gridDim,
  int*         wSize)
{
  HIP_INIT();

  amd::Program* program = as_amd(reinterpret_cast<cl_program>(module));

  const amd::Symbol* symbol = program->findSymbol(deviceName);
  if (!symbol) return;

  amd::Kernel* kernel = new amd::Kernel(*program, *symbol, deviceName);
  if (!kernel) return;

  PlatformState::instance().registerFunction(hostFunction, reinterpret_cast<hipFunction_t>(as_cl(kernel)));
}

// Registers a device-side global variable.
// For each global variable in device code, there is a corresponding shadow
// global variable in host code. The shadow host variable is used to keep
// track of the value of the device side global variable between kernel
// executions.
extern "C" void __hipRegisterVar(
  hipModule_t modules,   // The device modules containing code object
  char*       var,       // The shadow variable in host code
  char*       hostVar,   // Variable name in host code
  char*       deviceVar, // Variable name in device code
  int         ext,       // Whether this variable is external
  int         size,      // Size of the variable
  int         constant,  // Whether this variable is constant
  int         global)    // Unknown, always 0
{
  HIP_INIT();

  PlatformState::instance().registerVar(modules, var, hostVar, deviceVar, size, constant != 0);
}

extern "C" void __hipUnregisterFatBinary(
  hipModule_t module
)
{
  HIP_INIT();
}

extern "C" hipError_t hipConfigureCall(
  dim3 gridDim,
  dim3 blockDim,
  size_t sharedMem,
  hipStream_t stream)
{
  HIP_INIT_API(gridDim, blockDim, sharedMem, stream);

  PlatformState::instance().configureCall(gridDim, blockDim, sharedMem, stream);

  return hipSuccess;
}

extern "C" hipError_t hipSetupArgument(
  const void *arg,
  size_t size,
  size_t offset)
{
  HIP_INIT_API(arg, size, offset);

  PlatformState::instance().setupArgument(arg, size, offset);

  return hipSuccess;
}

extern "C" hipError_t hipLaunchByPtr(const void *hostFunction)
{
  HIP_INIT_API(hostFunction);

  hipFunction_t func = PlatformState::instance().getFunc(hostFunction);
  if (func == nullptr)
    return hipErrorUnknown;

  ihipExec_t exec;
  PlatformState::instance().popExec(exec);

  void *extra[] = {
      HIP_LAUNCH_PARAM_BUFFER_POINTER, &exec.arguments_[0],
      HIP_LAUNCH_PARAM_BUFFER_SIZE, 0 /* FIXME: not needed, but should be correct*/,
      HIP_LAUNCH_PARAM_END
    };

  return hipModuleLaunchKernel(func,
    exec.gridDim_.x, exec.gridDim_.y, exec.gridDim_.z,
    exec.blockDim_.x, exec.blockDim_.y, exec.blockDim_.z,
    exec.sharedMem_, exec.hStream_, nullptr, extra);
}

#if defined(ATI_OS_LINUX)

namespace hip_impl {

struct dl_phdr_info {
  ELFIO::Elf64_Addr        dlpi_addr;
  const char       *dlpi_name;
  const ELFIO::Elf64_Phdr *dlpi_phdr;
  ELFIO::Elf64_Half        dlpi_phnum;
};

extern "C" int dl_iterate_phdr(
  int (*callback) (struct dl_phdr_info *info, size_t size, void *data), void *data
);

struct Symbol {
  std::string name;
  ELFIO::Elf64_Addr value = 0;
  ELFIO::Elf_Xword size = 0;
  ELFIO::Elf_Half sect_idx = 0;
  uint8_t bind = 0;
  uint8_t type = 0;
  uint8_t other = 0;
};

inline Symbol read_symbol(const ELFIO::symbol_section_accessor& section, unsigned int idx) {
  assert(idx < section.get_symbols_num());

  Symbol r;
  section.get_symbol(idx, r.name, r.value, r.size, r.bind, r.type, r.sect_idx, r.other);

  return r;
}

template <typename P>
inline ELFIO::section* find_section_if(ELFIO::elfio& reader, P p) {
    const auto it = find_if(reader.sections.begin(), reader.sections.end(), std::move(p));

    return it != reader.sections.end() ? *it : nullptr;
}

std::vector<std::pair<uintptr_t, std::string>> function_names_for(const ELFIO::elfio& reader,
                                                                  ELFIO::section* symtab) {
  std::vector<std::pair<uintptr_t, std::string>> r;
  ELFIO::symbol_section_accessor symbols{reader, symtab};

  for (auto i = 0u; i != symbols.get_symbols_num(); ++i) {
    auto tmp = read_symbol(symbols, i);

    if (tmp.type == STT_FUNC && tmp.sect_idx != SHN_UNDEF && !tmp.name.empty()) {
      r.emplace_back(tmp.value, tmp.name);
    }
  }

  return r;
}

const std::vector<std::pair<uintptr_t, std::string>>& function_names_for_process() {
  static constexpr const char self[] = "/proc/self/exe";

  static std::vector<std::pair<uintptr_t, std::string>> r;
  static std::once_flag f;

  std::call_once(f, []() {
    ELFIO::elfio reader;

    if (reader.load(self)) {
      const auto it = find_section_if(
          reader, [](const ELFIO::section* x) { return x->get_type() == SHT_SYMTAB; });

      if (it) r = function_names_for(reader, it);
    }
  });

  return r;
}


const std::unordered_map<uintptr_t, std::string>& function_names()
{
  static std::unordered_map<uintptr_t, std::string> r{
    function_names_for_process().cbegin(),
    function_names_for_process().cend()};
  static std::once_flag f;

  std::call_once(f, []() {
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
      ELFIO::elfio reader;

      if (reader.load(info->dlpi_name)) {
        const auto it = find_section_if(
            reader, [](const ELFIO::section* x) { return x->get_type() == SHT_SYMTAB; });

        if (it) {
          auto n = function_names_for(reader, it);

          for (auto&& f : n) f.first += info->dlpi_addr;

          r.insert(make_move_iterator(n.begin()), make_move_iterator(n.end()));
        }
      }
      return 0;
    },
    nullptr);
  });

  return r;
}

std::vector<char> bundles_for_process() {
  static constexpr const char self[] = "/proc/self/exe";
  static constexpr const char kernel_section[] = ".kernel";
  std::vector<char> r;

  ELFIO::elfio reader;

  if (reader.load(self)) {
    auto it = find_section_if(
        reader, [](const ELFIO::section* x) { return x->get_name() == kernel_section; });

    if (it) r.insert(r.end(), it->get_data(), it->get_data() + it->get_size());
  }

  return r;
}

const std::vector<hipModule_t>& modules() {
    static std::vector<hipModule_t> r;
    static std::once_flag f;

    std::call_once(f, []() {
      static std::vector<std::vector<char>> bundles{bundles_for_process()};

      dl_iterate_phdr(
          [](dl_phdr_info* info, std::size_t, void*) {
        ELFIO::elfio tmp;
        if (tmp.load(info->dlpi_name)) {
          const auto it = find_section_if(
              tmp, [](const ELFIO::section* x) { return x->get_name() == ".kernel"; });

          if (it) bundles.emplace_back(it->get_data(), it->get_data() + it->get_size());
        }
        return 0;
      },
      nullptr);

      for (auto&& bundle : bundles) {
        std::string magic(&bundle[0], sizeof(CLANG_OFFLOAD_BUNDLER_MAGIC_STR) - 1);
        if (magic.compare(CLANG_OFFLOAD_BUNDLER_MAGIC_STR))
          continue;

        const auto obheader = reinterpret_cast<const __ClangOffloadBundleHeader*>(&bundle[0]);
        const auto* desc = &obheader->desc[0];
        for (uint64_t i = 0; i < obheader->numBundles; ++i,
             desc = reinterpret_cast<const __ClangOffloadBundleDesc*>(
                 reinterpret_cast<uintptr_t>(&desc->triple[0]) + desc->tripleSize)) {

          std::string triple(desc->triple, sizeof(HCC_AMDGCN_AMDHSA_TRIPLE) - 1);
          if (triple.compare(HCC_AMDGCN_AMDHSA_TRIPLE))
            continue;

          std::string target(desc->triple + sizeof(HCC_AMDGCN_AMDHSA_TRIPLE),
                             desc->tripleSize - sizeof(HCC_AMDGCN_AMDHSA_TRIPLE));

          if (!target.compare(hip::getCurrentContext()->devices()[0]->info().name_)) {
            hipModule_t module;
            if (hipSuccess == hipModuleLoadData(&module, reinterpret_cast<const void*>(
                reinterpret_cast<uintptr_t>(obheader) + desc->offset)))
              r.push_back(module);
              break;
          }
        }
      }
    });

    return r;
}

const std::unordered_map<uintptr_t, hipFunction_t>& functions()
{
  static std::unordered_map<uintptr_t, hipFunction_t> r;
  static std::once_flag f;

  std::call_once(f, []() {
    for (auto&& function : function_names()) {
      for (auto&& module : modules()) {
        hipFunction_t f;
        if (hipSuccess == hipModuleGetFunction(&f, module, function.second.c_str()))
          r[function.first] = f;
      }
    }
  });

  return r;
}


void hipLaunchKernelGGLImpl(
  uintptr_t function_address,
  const dim3& numBlocks,
  const dim3& dimBlocks,
  uint32_t sharedMemBytes,
  hipStream_t stream,
  void** kernarg)
{
  HIP_INIT();

  const auto it = functions().find(function_address);
  if (it == functions().cend())
    assert(0);

  hipModuleLaunchKernel(it->second,
    numBlocks.x, numBlocks.y, numBlocks.z,
    dimBlocks.x, dimBlocks.y, dimBlocks.z,
    sharedMemBytes, stream, nullptr, kernarg);
}

}

#endif // defined(ATI_OS_LINUX)
