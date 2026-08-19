#define main neutral_cpu_oracle_fixture_main
#include "neutral_cpu_oracle_fixture.cpp"
#undef main

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>

namespace {

void printFrame(HANDLE process, HANDLE thread, CONTEXT context,
                STACKFRAME64 frame) {
  for (int depth = 0; depth < 32 && frame.AddrPC.Offset != 0; ++depth) {
    DWORD64 displacement = 0;
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
      std::fprintf(stderr, "frame[%d] %s+0x%llx (%p)\n", depth,
                   symbol->Name,
                   static_cast<unsigned long long>(displacement),
                   reinterpret_cast<void *>(frame.AddrPC.Offset));
      IMAGEHLP_LINE64 line{};
      line.SizeOfStruct = sizeof(line);
      DWORD lineDisplacement = 0;
      if (SymGetLineFromAddr64(process, frame.AddrPC.Offset,
                               &lineDisplacement, &line))
        std::fprintf(stderr, "  source=%s:%lu\n", line.FileName, line.LineNumber);
    } else {
      std::fprintf(stderr, "frame[%d] <unknown> (%p)\n", depth,
                   reinterpret_cast<void *>(frame.AddrPC.Offset));
    }
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                     &context, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr))
      break;
  }
}

int captureException(EXCEPTION_POINTERS *exception) {
  const auto code = exception->ExceptionRecord->ExceptionCode;
  const auto address = exception->ExceptionRecord->ExceptionAddress;
  std::fprintf(stderr, "exception code=0x%08lx address=%p\n",
               static_cast<unsigned long>(code), address);
  const auto &registers = *exception->ContextRecord;
  std::fprintf(stderr,
               "registers rip=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx "
               "rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx\n",
               registers.Rip, registers.Rsp, registers.Rbp, registers.Rax,
               registers.Rbx, registers.Rcx, registers.Rdx, registers.Rsi,
               registers.Rdi, registers.R8, registers.R9);

  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  SymInitialize(process, nullptr, TRUE);

  CONTEXT context = *exception->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  printFrame(process, thread, context, frame);

  SymCleanup(process);
  return static_cast<int>(code);
}

} // namespace

int main(int argc, char **argv) {
  __try {
    return neutral_cpu_oracle_fixture_main(argc, argv);
  } __except (captureException(GetExceptionInformation()),
              EXCEPTION_EXECUTE_HANDLER) {
    return 128;
  }
}
