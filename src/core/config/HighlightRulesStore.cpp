// HighlightRulesStore.cpp — 高亮规则的 QSettings 持久化。见 HighlightRulesStore.h。

#include "HighlightRulesStore.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QObject>
#include <QSettings>

#include "qtermwidget.h"   // QTermWidget::setHighlightRules

namespace cubeshell {

namespace {

// QSettings 键（组织/应用名见 app/main.cpp 的 setOrganizationName）。
constexpr const char kRulesKey[] = "settings/highlight_rules";
constexpr int kSchemaVersion = 1;

} // namespace

QList<Konsole::HighlightRule> loadHighlightRules()
{
    QSettings qs;
    if (!qs.contains(QLatin1String(kRulesKey))) {
        // 从未保存过：首次启动就带上内置默认规则（WindTerm 那种开箱观感）。
        return Konsole::builtinHighlightRules();
    }

    const QByteArray raw = qs.value(QLatin1String(kRulesKey)).toString().toUtf8();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "HighlightRulesStore: 规则 JSON 解析失败，回退内置默认规则:"
                   << parseError.errorString();
        return Konsole::builtinHighlightRules();
    }

    const QJsonArray array = doc.object().value(QStringLiteral("rules")).toArray();
    QList<Konsole::HighlightRule> rules;
    rules.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();

        Konsole::HighlightRule rule;
        rule.name = obj.value(QStringLiteral("name")).toString();
        rule.pattern = obj.value(QStringLiteral("pattern")).toString();
        rule.foreground = QColor(obj.value(QStringLiteral("foreground")).toString());
        rule.caseSensitive = obj.value(QStringLiteral("caseSensitive")).toBool(false);
        // 缺省视为启用：老版本数据里没有该字段时不应悄悄关掉高亮。
        rule.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
        rules.append(rule);
    }

    // 键存在但 rules 为空数组 → 用户主动删空，如实返回空表。
    return rules;
}

bool saveHighlightRules(const QList<Konsole::HighlightRule> &rules, QString *errorOut)
{
    QJsonArray array;
    for (const Konsole::HighlightRule &rule : rules) {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), rule.name);
        obj.insert(QStringLiteral("pattern"), rule.pattern);
        obj.insert(QStringLiteral("foreground"),
                   rule.foreground.isValid()
                       ? rule.foreground.name(QColor::HexRgb)
                       : QString());
        obj.insert(QStringLiteral("caseSensitive"), rule.caseSensitive);
        obj.insert(QStringLiteral("enabled"), rule.enabled);
        array.append(obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kSchemaVersion);
    root.insert(QStringLiteral("rules"), array);

    QSettings qs;
    qs.setValue(QLatin1String(kRulesKey),
                QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    qs.sync();
    if (qs.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QObject::tr("写入设置失败（%1）")
                            .arg(qs.status() == QSettings::AccessError
                                     ? QObject::tr("无权访问设置文件")
                                     : QObject::tr("格式错误"));
        return false;
    }
    return true;
}

void applyHighlightRules(QTermWidget *term)
{
    if (!term)
        return;
    term->setHighlightRules(loadHighlightRules());
}

} // namespace cubeshell
