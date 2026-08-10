# P0-006: MSG_DROP 正常路径禁止
# 行为驱动开发 (BDD) 规范

Feature: MSG_DROP 禁止与背压保障
  作为扫描系统
  我需要在正常路径禁止丢弃消息
  确保 BATCH/ERROR 消息不会导致结果丢失或静默失败

  Background:
    Given 系统使用 IPC 双通道通信
    And 队列有容量上限
    And 背压机制已启用（P0-004）

  # ===== 正常路径禁止 DROP =====

  Scenario: BATCH 消息不应被 DROP
    Given Worker 发送 BATCH 消息
    And 控制通道队列未满
    When 消息到达 IPC 层
    Then 消息必须被接收和处理
    And 不允许返回 MSG_DROP
    And 若队列将满，触发背压而非丢弃

  Scenario: ERROR 消息不应被 DROP
    Given Worker 发送 ERROR 消息（设备故障报告）
    When 消息到达 IPC 层
    Then 消息必须被接收和处理
    And 不允许返回 MSG_DROP
    And ERROR 丢失会导致设备状态误判

  # ===== 背压替代 DROP =====

  Scenario: 队列满时触发背压
    Given dispatch_queue 和 dspill 均达上限
    And Worker 尝试发送 BATCH
    When Master 侧检测到队列满
    Then 通知 Worker 暂停发送（背压信号）
    And Worker 进入等待状态
    And 不丢弃 BATCH
    And 队列有空位后恢复发送

  Scenario: IPC 通道背压传导
    Given IPC 数据通道缓冲区满
    When Worker 尝试 write() BATCH
    Then write() 返回 EAGAIN 或阻塞
    And Worker 等待并重试
    And 不构造 MSG_DROP

  # ===== 异常 DROP 处理（防御性） =====

  Scenario: 极端情况下 BATCH 被 DROP
    Given 系统在资源耗尽边缘（ENOMEM）
    And BATCH 无法被接收
    When 防御性 DROP 发生
    Then 该目录任务标记为失败
    And 整目录重新入队重扫
    或标记为 FAILED 进 spbin
    And 本次运行不能标记为"完整完成"

  Scenario: 极端情况下 ERROR 被 DROP
    Given 系统资源极度紧张
    And ERROR 消息无法被接收
    When 防御性 DROP 发生
    Then 终止运行（log_fatal）
    And ERROR 丢失不可接受（设备状态未知）
    And 返回退出码 2（严重失败）

  # ===== 运行完整性标记 =====

  Scenario: 任何 DROP 后运行不完整
    Given 运行过程中发生任何 DROP
    When 系统尝试正常退出
    Then 退出码为 1（部分完成）
    And manifest 中 status = incomplete
    And baseline_eligible = false（P0-008）
    And 记录 DROP 事件日志

  Scenario: 无 DROP 完整运行
    Given 运行过程中无任何 DROP
    And 所有目录处理完成（含 spbin 处理）
    When 系统正常退出
    Then 退出码为 0（完全完成）
    And 可标记 baseline_eligible = true（满足其他条件时）

  # ===== 验收标准 =====

  Scenario: DROP 零容忍验证
    Given 系统在各种负载下运行
    When 监控 MSG_DROP 发生次数
    Then 正常路径下 DROP 次数必须为 0
    And 队列满时应触发背压而非 DROP
    And 防御性 DROP 必须伴随：
      | 条件 | 要求 |
      | BATCH DROP | 目录重扫或进 spbin |
      | ERROR DROP | 终止运行 |
      | 任何 DROP | 运行标记不完整 |
