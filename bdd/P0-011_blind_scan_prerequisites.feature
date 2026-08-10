# P0-011: 半增量跳过前提条件声明
# 行为驱动开发 (BDD) 规范

Feature: 半增量跳过（盲信扫描）前提条件
  作为扫描系统
  我需要明确盲信扫描的前置条件和语义限制
  以防止用户对增量能力产生错误预期

  Background:
    Given 盲信扫描基于上次完整运行的 pbin 记录
    And pbin 记录目录及其文件的元数据（mtime 等）
    And 盲信扫描不更新 reference_map（只读）

  # ===== 核心语义声明 =====

  Scenario: 盲信扫描的文件级粒度
    Given 上次完整运行已记录 "/data/dir1/file1.txt" mtime=1000
    When 启动盲信扫描
    Then 目录 "/data/dir1" 仍需 readdir（发现新增/删除）
    And file1.txt 的 mtime 未变化 → 盲信跳过 lstat
    And file1.txt 直接输出（使用 pbin 记录）
    And file1.txt 的 mtime 变化 → 执行 lstat，更新输出

  Scenario: 目录 mtime 传播无关紧要
    Given 文件系统不保证目录 mtime 向上传播
    When 盲信扫描
    Then 不依赖目录 mtime
    And 直接比较文件级 mtime
    And 目录本身仍需 readdir

  Scenario: 盲信扫描不检测变更
    Given 上次运行后某文件内容改变但 mtime 未变
    When 盲信扫描
    Then 该文件被盲信跳过
    And 不检测内容变更
    And 这是设计预期（非 bug）

  Scenario: 盲信扫描不检测删除
    Given 上次运行后某文件被删除
    And 目录 mtime 可能未变
    When 盲信扫描 readdir
    Then 发现该文件不存在
    And 不输出该文件
    And 正确反映当前状态

  # ===== reference_map 内存管理 =====

  Scenario: 盲信扫描不更新 reference_map
    Given 上次运行建立了 reference_map
    When 盲信扫描
    Then reference_map 以只读方式使用
    And 不添加新条目
    And 不删除旧条目
    And 内存占用稳定

  Scenario: 全量扫描重建 reference_map
    Given 启动全量扫描（--runone）
    When 扫描执行
    Then 重建 reference_map
    And 旧 reference_map 被释放
    And 新运行完成后成为新的盲信基准

  # ===== 启动前置条件 =====

  Scenario: 非空目标目录启动检查
    Given 用户指定 --runone（强制全量）
    And 目标目录非空（有输出文件）
    When 启动时
    Then exit(2)
    And 提示："目标目录非空，请先清理或使用续传模式"

  Scenario: 无有效基准拒绝盲信
    Given 上次运行 baseline_eligible = false
    When 启动盲信扫描
    Then 拒绝运行
    And 提示："无有效盲信基准，请使用 --runone 全量扫描"
    And exit(2)

  # ===== 性能预期 =====

  Scenario: 盲信扫描跳过 lstat
    Given 目录下有 1000 个文件
    And 所有文件 mtime 未变
    When 盲信扫描
    Then 只需 readdir（1 次系统调用）
    And 无需 lstat（1000 次系统调用跳过）
    And 文件条目直接从 pbin 复制到输出
    And 性能提升显著（减少 99%+ 的 stat 调用）

  # ===== 验收标准 =====

  Scenario: 盲信语义完整性验证
    Given 用户查阅盲信扫描文档
    When 理解盲信能力边界
    Then 明确知道：
      | 能力 | 说明 |
      | 跳过未变更文件的 lstat | 是 |
      | 检测新增文件 | 是（通过 readdir） |
      | 检测删除文件 | 是（通过 readdir） |
      | 检测内容变更（mtime 未变） | 否（设计限制） |
      | 检测权限变更 | 否（除非 mtime 变） |
      | 保证全局一致快照 | 否（每个目录原子，跨目录不保证） |
    And 文档明确声明这些限制
