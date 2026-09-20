# EP Memory Server Dedup Combine 设计索引

Combine 内部设计已集中迁入 [Combine Server Dedup 设计](ep-memory-server-dedup-combine-design.md)。
当前按 M0、M1a/M1b、M2–M6 维护功能预期和简要设计方向。全部模块初版完成后再整体通读查漏，
历史 F/C 详细问题不作为当前待确认清单。下列旧锚点仅用于跳转到现有设计职责。
五表和公共接口仍由[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)维护。
本文件仅保留历史链接锚点，不再存放内部方案正文。

## Server 内主动读取方案（2026-09-18）

见 [Server 内主动读取方案（2026-09-18）](ep-memory-server-dedup-combine-design.md#已确定的整体结构)。

### 联合设计的选窗与调度前提

见 [共同通信域契约引用](ep-memory-server-dedup-combine-design.md#已确定的整体结构)。

### 已指定的职责与阶段

见 [已指定的职责与阶段](ep-memory-server-dedup-combine-design.md#模块边界m0m6)。

### 两级完成 Flag

见 [两级完成 Flag](ep-memory-server-dedup-combine-design.md#m2-本卡汇总与-server-ready)。

### 已确认的主动读取寻址

见 [已确认的主动读取寻址](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

### 接收核进度与中转区寿命

见 [接收核进度与中转区寿命](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

### 尚需明确的决定与验收

见 [尚需明确的决定与验收](ep-memory-server-dedup-combine-design.md#初版完成后的整体通读)。

## Dispatch 交接基线

见 [Dispatch 交接基线](ep-memory-server-dedup-combine-design.md#已确定的整体结构)。

## 本轮迁入边界

见 [本轮迁入边界](ep-memory-server-dedup-combine-design.md#模块边界m0m6)。

## C15

见 [C15](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C16

见 [C16](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C17

见 [C17](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## C18

见 [C18](ep-memory-server-dedup-combine-design.md#m5-最终本卡-token-聚合)。

## C19

见 [C19](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C20

见 [C20](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## C21

见 [C21](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C22

见 [C22](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## C23

见 [C23](ep-memory-server-dedup-combine-design.md#初版完成后的整体通读)。

## C24

见 [C24](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## 编号映射

见 [编号映射](ep-memory-server-dedup-combine-design.md#模块边界m0m6)。

## 旧议题入口

见 [旧议题入口](ep-memory-server-dedup-combine-design.md#模块边界m0m6)。

## C01

见 [C01](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C02

见 [C02](ep-memory-server-dedup-combine-design.md#m0-接口接线与公共布局)。

## C03

见 [C03](ep-memory-server-dedup-combine-design.md#m5-最终本卡-token-聚合)。

## C04

见 [C04](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C05

见 [C05](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C06

见 [C06](ep-memory-server-dedup-combine-design.md#m3-direct-返回)。

## C07

见 [C07](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## C08

见 [C08](ep-memory-server-dedup-combine-design.md#m4-读取局部聚合与-partial-返回)。

## C09

见 [C09](ep-memory-server-dedup-combine-design.md#m0-接口接线与公共布局)。

## C10

见 [C10](ep-memory-server-dedup-combine-design.md#已确定的整体结构)。

## C11

见 [C11](ep-memory-server-dedup-combine-design.md#m0-接口接线与公共布局)。

## C12

见 [C12](ep-memory-server-dedup-combine-design.md#m6-调度资源衔接与整体结束)。

## C13

见 [C13](ep-memory-server-dedup-combine-design.md#初版完成后的整体通读)。

## C14

见 [C14](ep-memory-server-dedup-combine-design.md#初版完成后的整体通读)。

## 验证边界

见 [验证边界](ep-memory-server-dedup-combine-design.md#参考与验证边界)。
