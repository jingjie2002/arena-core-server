# M1 交接：C++17 工程骨架

- 日期：2026-08-11
- 结论：M1 工程骨架已在 WSL2 Ubuntu 上分别通过 GCC 13.3 和 Clang 18.1 构建与测试。

## 已有能力

- CMake Presets 提供 Linux GCC、Linux Clang、Linux vcpkg 和 Windows MSVC 入口。
- 所有目标强制使用 C++17；专门的 contract test 会尝试用 C++20 编译并确认被拒绝。
- `arena-core-server` 和 `arena-core-robot` 已有版本与 self-check 入口。
- 配置 JSON 有默认值、范围校验和稳定错误码；示例配置不含秘密。
- spdlog 输出带时间、级别和服务名的结构化文本，并支持可选文件 sink。
- GoogleTest/CTest 已接入，CI 定义覆盖 Ubuntu GCC/Clang。
- `vcpkg.json` 固定官方 registry baseline，并把 M5/M6 的数据、规则和观测依赖分为可选 feature。

## 本地验证命令

```bash
./scripts/dev/build-wsl.sh linux-gcc
./scripts/dev/build-wsl.sh linux-clang
./out/build/linux-gcc/arena-core-server --check-config --config config/arena-core.example.json
```

本轮实际结果：两套编译器各 11/11 测试通过，安装后的服务端 self-check 通过。

## 当前边界

- 可执行文件还是工程骨架，M3 才开始监听 TCP。
- vcpkg manifest 已格式化和固定基线；本地 M1 构建使用 Ubuntu 系统包，完整 vcpkg feature 会随对应里程碑启用并验证。
- GitHub Actions 文件已建立，但工作区尚未发布远端，因此没有线上 CI 运行记录。

## 用户介入

不需要。按三节点约定，继续自动进入 M2。
