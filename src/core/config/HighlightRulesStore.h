#pragma once

// HighlightRulesStore.h — WindTerm 风格高亮规则的持久化。
//
// 存储走 QSettings（键 settings/highlight_rules），值是紧凑 JSON：
//   {"version":1,"rules":[{"name":…,"pattern":…,"foreground":"#rrggbb",
//                          "caseSensitive":false,"enabled":true}, …]}
// 这是 C++ 版专属设置，不写进与 Python 版共享的 theme.json。
//
// 语义：
//   * 键不存在（从未保存过）→ 返回 Konsole::builtinHighlightRules()；
//   * 键存在但规则为空 → 用户主动删空，返回空表（仅剩权限串与搜索高亮）；
//   * JSON 损坏 → qWarning 并回退内置默认规则。

#include <QList>
#include <QString>

#include "HighlightRule.h"   // Konsole::HighlightRule（qtermwidget PUBLIC include）

class QTermWidget;

namespace cubeshell {

// 读取当前高亮规则（无记录时返回内置默认）。
QList<Konsole::HighlightRule> loadHighlightRules();

// 保存规则；失败时把原因写入 errorOut（若有）。
bool saveHighlightRules(const QList<Konsole::HighlightRule> &rules,
                        QString *errorOut = nullptr);

// 便捷入口：把已保存的规则应用到指定终端（已打开的终端立即重绘）。
void applyHighlightRules(QTermWidget *term);

} // namespace cubeshell
