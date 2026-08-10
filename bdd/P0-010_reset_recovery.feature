# P0-010: 崩溃恢复矩阵 — 统一 Reset 援救机制
# 行为驱动开发 (BDD) 规范

Feature: 统一 Reset 援救机制
  作为扫描系统
  我需要不枚举每个崩溃点，而是通过统一 Reset 机制恢复
  以简化恢复逻辑并覆盖所有非终态目录

  Background:
    Given 状态机终态只有 COMPLETED（P0-009）
    And 非终态目录在恢复时统一重置
    And pbin 是权威持久化（append-only，幂等）

  # ===== Reset 核心原则 =====

  Scenario: Reset 基本原则
    Given 系统崩溃后恢复
    When 执行 Reset 援救
    Then 核心原则：
      | 原则 | 说明 |
      | 非终态即未扫描 | 任何未写 dpbin 的目录视为未扫描 |
      | 输出截断 | 输出文件截断到最后确认的 dpbin offset |
      | pbin 幂等 | 重新扫描已写 pbin 的目录是安全的 |
      | 不枚举崩溃点 | 不区分"崩溃在哪里"，统一按非终态处理 |

  # ===== 恢复时输出截断 =====

  Scenario: 输出文件截断到最后确认 dpbin
    Given 上次运行 dpbin 最后记录目录 "/data/last_ok"
    And 输出文件 offset = 1234567
    And 之后还有 "/data/incomplete1" 的输出已写入
    When 恢复启动
    Then 输出文件截断到 offset 1234567
    And "/data/incomplete1" 的输出被丢弃
    And 重新扫描时重新输出（at-least-once）

  Scenario: dpbin 为空时截断到 0
    Given 上次运行没有任何目录完成
    And dpbin 为空
    When 恢复启动
    Then 输出文件截断到 0（清空）
    And 从目标路径重新全量扫描

  # ===== 非终态目录重置 =====

  Scenario: 各状态目录的 Reset 行为
    Given 上次运行后各状态目录：
      | 目录 | 状态 | Reset 行为 |
      | /data/d1 | DISCOVERED | 进差集，重新入队 |
      | /data/d2 | ENQUEUED | 进差集，重新入队 |
      | /data/d3 | SCANNING | 进差集，重新入队 |
      | /data/d4 | ALL_BATCHES_RECEIVED | 进差集，重新入队 |
      | /data/d5 | OUTPUT_COMMITTED | 进差集，重新入队（输出已截断） |
      | /data/d6 | COMPLETED | 保持完成，不进差集 |
      | /data/d7 | SKIPPED_FAILED | 按 spbin 处理（P0-005） |
    When 执行 Reset
    Then d1-d5 重新入队
    And d6 跳过
    And d7 按 spbin reason 处理

  # ===== pbin 幂等性 =====

  Scenario: 重新扫描已写 pbin 的目录
    Given 目录 "/data/redo1" 已写 pbin
    And 状态为 ALL_BATCHES_RECEIVED（崩溃时）
    When Reset 后重新扫描
    Then 重新发现该目录
    And 新 pbin 记录覆盖旧记录（幂等）
    And 文件条目重新输出
    And 不影响正确性（可能有重复输出）

  # ===== spbin 处理（P0-005） =====

  Scenario: Reset 时 spbin 整合
    Given spbin 中有 PROBE_FAIL 条目
    And 有 PERMISSION 条目
    When Reset 执行
    Then PERMISSION 条目永久跳过
    And PROBE_FAIL 条目按设备分组探测
    And 探测成功 → 入队
    And 探测失败 → 保持跳过，更新 timestamp

  # ===== fpbin 处理（P0-003） =====

  Scenario: Reset 时 fpbin 检查
    Given 存在 fpbin + dfpbin
    When Reset 执行
    Then 检查完整性
    And 完整 → 合并到 pbin/dpbin
    And 不完整 → 抛弃，从旧 pbin 恢复

  # ===== 不枚举崩溃点 =====

  Scenario: 任何崩溃点统一处理
    Given 系统在以下任意时刻崩溃：
      | 崩溃点 | 场景 |
      | 1 | Worker 发送 BATCH 时 |
      | 2 | Master 处理 BATCH 时 |
      | 3 | dpbin_append 时 |
      | 4 | 输出刷盘时 |
      | 5 | dspill 写入时 |
    When 恢复启动
    Then 不区分崩溃点
    And 统一执行 Reset
    And 非终态目录重新入队
    And 输出截断

  # ===== 验收标准 =====

  Scenario: Reset 援救完整性验证
    Given 在扫描各阶段注入崩溃
    When 每次恢复后验证
    Then 所有非终态目录最终都被扫描
    And 所有终态目录保持完成
    And 输出文件与 dpbin 一致
    And 无目录永久漏扫
    And 恢复逻辑不依赖崩溃点枚举
