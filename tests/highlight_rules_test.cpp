// Highlight rules unit test:
//   * 内置默认规则的每一条正则都能编译；
//   * 否定短语 / 失败 / 成功关键字在整屏文本里的命中列范围；
//   * 大小写开关；
//   * QSettings 存取往返（含「从未保存过 → 回退内置默认」与「删空 → 空表」）。

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

#include "Filter.h"
#include "HighlightRule.h"
#include "config/HighlightRulesStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

using Range = QPair<int, int>;   // (startColumn, endColumn)

// 用单行缓冲跑一遍 HighlightFilter，返回所有命中区间。
static QList<Range> matchRanges(const QString &pattern, const QString &text,
                                bool caseSensitive = false)
{
    Konsole::HighlightFilter filter(pattern, QColor(QStringLiteral("#ff0000")));
    QRegularExpression re(pattern);
    re.setPatternOptions(caseSensitive ? QRegularExpression::NoPatternOption
                                       : QRegularExpression::CaseInsensitiveOption);
    filter.setRegExp(re);

    QString buffer = text;
    QList<int> linePositions{0};
    filter.setBuffer(&buffer, &linePositions);
    filter.process();

    QList<Range> ranges;
    const QList<Konsole::Filter::HotSpot *> spots = filter.hotSpots();
    for (Konsole::Filter::HotSpot *spot : spots)
        ranges.append({spot->startColumn(), spot->endColumn()});
    return ranges;
}

static Konsole::HighlightRule findBuiltin(const QString &name)
{
    const QList<Konsole::HighlightRule> rules = Konsole::builtinHighlightRules();
    for (const Konsole::HighlightRule &rule : rules) {
        if (rule.name == name)
            return rule;
    }
    return Konsole::HighlightRule{};
}

static void testBuiltinRulesCompile()
{
    const QList<Konsole::HighlightRule> rules = Konsole::builtinHighlightRules();
    CHECK(!rules.isEmpty());
    for (const Konsole::HighlightRule &rule : rules) {
        CHECK(!rule.name.isEmpty());
        CHECK(!rule.pattern.trimmed().isEmpty());
        CHECK(rule.foreground.isValid());
        QRegularExpression re(rule.pattern);
        CHECK(re.isValid());
    }
}

static void testPhraseAndKeywordMatching()
{
    const Konsole::HighlightRule phrase = findBuiltin(QStringLiteral("否定短语"));
    CHECK(!phrase.pattern.isEmpty());
    if (!phrase.pattern.isEmpty()) {
        // Could not xxx：整段短语 + 紧跟的一个词
        QList<Range> ranges = matchRanges(phrase.pattern, QStringLiteral("Could not connect"));
        CHECK(ranges.size() == 1);
        if (!ranges.isEmpty()) {
            CHECK(ranges.first().first == 0);
            CHECK(ranges.first().second == QStringLiteral("Could not connect").size());
        }

        // no such file：只吃 "no such"，不吞整行
        ranges = matchRanges(phrase.pattern, QStringLiteral("no such file or directory"));
        CHECK(!ranges.isEmpty());
        if (!ranges.isEmpty()) {
            CHECK(ranges.first().first == 0);
            CHECK(ranges.first().second == QStringLiteral("no such").size());
        }
    }

    const Konsole::HighlightRule failed = findBuiltin(QStringLiteral("失败/拒绝关键字"));
    CHECK(!failed.pattern.isEmpty());
    if (!failed.pattern.isEmpty()) {
        const QList<Range> ranges =
            matchRanges(failed.pattern, QStringLiteral("Permission denied"));
        CHECK(ranges.size() == 1);
        if (!ranges.isEmpty()) {
            CHECK(ranges.first().first == 11);
            CHECK(ranges.first().second == 17);
        }
    }

    const Konsole::HighlightRule success = findBuiltin(QStringLiteral("成功关键字"));
    CHECK(!success.pattern.isEmpty());
    if (!success.pattern.isEmpty()) {
        // 默认不区分大小写
        CHECK(matchRanges(success.pattern, QStringLiteral("SUCCESS")).size() == 1);
        CHECK(matchRanges(success.pattern, QStringLiteral("success")).size() == 1);
        CHECK(matchRanges(success.pattern, QStringLiteral("token")).isEmpty());
    }
}

static void testCaseSensitivity()
{
    // 区分大小写时，全大写的 SUCCESS 不应命中
    CHECK(matchRanges(QStringLiteral("success"), QStringLiteral("SUCCESS"), true).isEmpty());
    CHECK(matchRanges(QStringLiteral("success"), QStringLiteral("SUCCESS"), false).size() == 1);
}

static void testStoreRoundTrip()
{
    {
        QSettings qs;
        qs.clear();
        qs.sync();
    }

    // 从未保存过 → 内置默认
    QList<Konsole::HighlightRule> loaded = loadHighlightRules();
    CHECK(loaded.size() == Konsole::builtinHighlightRules().size());

    // 存取往返
    QList<Konsole::HighlightRule> rules;
    Konsole::HighlightRule a;
    a.name = QStringLiteral("测试红");
    a.pattern = QStringLiteral("fail\\w*");
    a.foreground = QColor(QStringLiteral("#e74c3c"));
    a.caseSensitive = true;
    a.enabled = true;
    rules.append(a);

    Konsole::HighlightRule b;
    b.name = QStringLiteral("测试灰");
    b.pattern = QStringLiteral("debug");
    b.foreground = QColor(QStringLiteral("#888888"));
    b.caseSensitive = false;
    b.enabled = false;
    rules.append(b);

    QString err;
    CHECK(saveHighlightRules(rules, &err));
    CHECK(err.isEmpty());

    loaded = loadHighlightRules();
    CHECK(loaded.size() == 2);
    if (loaded.size() == 2) {
        CHECK(loaded[0].name == a.name);
        CHECK(loaded[0].pattern == a.pattern);
        CHECK(loaded[0].foreground == a.foreground);
        CHECK(loaded[0].caseSensitive == a.caseSensitive);
        CHECK(loaded[0].enabled == a.enabled);
        CHECK(loaded[1].name == b.name);
        CHECK(loaded[1].enabled == false);
    }

    // 用户删空 → 空表（不是回退默认）
    CHECK(saveHighlightRules({}, &err));
    CHECK(loadHighlightRules().isEmpty());

    // nullptr 终端不应崩溃
    applyHighlightRules(nullptr);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CubeShellTest"));
    QCoreApplication::setApplicationName(QStringLiteral("cube-shell-highlight-test"));

    // 测试用 INI 落到临时目录，避免污染真实用户设置。
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qWarning() << "FAIL: 无法创建临时目录";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

    testBuiltinRulesCompile();
    testPhraseAndKeywordMatching();
    testCaseSensitivity();
    testStoreRoundTrip();

    if (failures == 0)
        qInfo() << "highlight_rules_test: all checks passed";
    return failures == 0 ? 0 : 1;
}
