#pragma once

// HighlightRule.h — WindTerm 风格「文本正则高亮」规则的数据模型与内置默认值。
//
// 规则只描述「匹配什么 + 用什么前景色」，不依赖任何 UI / 持久化实现，
// 由 qtermwidget（渲染）、cube_core（持久化）与 cube_ui（设置界面）共用。
//
// 背景色暂不支持：TerminalDisplay::_drawTextFragment() 目前只能覆盖前景色，
// 纯背景色高亮在 _computeHighlightMap() 里会被忽略。

#include <QColor>
#include <QList>
#include <QString>

namespace Konsole {

struct HighlightRule {
    QString name;                   // 规则名，仅设置界面展示
    QString pattern;                // QRegularExpression 源码
    QColor  foreground;             // 命中字符的前景色
    bool    caseSensitive = false;  // 默认不区分大小写
    bool    enabled = true;
};

// 内置默认规则：列表顺序即优先级（靠上的规则先命中）。
// 用户从未保存过自定义规则时使用；用户保存过则整表替换。
QList<HighlightRule> builtinHighlightRules();

} // namespace Konsole
