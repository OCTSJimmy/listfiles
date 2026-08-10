# P0-008: Run manifest + baseline_eligible 原子切换
# 行为驱动开发 (BDD) 规范

Feature: 运行清单与基准资格原子切换
  作为扫描系统
  我需要确保只有完整成功的运行才能成为盲信扫描的基准
  并通过原子切换防止基准损坏

  Background:
    Given 每次运行生成 `{base}.manifest` 文件
    And manifest 包含运行元数据、状态、统计、游标和校验和
    And 盲信扫描启动前检查 baseline_eligible

  # ===== manifest 结构 =====

  Scenario: manifest 生成
    Given 扫描启动（全量或续传）
    When 初始化运行时
    Then 生成 manifest：
      | 字段             | 类型       | 说明                  |
      | run_id           | uint64_t   | 运行唯一标识          |
      | target_path      | 字符串     | 扫描目标路径          |
      | schema_version   | uint16_t   | 进度文件格式版本      |
      | status           | 枚举       | running/complete/incomplete/fatal |
      | stats            | 结构体     | 文件数/目录数/字节数   |
      | cursors          | 结构体     | pbin/dpbin/dspill/output_offset |
      | checksum         | uint64_t   | manifest 自身校验和   |
      | baseline_eligible| bool       | 是否可作为盲信基准    |

  # ===== baseline_eligible 条件 =====

  Scenario: 完整运行标记 baseline_eligible
    Given 系统正常运行完成
    And 满足以下条件：
      | 条件 | 状态 |
      | status == complete | 是 |
      | skipped == 0（无 spbin 或 spbin 为空） | 是 |
      | dspill 已排空 | 是 |
      | fpbin 已转正（或不存在） | 是 |
      | archive 校验通过 | 是 |
    When 退出时 finalize()
    Then baseline_eligible = true
    And manifest 写入磁盘
    And 输出文件可安全作为下次盲信基准

  Scenario: 不完整运行拒绝 baseline_eligible
    Given 系统运行中有 spbin 条目（目录被跳过）
    When 退出时
    Then baseline_eligible = false
    And status = incomplete
    And 下次盲信扫描启动时拒绝运行
    And 提示用户："上次运行不完整，请使用 --runone 全量扫描"
    And 返回退出码 1

  Scenario: dspill 未排空拒绝 baseline_eligible
    Given 系统运行完成
    And dspill 文件非空（背压未完全消化）
    When 退出时
    Then baseline_eligible = false
    And 提示："dispatch_queue 有积压，运行不完整"

  Scenario: fpbin 未转正拒绝 baseline_eligible
    Given 系统运行完成
    And fpbin 文件存在且非空
    And fpbin 未合并到 pbin
    When 退出时
    Then baseline_eligible = false
    And 提示："临时进度未合并，运行不完整"

  # ===== 原子切换 =====

  Scenario: 新基准原子替换
    Given 当前存在旧基准 `{base}.archive`
    And 新运行满足 baseline_eligible = true
    When finalize_archive() 执行
    Then 新 archive 写入 `{base}.archive.new`
    And 新 archive 校验通过
    Then 原子重命名：`{base}.archive.new` → `{base}.archive`
    And 旧基准保留为 `{base}.archive.prev`
    And 防止切换过程中崩溃导致无有效基准

  Scenario: 切换失败回滚
    Given 新 archive 写入过程中发生错误
    When 写入失败或校验失败
    Then 保留旧 `{base}.archive`
    And 删除不完整的 `{base}.archive.new`
    And baseline_eligible = false
    And 记录错误日志

  # ===== 盲信扫描启动检查 =====

  Scenario: 盲信扫描启动前检查
    Given 用户启动盲信扫描（--trust 或默认增量）
 When 系统初始化
    Then 读取 `{base}.manifest`
    And 检查 baseline_eligible
    If baseline_eligible != true
    Then 拒绝盲信扫描
    And 提示用户全量扫描
    And exit(2)

  Scenario: 旧版本进度无 manifest
    Given 存在旧版本进度文件（无 manifest）
    When 启动盲信扫描
    Then 视为不兼容
    And 拒绝盲信扫描
    And 提示："旧版本进度，请使用 --runone 全量扫描"

  # ===== 验收标准 =====

  Scenario: 基准完整性验证
    Given 系统多次运行（全量/增量/盲信）
    When 检查所有 manifest
    Then 每个 manifest 都应满足：
      | 条件 | 必须成立 |
      | baseline_eligible=true 时 status=complete | 是 |
      | baseline_eligible=true 时 skipped=0 | 是 |
      | baseline_eligible=true 时 archive 校验通过 | 是 |
      | 切换时旧基准保留 .prev | 是 |
      | 盲信扫描拒绝 ineligible 基准 | 是 |
    And 不存在"baseline_eligible=true 但数据不完整"的情况
