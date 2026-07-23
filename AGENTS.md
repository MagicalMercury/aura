# AGENTS.md

## 构建命令
- 编译编译器：`cmake --build build`
- 编译 Runtime 库：`cmake --build runtime/build`

## 项目约定
- `plan/` 文件夹用于存放计划草案和带审阅计划。
- 所有即将实现的计划（plan）都会写入根目录的 `change.md`。
- 所有测试均在 `example/` 文件夹中进行。example文件夹中的CMakeLists.txt仅是供我调试使用，禁止AIagent使用！测试的具体流程为：将测试代码写入test.aura，使用compile.cmd编译，运行test.exe。
- cmake 构建时输出no work to do ，即为已经编译完成，不可能出现忽略的问题！

## AddressSanitizer 深度调试模式
- 根目录和 `runtime/` 的 CMakeLists.txt 均提供 `ENABLE_ASAN` 选项（默认 OFF）。
- **常规模式（默认）**：使用 UCRT64 的 g++，不带 ASAN。
- **ASAN 模式**：需要 MSYS2 CLANG64 的 clang，用以下命令重新配置（必须清空 build 目录）：
  ```powershell
  Remove-Item -Recurse -Force build, runtime/build
  cmake -S . -B build -DENABLE_ASAN=ON
  cmake -S runtime -B runtime/build -DENABLE_ASAN=ON
  cmake --build build
  cmake --build runtime/build
  ```
- **手动编译 test.cpp 用 ASAN**（绕过 aurac 自身构建，直接对生成代码做深度检测）：
  ```powershell
  $env:PATH = "C:/msys64/clang64/bin;" + $env:PATH
  C:/msys64/clang64/bin/clang++.exe -std=gnu++20 -fsanitize=address `
    -fno-omit-frame-pointer -g -O0 -fuse-ld=lld -w `
    -I runtime example/test.cpp runtime/build/libaura_rt.a -o example/test.exe
  ```
- **运行 ASAN 程序**：需将 `C:/msys64/clang64/bin/libclang_rt.asan_dynamic-x86_64.dll` 复制到 test.exe 同目录，或将 `C:/msys64/clang64/bin` 加入 PATH。ASAN 错误输出到 stderr，用 `Start-Process -RedirectStandardError` 捕获。
- **切回普通模式**：同样必须清空 build 目录重新配置（CMakeCache 会缓存编译器选择）：
  ```powershell
  Remove-Item -Recurse -Force build, runtime/build
  cmake -S . -B build
  cmake -S runtime -B runtime/build
  ```
- 适用场景：排查 SIGSEGV / 内存损坏 / use-after-free 等运行时崩溃，ASAN 能给出精确的栈追踪和根因定位。