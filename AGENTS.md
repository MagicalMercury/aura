# AGENTS.md

## 构建命令
- 编译编译器：`cmake --build build`
- 编译 Runtime 库：`cmake --build runtime/build`

## 项目约定
- `plan/` 文件夹用于存放计划草案和带审阅计划。
- 所有即将实现的计划（plan）都会写入根目录的 `change.md`。
- 所有测试均在 `example/` 文件夹中进行。example文件夹中的CMakeLists.txt仅是供我调试使用，禁止AIagent使用！测试的具体流程为：将测试代码写入test.aura，使用compile.cmd编译，运行test.exe。
- cmake 构建时输出no work to do ，即为已经编译完成，不可能出现忽略的问题！