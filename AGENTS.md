# AGENTS.md

## 构建命令
- 编译编译器：`cmake --build build`
- 编译 Runtime 库：`cmake --build runtime/build`

## 项目约定
- `plan/` 文件夹用于存放计划草案和带审阅计划。
- 所有即将实现的计划（plan）都会写入根目录的 `change.md`。
- 所有测试均在 `example/` 文件夹中进行。example文件夹中的CMakeLists.txt仅是供我调试使用，禁止AIagent使用！
- 编译aura代码失败后，记得查看example\compile.cmd文件，这是必定正确的编译命令！