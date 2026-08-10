# P0-002: 输出三态状态机 — DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED
# 行为驱动开发 (BDD) 规范
# 关联: P0-001, P0-009

Feature: 输出三态状态机
  作为扫描系统
  我需要区分"已发现"、"已排队输出"和"已提交输出"三种状态
  以确保崩溃恢复后不会永久丢失文件条目

  Background:
    Given 系统启用目录任务生命周期状态机（P0-009）
    And 输出系统使用异步批处理机制
    And pbin 是权威持久化（记录已发现目录）
    And dpbin 记录已完成目录

  # ===== 核心状态转移 =====

  Scenario: 新目录发现后进入 DISCOVERED 态
    Given Worker 扫描目录 "/data/source"
    And 发现文件 file1.txt, file2.txt
    When batch_processor 处理 BATCH
    Then 目录状态从 SCANNING 转移至 ALL_BATCHES_RECEIVED
    And 文件条目进入 OUTPUT_QUEUED 状态
    And pbin 记录该目录（含文件列表和 mtime）
    But dpbin 尚未记录（未 COMPLETED）

  Scenario: 输出线程确认后进入 OUTPUT_COMMITTED 态
    Given 目录 "/data/source" 状态为 ALL_BATCHES_PROCESSED
    And 文件条目已写入输出缓冲区
    When 输出线程完成刷盘
    And output_offset checkpoint 更新
    Then 目录状态转移至 OUTPUT_COMMITTED
    And 输出文件已持久化

  Scenario: dpbin 写入标记 COMPLETED 态
    Given 目录 "/data/source" 状态为 OUTPUT_COMMITTED
    When dpbin_append 执行
    Then 目录状态转移至 COMPLETED
    And 该目录进入 completed_set
    And 盲信扫描时可被跳过

  # ===== 崩溃恢复场景 =====

  Scenario: 崩溃于 DISCOVERED 态（未 OUTPUT_QUEUED）
    Given 上次运行时目录 "/data/incomplete" 已写 pbin
    And 崩溃前未进入 OUTPUT_QUEUED
    When 系统恢复启动
    Then 该目录在 pbin-dpbin 差集中
    And 目录被重新入队扫描
    And 旧 pbin 记录被覆盖（幂等）
    And 文件条目不会丢失

  Scenario: 崩溃于 OUTPUT_QUEUED 态（未 OUTPUT_COMMITTED）
    Given 上次运行时目录 "/data/incomplete2" 已写 pbin
    And 输出线程已排队但未刷盘
    When 系统恢复启动
    Then 输出文件截断到最后一个已确认 dpbin 的 offset（P0-010）
    And 该目录重新入队扫描
    And 已排队但未提交的输出被丢弃（安全，因为未确认）

  Scenario: 崩溃于 OUTPUT_COMMITTED 态（未 COMPLETED）
    Given 上次运行时目录 "/data/incomplete3" 输出已刷盘
    And output_offset 已 checkpoint
    But dpbin 未写入（崩溃发生在 dpbin_append 前）
    When 系统恢复启动
    Then 输出文件校验通过（与 dpbin 最后记录一致）
    And 该目录重新入队扫描
    And 旧输出条目可能被重复（at-least-once 语义，P1-007）
    And dpbin 写入后标记 COMPLETED

  # ===== dpbin_append 等待机制 =====

  Scenario: dpbin_append 阻塞等待 OUTPUT_COMMITTED
    Given 目录 "/data/waiting" 状态为 ALL_BATCHES_PROCESSED
    And 输出线程尚未完成刷盘
    When dpbin_append 被调用
    Then dpbin_append 应阻塞或重试等待
    And 不写入未确认的目录到 dpbin
    And 不标记为 COMPLETED
    When 输出线程完成刷盘
    Then dpbin_append 成功执行
    And 目录标记为 COMPLETED

  Scenario: 批量目录的 dpbin 顺序写入
    Given 目录 A、B、C 先后完成扫描
    And A 的输出已 COMMITTED
    And B 的输出尚未 COMMITTED
    And C 的输出已 COMMITTED
    When 检查 dpbin 写入
    Then A 可被写入 dpbin
    And B 必须等待，阻塞后续 C 的 dpbin 写入
    And 或采用批量缓冲，等待 B 完成后一起写入
    # 设计决策：dpbin 顺序保证输出与完成记录一致性

  # ===== 盲信扫描交互 =====

  Scenario: 盲信扫描跳过 OUTPUT_COMMITTED 目录
    Given 上次运行完整完成（baseline_eligible=true, P0-008）
    And 目录 "/data/skipme" 状态为 COMPLETED
    When 启动盲信扫描
    Then 读取 pbin 中该目录记录
    And 文件条目直接输出（不执行 lstat）
    And 目录本身仍需 readdir（检测新增/删除）
    And 新发现的文件进入全量扫描流程

  # ===== 验收标准 =====

  Scenario: 三态完整性验证
    Given 系统完成一次完整扫描后崩溃恢复
    When 验证所有目录状态
    Then 每个目录应处于以下状态之一：
      | 状态 | 含义 | 恢复行为 |
      | DISCOVERED | 已发现，未输出 | 重扫 |
      | OUTPUT_QUEUED | 已排队，未刷盘 | 截断输出，重扫 |
      | OUTPUT_COMMITTED | 已刷盘，未记dpbin | 校验输出，重扫 |
      | COMPLETED | 已记dpbin | 盲信跳过 |
    And 不应存在"pbin 有记录但未 COMPLETED 且未重扫"的目录
    And 不应存在"dpbin 有记录但输出文件缺失"的目录
