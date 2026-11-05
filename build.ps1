# ============================================================
# EMDA build 脚本（PowerShell 版）
# ============================================================
# 用法：
#   .\build              编译静态库（默认 arm-none-eabi 交叉编译）
#   .\build clean        清理 build/
#   .\build host         用主机 gcc 编译（语法验证）
#   .\build test         用主机 gcc 跑模拟模组测试
#   .\build rebuild      clean + 编译
# ============================================================

param(
    [Parameter(Position = 0)]
    [ValidateSet("all", "clean", "rebuild", "host", "test", "")]
    [string]$Action = "all"
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

switch ($Action) {
    "clean"   { make clean; break }
    "rebuild" { make clean; make; break }
    "host"    { make CC=gcc AR=ar; break }
    "test"    { make test; break }
    default   { make; break }
}
