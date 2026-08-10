# P0 级问题 BDD 文件清单

> 版本: v15.5.9+  
> 日期: 2026-08-11  
> 来源: fixme_v15.5.9_design_review.md

---

## 文件列表

| 编号 | 文件 | 问题描述 | 关联 |
|------|------|---------|------|
| P0-001 | `P0-001_finish_batch_race_barrier.feature` | FINISH/BATCH 竞态——目录任务完成屏障 | P0-009 |
| P0-002 | `P0-002_output_three_state_machine.feature` | 输出三态状态机 | P0-001, P0-009 |
| P0-003 | `P0-003_fpbin_crash_recovery.feature` | fpbin 二次崩溃恢复路径 | - |
| P0-004 | `P0-004_unified_queue_model.feature` | visited_set 背压竞态——统一队列模型 | P0-003, P1-001 |
| P0-005 | `P0-005_spbin_recovery.feature` | spbin 纳入恢复路径 | P1-002, P1-004 |
| P0-006 | `P0-006_msg_drop_prohibition.feature` | MSG_DROP 正常路径禁止 | - |
| P0-007 | `P0-007_ret_error_state_machine.feature` | RET_ERROR 状态机补全 | - |
| P0-008 | `P0-008_run_manifest_baseline.feature` | Run manifest + baseline_eligible 原子切换 | - |
| P0-009 | `P0-009_directory_lifecycle_state_machine.feature` | 目录任务生命周期状态机 | P0-001, P0-002 |
| P0-010 | `P0-010_reset_recovery.feature` | 崩溃恢复矩阵——统一 Reset 援救机制 | P0-003, P0-005 |
| P0-011 | `P0-011_blind_scan_prerequisites.feature` | 半增量跳过前提条件声明 | - |
| P0-012 | `P0-012_epoch_waitpid.feature` | epoch + waitpid 确认（旧 Worker 残留数据） | P0-001 |

---

## 使用方式

这些 BDD 文件使用 Gherkin 语法（Given/When/Then），可用于：

1. **需求确认**: 与利益相关者确认行为预期
2. **测试驱动开发**: 每个 Scenario 对应一个或多个测试用例
3. **验收标准**: 开发完成后验证是否满足需求
4. **文档**: 作为系统行为的活文档

---

## 状态说明

所有 P0 问题均为 **Design v1 已确认，待编码实现** 状态。

进入编码前必须闭环全部 P0 项。
