#include "SettingsDialog.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVector>

#include "LanguageManager.h"
#include "config/GlobalState.h"
#include "config/HighlightRulesStore.h"
#include "util/ThemeManager.h"

namespace cubeshell {

// theme.json 中 SSH 超时的键现由 GlobalState::sshConnectTimeoutSeconds() 持有
// （之前是本文件的 kSshTimeoutKey）。挪过去是因为要有人**读**它：SshClient
// 建连时取这个值当预算，在此之前这个设置项写了没人看，纯装饰。

// 表单字段统一最小宽度：各 Tab 里所有输入框/下拉框同宽，
// 避免长短不一显得杂乱，也给长下拉值留出显示空间。
constexpr int kFieldMinWidth = 280;

static void unifyFieldWidth(QWidget *w)
{
    w->setMinimumWidth(kFieldMinWidth);
    QSizePolicy sp = w->sizePolicy();
    sp.setHorizontalPolicy(QSizePolicy::Expanding);
    w->setSizePolicy(sp);
}

// 下拉框闭合态对超长值会省略显示，用 tooltip 兜底展示全值。
static void addFullTextToolTip(QComboBox *combo)
{
    QObject::connect(combo, &QComboBox::currentTextChanged, combo,
                     [combo](const QString &text) { combo->setToolTip(text); });
    combo->setToolTip(combo->currentText());
}

// 高亮规则表格的列号。
constexpr int kHlColEnabled = 0;
constexpr int kHlColName = 1;
constexpr int kHlColPattern = 2;
constexpr int kHlColCase = 3;
constexpr int kHlColColor = 4;

// 颜色按钮：色块背景 + 十六进制文字；按亮度选黑/白字保证可读。
// 选中的颜色存在按钮的 hlColor 属性里，currentHighlightRules() 从那里取回。
static void paintHighlightColorButton(QPushButton *button, const QColor &color)
{
    button->setProperty("hlColor", color);
    const QColor shown = color.isValid() ? color : QColor(Qt::gray);
    const QString textColor = (shown.lightness() < 128) ? QStringLiteral("#ffffff")
                                                        : QStringLiteral("#000000");
    button->setText(color.isValid() ? color.name(QColor::HexRgb) : QObject::tr("未设置"));
    button->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; color: %2;"
                       " border: 1px solid #888; padding: 2px 8px; }")
            .arg(shown.name(QColor::HexRgb), textColor));
}

static QTableWidgetItem *makeCheckableItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

// 把「逐字符首匹配优先」的着色结果渲染成富文本，语义与终端
// TerminalDisplay::_computeHighlightMap() 一致：靠上的规则先占住字符。
static QString highlightPreviewHtml(const QString &text,
                                    const QList<Konsole::HighlightRule> &rules,
                                    const QColor &defaultColor)
{
    QVector<QColor> colors(text.size());
    for (const Konsole::HighlightRule &rule : rules) {
        if (!rule.enabled || rule.pattern.trimmed().isEmpty() || !rule.foreground.isValid())
            continue;
        QRegularExpression re(rule.pattern);
        re.setPatternOptions(rule.caseSensitive
                                 ? QRegularExpression::NoPatternOption
                                 : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid())
            continue;
        QRegularExpressionMatchIterator it = re.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const int start = qMax(0, int(match.capturedStart()));
            const int end = qMin(int(match.capturedEnd()), int(colors.size()));
            for (int i = start; i < end; ++i) {
                if (!colors[i].isValid())
                    colors[i] = rule.foreground;
            }
        }
    }

    QString html;
    html.reserve(text.size() * 5);
    for (int i = 0; i < text.size();) {
        const QColor color = colors[i].isValid() ? colors[i] : defaultColor;
        int j = i + 1;
        while (j < text.size()) {
            const QColor next = colors[j].isValid() ? colors[j] : defaultColor;
            if (next != color)
                break;
            ++j;
        }
        QString chunk = text.mid(i, j - i).toHtmlEscaped();
        chunk.replace(QLatin1Char(' '), QStringLiteral("&#160;"));   // 保留连续空格
        html += QStringLiteral("<span style=\"color:%1\">%2</span>")
                    .arg(color.name(QColor::HexRgb), chunk);
        i = j;
    }
    return html;
}

// QSettings 键（组织/应用名见 app/main.cpp 的 setOrganizationName）。
namespace settings_keys {
static const char kTheme[]      = "settings/theme";
static const char kLanguage[]   = "settings/language";
static const char kFontFamily[] = "settings/font_family";
static const char kFontSize[]   = "settings/font_size";
static const char kDeviceListFontSize[] = "settings/device_list_font_size";
static const char kSftpListFontSize[] = "settings/sftp_list_font_size";
static const char kSshTimeout[] = "settings/ssh_timeout";
static const char kEncoding[]   = "settings/terminal_encoding";
static const char kScrollback[] = "settings/scrollback_lines";
static const char kCommandCompletion[] = "settings/command_completion";
static const char kHostKeyVerification[] = "settings/host_key_verification";
static const char kKeepaliveEnabled[] = "settings/ssh_keepalive_enabled";
static const char kKeepaliveInterval[] = "settings/ssh_keepalive_interval_sec";
static const char kKeepaliveGrace[] = "settings/ssh_keepalive_grace_sec";
} // namespace settings_keys

// 对应Python: function/theme.py::MainWindow.__init__（设置窗布局）
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("设置"));
    setMinimumSize(560, 380);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->addTab(createThemeTab(), tr("主题设置"));
    m_tabWidget->addTab(createLanguageTab(), tr("语言设置"));
    m_tabWidget->addTab(createGeneralTab(), tr("通用"));
    // 高亮规则页插在通用之后：代理页必须保持最后，现有 setCurrentTab(0/1/2)
    // 的入口调用不受影响。
    m_tabWidget->addTab(createHighlightTab(), tr("高亮规则"));
    // 代理页排在最后：既有的 setCurrentTab 调用点用的是 0/1/2 三个字面量
    // （见其声明处的注释），插在中间会把那些入口全部错位。
    m_tabWidget->addTab(createProxyTab(), tr("代理"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabWidget, 1);
    layout->addWidget(buttons);

    loadCurrentSettings();

    // 选中主题即实时预览（确认前不落盘；Cancel 经 reject() 还原）。
    // 对应Python: theme.py 单选切换后立即 applyAppearance
    connect(m_themeList, &QListWidget::currentItemChanged, this,
            [](QListWidgetItem *current, QListWidgetItem *) {
                if (current)
                    ThemeManager::applyTheme(qApp, current->data(Qt::UserRole).toString());
            });
}

// 打开后选中指定 Tab（主题设置入口定位主题页，通用设置入口定位通用页）。
void SettingsDialog::setCurrentTab(int index)
{
    if (index >= 0 && index < m_tabWidget->count())
        m_tabWidget->setCurrentIndex(index);
}

// 主题 Tab：可用主题列表 + 实时预览说明。
// 对应Python: theme.py 的 dark/light 单选
QWidget *SettingsDialog::createThemeTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *tip = new QLabel(tr("选中主题即可实时预览，点“确定”后应用并保存。"), page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(tip);

    m_themeList = new QListWidget(page);
    for (const QString &name : ThemeManager::availableThemes()) {
        auto *item = new QListWidgetItem(
            name.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0
                ? tr("亮色") : tr("暗色"),
            m_themeList);
        item->setData(Qt::UserRole, name);
    }
    layout->addWidget(m_themeList, 1);
    return page;
}

// 语言 Tab：语言下拉（重启后全量生效）。
// 对应Python: cube-shell.py::show_language_settings 的语言下拉
QWidget *SettingsDialog::createLanguageTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_language = new QComboBox(page);
    const auto langs = LanguageManager::supportedLanguages();
    for (const LanguageInfo &info : langs)
        m_language->addItem(QStringLiteral("%1 (%2)").arg(info.nativeName, info.code), info.code);
    unifyFieldWidth(m_language);
    addFullTextToolTip(m_language);
    form->addRow(tr("选择应用程序语言"), m_language);

    auto *hint = new QLabel(tr("更改语言后需要重启应用程序才能生效"), page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    form->addRow(hint);
    return page;
}

// 通用 Tab：字体 / SSH 超时 / 默认终端编码。
// 对应Python: theme.py::apply_font_settings + 命令执行超时策略
QWidget *SettingsDialog::createGeneralTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_fontFamily = new QFontComboBox(page);
    m_fontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    unifyFieldWidth(m_fontFamily);
    addFullTextToolTip(m_fontFamily);
    form->addRow(tr("终端字体"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(8, 40);
    unifyFieldWidth(m_fontSize);
    form->addRow(tr("终端字体大小:"), m_fontSize);

    // 设备列表（左侧分组/设备树）字号，与终端字号相互独立。
    m_deviceListFontSize = new QSpinBox(page);
    m_deviceListFontSize->setRange(8, 40);
    unifyFieldWidth(m_deviceListFontSize);
    form->addRow(tr("设备列表字体大小:"), m_deviceListFontSize);

    // 文件目录树字号独立于终端和设备列表；名称沿用“SFTP 列表”，实际同时
    // 应用于本地连接与远程连接，避免同一文件区域在两种会话下字号不一致。
    m_sftpListFontSize = new QSpinBox(page);
    m_sftpListFontSize->setObjectName(QStringLiteral("sftpListFontSize"));
    m_sftpListFontSize->setRange(8, 40);
    m_sftpListFontSize->setToolTip(tr("本地与远程 SFTP 文件目录树的字号，保存后立即生效。"));
    unifyFieldWidth(m_sftpListFontSize);
    form->addRow(tr("SFTP 列表字体大小:"), m_sftpListFontSize);

    m_sshTimeout = new QSpinBox(page);
    m_sshTimeout->setRange(5, 600);
    m_sshTimeout->setSuffix(tr(" 秒"));
    unifyFieldWidth(m_sshTimeout);
    form->addRow(tr("SSH 连接超时："), m_sshTimeout);

    m_encoding = new QComboBox(page);
    m_encoding->addItems({QStringLiteral("UTF-8"), QStringLiteral("GBK"),
                          QStringLiteral("GB2312")});
    unifyFieldWidth(m_encoding);
    addFullTextToolTip(m_encoding);
    form->addRow(tr("默认终端编码："), m_encoding);

    // 回滚行数：决定能往回翻多少输出，也决定「查找」能检索到多大范围。
    m_scrollback = new QSpinBox(page);
    m_scrollback->setRange(0, 1000000);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setSuffix(tr(" 行"));
    m_scrollback->setSpecialValueText(tr("不保留"));   // 0
    m_scrollback->setToolTip(tr("终端保留的历史输出行数，也是“查找”能检索的范围。\n"
                                "行数越大越占内存，排查线上日志建议 10000 以上。"));
    unifyFieldWidth(m_scrollback);
    form->addRow(tr("终端回滚行数："), m_scrollback);

    // 命令补全总开关：关掉后终端输入时不再弹候选窗、Ctrl+Space 也不唤起。
    // 默认开启，保持加开关之前的行为；历史命令始终照常记录。
    m_commandCompletion = new QCheckBox(tr("启用（输入时自动弹出候选，Ctrl+Space 手动唤起）"), page);
    m_commandCompletion->setToolTip(tr("开启后在终端输入时自动弹出命令补全候选（历史命令 + 常用命令）。\n"
                                       "关闭则完全不弹候选窗，Ctrl+Space 也不再唤起。"));
    form->addRow(tr("终端命令补全："), m_commandCompletion);

    // 主机密钥校验策略：决定首次连接或密钥变更时如何提示/阻断。
    m_hostKeyVerification = new QComboBox(page);
    m_hostKeyVerification->addItem(tr("询问（首次连接确认，变更时阻断）"), 1); // Ask
    m_hostKeyVerification->addItem(tr("自动接受新密钥并保存"), 2);            // AcceptNew
    m_hostKeyVerification->addItem(tr("严格（拒绝未知和变更）"), 0);           // Strict
    m_hostKeyVerification->addItem(tr("关闭校验"), 3);                        // Off
    m_hostKeyVerification->setToolTip(tr("严格模式会拒绝任何未知或已变更密钥的主机；\n"
                                         "询问模式会在首次连接时弹窗确认；\n"
                                         "自动接受新模式会静默记录首次密钥但拒绝变更。"));
    unifyFieldWidth(m_hostKeyVerification);
    addFullTextToolTip(m_hostKeyVerification);
    form->addRow(tr("SSH 主机密钥校验："), m_hostKeyVerification);

    m_keepaliveEnabled = new QCheckBox(tr("启用 SSH 保活"), page);
    m_keepaliveEnabled->setToolTip(tr("周期性发送 SSH keepalive，防止 NAT/防火墙切断空闲连接。"));
    form->addRow(tr("SSH 保活："), m_keepaliveEnabled);

    m_keepaliveInterval = new QSpinBox(page);
    m_keepaliveInterval->setRange(5, 600);
    m_keepaliveInterval->setSuffix(tr(" 秒"));
    m_keepaliveInterval->setToolTip(tr("向服务器发送 keepalive 的间隔。"));
    unifyFieldWidth(m_keepaliveInterval);
    form->addRow(tr("保活间隔："), m_keepaliveInterval);

    m_keepaliveGrace = new QSpinBox(page);
    m_keepaliveGrace->setRange(10, 300);
    m_keepaliveGrace->setSuffix(tr(" 秒"));
    m_keepaliveGrace->setToolTip(tr("连续多久未收到 keepalive 应答后判定连接已断开。"));
    unifyFieldWidth(m_keepaliveGrace);
    form->addRow(tr("无响应判定断开："), m_keepaliveGrace);

    // --- 会话日志录制（SSH/Telnet/串口 共用；C++ 独有，只写 GlobalState）---
    auto *logDirRow = new QWidget(page);
    auto *logDirLayout = new QHBoxLayout(logDirRow);
    logDirLayout->setContentsMargins(0, 0, 0, 0);
    m_logDir = new QLineEdit(logDirRow);
    m_logDir->setPlaceholderText(tr("留空使用默认目录（数据目录/session-logs）"));
    m_logDir->setToolTip(tr("会话日志的保存目录。"));
    auto *browseBtn = new QPushButton(tr("浏览…"), logDirRow);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(
            this, tr("选择会话日志目录"), m_logDir->text());
        if (!d.isEmpty())
            m_logDir->setText(d);
    });
    logDirLayout->addWidget(m_logDir, 1);
    logDirLayout->addWidget(browseBtn);
    form->addRow(tr("日志录制目录："), logDirRow);

    m_logAutoName = new QCheckBox(tr("自动按“设备名 + 时间”命名"), page);
    m_logAutoName->setToolTip(tr("开启后录制直接落到上面目录，无需每次另存为。"));
    form->addRow(tr("日志命名："), m_logAutoName);

    m_logTimestamps = new QCheckBox(tr("行首加时间戳"), page);
    m_logTimestamps->setToolTip(tr("审计定位用：在每行输出前加 [HH:MM:SS.zzz]。"));
    form->addRow(tr("日志时间戳："), m_logTimestamps);

    m_logMaxMB = new QSpinBox(page);
    m_logMaxMB->setRange(0, 10240);
    m_logMaxMB->setSuffix(tr(" MB"));
    m_logMaxMB->setSpecialValueText(tr("不轮转"));
    m_logMaxMB->setToolTip(tr("单个日志文件超过该大小就切到新卷；0 表示不轮转。"));
    unifyFieldWidth(m_logMaxMB);
    form->addRow(tr("单文件上限："), m_logMaxMB);

    m_logBackupCount = new QSpinBox(page);
    m_logBackupCount->setRange(1, 100);
    m_logBackupCount->setToolTip(tr("轮转时保留的历史卷数，最老的超出即删除。"));
    unifyFieldWidth(m_logBackupCount);
    form->addRow(tr("保留卷数："), m_logBackupCount);
    return page;
}

// 高亮规则 Tab：WindTerm 风格的文本正则高亮。
// 表格行序即优先级（第一行优先），与终端「首匹配优先」的着色语义一致。
QWidget *SettingsDialog::createHighlightTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *tip = new QLabel(
        tr("规则从上到下依次匹配，第一个命中的颜色生效；用「上移 / 下移」调整优先级。\n"
           "文件权限串（drwxr-xr-x）始终按位着色，不在本列表中。\n"
           "提示：用户正则运行在界面线程，过于复杂的表达式（如 (a+)+$）可能造成卡顿。"),
        page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(tip);

    m_highlightTable = new QTableWidget(0, 5, page);
    m_highlightTable->setHorizontalHeaderLabels(
        {tr("启用"), tr("规则名称"), tr("正则表达式"), tr("区分大小写"), tr("颜色")});
    m_highlightTable->verticalHeader()->setVisible(false);
    m_highlightTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_highlightTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_highlightTable->setAlternatingRowColors(true);
    QHeaderView *header = m_highlightTable->horizontalHeader();
    header->setSectionResizeMode(kHlColEnabled, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kHlColName, QHeaderView::Interactive);
    header->setSectionResizeMode(kHlColPattern, QHeaderView::Stretch);
    header->setSectionResizeMode(kHlColCase, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kHlColColor, QHeaderView::ResizeToContents);
    m_highlightTable->setColumnWidth(kHlColName, 140);
    layout->addWidget(m_highlightTable, 1);

    // 正则就地校验：非法行标红并给出 errorString，保存时仍会统一拦截。
    connect(m_highlightTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (!item || item->column() != kHlColPattern)
                    return;
                const QString pattern = item->text();
                QRegularExpression re(pattern);
                const bool ok = !pattern.trimmed().isEmpty() && re.isValid();
                {
                    QSignalBlocker blocker(m_highlightTable);
                    if (ok) {
                        item->setData(Qt::BackgroundRole, QVariant());
                        item->setToolTip(QString());
                    } else {
                        item->setData(Qt::BackgroundRole,
                                      QBrush(QColor(220, 80, 80, 120)));
                        item->setToolTip(pattern.trimmed().isEmpty()
                                             ? tr("正则不能为空")
                                             : re.errorString());
                    }
                }
                updateHighlightPreview();
            });

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("新增"), page);
    auto *copyButton = new QPushButton(tr("复制"), page);
    auto *removeButton = new QPushButton(tr("删除"), page);
    auto *upButton = new QPushButton(tr("上移"), page);
    auto *downButton = new QPushButton(tr("下移"), page);
    auto *restoreButton = new QPushButton(tr("恢复默认"), page);
    for (QPushButton *button : {addButton, copyButton, removeButton,
                                upButton, downButton, restoreButton})
        buttons->addWidget(button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, [this] {
        QList<Konsole::HighlightRule> rules = currentHighlightRules();
        Konsole::HighlightRule rule;
        rule.name = tr("新规则");
        rule.pattern = QStringLiteral("error");
        rule.foreground = QColor(QStringLiteral("#e74c3c"));
        rules.append(rule);
        setHighlightRows(rules);
        m_highlightTable->selectRow(rules.size() - 1);
        m_highlightTable->setCurrentCell(rules.size() - 1, kHlColPattern);
    });

    connect(copyButton, &QPushButton::clicked, this, [this] {
        const int row = m_highlightTable->currentRow();
        if (row < 0)
            return;
        QList<Konsole::HighlightRule> rules = currentHighlightRules();
        rules.insert(row + 1, rules.at(row));
        setHighlightRows(rules);
        m_highlightTable->selectRow(row + 1);
    });

    connect(removeButton, &QPushButton::clicked, this, [this] {
        const int row = m_highlightTable->currentRow();
        if (row < 0)
            return;
        QList<Konsole::HighlightRule> rules = currentHighlightRules();
        rules.removeAt(row);
        setHighlightRows(rules);
        if (!rules.isEmpty())
            m_highlightTable->selectRow(qMin(row, rules.size() - 1));
    });

    auto moveRow = [this](int step) {
        const int row = m_highlightTable->currentRow();
        const int target = row + step;
        if (row < 0 || target < 0 || target >= m_highlightTable->rowCount())
            return;
        QList<Konsole::HighlightRule> rules = currentHighlightRules();
        rules.swapItemsAt(row, target);
        setHighlightRows(rules);
        m_highlightTable->selectRow(target);
    };
    connect(upButton, &QPushButton::clicked, this, [moveRow] { moveRow(-1); });
    connect(downButton, &QPushButton::clicked, this, [moveRow] { moveRow(1); });

    connect(restoreButton, &QPushButton::clicked, this, [this] {
        setHighlightRows(Konsole::builtinHighlightRules());
        m_highlightTable->selectRow(0);
    });

    // 实时预览：改规则、改色、启停、键入样例都会重算。
    auto *previewRow = new QHBoxLayout;
    previewRow->addWidget(new QLabel(tr("预览文本："), page));
    m_highlightPreviewInput = new QLineEdit(
        tr("Could not connect to 192.168.1.10: success=1 failed=0 warning"), page);
    previewRow->addWidget(m_highlightPreviewInput, 1);
    layout->addLayout(previewRow);

    m_highlightPreview = new QLabel(page);
    m_highlightPreview->setTextFormat(Qt::RichText);
    m_highlightPreview->setWordWrap(true);
    m_highlightPreview->setMinimumHeight(30);
    m_highlightPreview->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid); padding: 4px;"));
    layout->addWidget(m_highlightPreview);

    connect(m_highlightPreviewInput, &QLineEdit::textChanged, this,
            [this](const QString &) { updateHighlightPreview(); });
    // 勾选/取消勾选也要刷新预览。
    connect(m_highlightTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *) { updateHighlightPreview(); });

    return page;
}

// 整表重建。上移/下移/复制/恢复默认都走这里，避免带 cellWidget 的行搬运。
void SettingsDialog::setHighlightRows(const QList<Konsole::HighlightRule> &rules)
{
    if (!m_highlightTable)
        return;

    QSignalBlocker blocker(m_highlightTable);
    m_highlightTable->setRowCount(0);
    for (const Konsole::HighlightRule &rule : rules) {
        const int row = m_highlightTable->rowCount();
        m_highlightTable->insertRow(row);

        m_highlightTable->setItem(row, kHlColEnabled, makeCheckableItem(rule.enabled));

        auto *nameItem = new QTableWidgetItem(rule.name);
        nameItem->setFlags(nameItem->flags() | Qt::ItemIsEditable);
        m_highlightTable->setItem(row, kHlColName, nameItem);

        auto *patternItem = new QTableWidgetItem(rule.pattern);
        patternItem->setFlags(patternItem->flags() | Qt::ItemIsEditable);
        if (!patternItem->text().trimmed().isEmpty()) {
            QRegularExpression re(patternItem->text());
            if (!re.isValid()) {
                patternItem->setData(Qt::BackgroundRole,
                                     QBrush(QColor(220, 80, 80, 120)));
                patternItem->setToolTip(re.errorString());
            }
        }
        m_highlightTable->setItem(row, kHlColPattern, patternItem);

        m_highlightTable->setItem(row, kHlColCase, makeCheckableItem(rule.caseSensitive));

        auto *colorButton = new QPushButton(m_highlightTable);
        colorButton->setCursor(Qt::PointingHandCursor);
        paintHighlightColorButton(colorButton, rule.foreground);
        connect(colorButton, &QPushButton::clicked, this, [this, colorButton] {
            const QColor current = colorButton->property("hlColor").value<QColor>();
            const QColor picked =
                QColorDialog::getColor(current, this, tr("选择高亮颜色"));
            if (!picked.isValid())
                return;
            paintHighlightColorButton(colorButton, picked);
            updateHighlightPreview();
        });
        m_highlightTable->setCellWidget(row, kHlColColor, colorButton);
    }

    updateHighlightPreview();
}

QList<Konsole::HighlightRule> SettingsDialog::currentHighlightRules() const
{
    QList<Konsole::HighlightRule> rules;
    if (!m_highlightTable)
        return rules;

    rules.reserve(m_highlightTable->rowCount());
    for (int row = 0; row < m_highlightTable->rowCount(); ++row) {
        Konsole::HighlightRule rule;
        if (auto *item = m_highlightTable->item(row, kHlColEnabled))
            rule.enabled = item->checkState() == Qt::Checked;
        if (auto *item = m_highlightTable->item(row, kHlColName))
            rule.name = item->text();
        if (auto *item = m_highlightTable->item(row, kHlColPattern))
            rule.pattern = item->text();
        if (auto *item = m_highlightTable->item(row, kHlColCase))
            rule.caseSensitive = item->checkState() == Qt::Checked;
        if (auto *button = qobject_cast<QPushButton *>(
                m_highlightTable->cellWidget(row, kHlColColor))) {
            rule.foreground = button->property("hlColor").value<QColor>();
        }
        rules.append(rule);
    }
    return rules;
}

bool SettingsDialog::validateHighlightRules(QString *errorOut, int *badRow) const
{
    if (!m_highlightTable)
        return true;

    for (int row = 0; row < m_highlightTable->rowCount(); ++row) {
        auto *enabledItem = m_highlightTable->item(row, kHlColEnabled);
        // 停用的规则不参与匹配，正则坏了也不阻断保存。
        if (!enabledItem || enabledItem->checkState() != Qt::Checked)
            continue;

        auto *nameItem = m_highlightTable->item(row, kHlColName);
        const QString name = (nameItem && !nameItem->text().isEmpty())
                                 ? nameItem->text()
                                 : tr("未命名");
        auto *patternItem = m_highlightTable->item(row, kHlColPattern);
        const QString pattern = patternItem ? patternItem->text() : QString();

        QRegularExpression re(pattern);
        if (pattern.trimmed().isEmpty()) {
            if (badRow)
                *badRow = row;
            if (errorOut)
                *errorOut = tr("第 %1 行「%2」的正则不能为空。").arg(row + 1).arg(name);
            return false;
        }
        if (!re.isValid()) {
            if (badRow)
                *badRow = row;
            if (errorOut)
                *errorOut = tr("第 %1 行「%2」的正则无效：%3")
                                .arg(row + 1)
                                .arg(name, re.errorString());
            return false;
        }
    }
    return true;
}

void SettingsDialog::updateHighlightPreview()
{
    if (!m_highlightPreview || !m_highlightPreviewInput)
        return;
    const QColor defaultColor =
        m_highlightPreview->palette().color(QPalette::WindowText);
    m_highlightPreview->setText(highlightPreviewHtml(
        m_highlightPreviewInput->text(), currentHighlightRules(), defaultColor));
}

// 代理 Tab：那份「全局代理」。设备的代理类型选「全局代理」时取的就是这里。
// Python 版没有代理功能，故无对应实现。
QWidget *SettingsDialog::createProxyTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *tip = new QLabel(
        tr("这里配置的是「全局代理」：在设备的代理类型里选「全局代理」的设备都走这一份，\n"
           "改一处即全部生效。设备也可以各自配自己的代理，与这份互不影响。"),
        page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(tip);

    // 关掉「全局代理」这一项：这一页**自己就是**全局代理，选出 Global→Global
    // 是唯一的无限递归入口（见 ProxySettingsWidget::setGlobalOptionEnabled）。
    m_proxy = new ProxySettingsWidget(page);
    m_proxy->setGlobalOptionEnabled(false);
    layout->addWidget(m_proxy);
    layout->addStretch(1);
    return page;
}

void SettingsDialog::setProxyDeviceCatalog(const QList<ProxyDeviceItem> &devices)
{
    // excludeId 留空：全局代理不属于任何设备，没有"自己"要排除。
    // （成环由连接期的 flattenJumpChain 兜住——某台跳板机自己又选了「全局代理」
    //   而全局代理正是跳转服务器时会绕回来。）
    m_proxy->setDeviceCatalog(devices);
}

void SettingsDialog::setHasStoredProxyPassword(bool has) { m_proxy->setHasStoredPassword(has); }
bool SettingsDialog::proxyPasswordEdited() const { return m_proxy->passwordEdited(); }
QString SettingsDialog::proxyPassword() const { return m_proxy->config().password; }

// 对应Python: function/theme.py::MainWindow._load_current_settings
// QSettings 优先，缺省回退 GlobalState（theme.json，与 Python 版共享）。
void SettingsDialog::loadCurrentSettings()
{
    GlobalState &state = GlobalState::instance();
    QSettings qs;

    m_originalAppearance =
        qs.value(settings_keys::kTheme, state.appearance()).toString();
    for (int i = 0; i < m_themeList->count(); ++i) {
        if (m_themeList->item(i)->data(Qt::UserRole).toString() == m_originalAppearance) {
            m_themeList->setCurrentRow(i);
            break;
        }
    }
    if (!m_themeList->currentItem() && m_themeList->count() > 0)
        m_themeList->setCurrentRow(0);

    const int langIdx = m_language->findData(
        qs.value(settings_keys::kLanguage, state.language()).toString());
    if (langIdx >= 0)
        m_language->setCurrentIndex(langIdx);

    const QString family =
        qs.value(settings_keys::kFontFamily, state.fontFamily()).toString();
    if (!family.isEmpty())
        m_fontFamily->setCurrentFont(QFont(family));
    m_fontSize->setValue(qs.value(settings_keys::kFontSize, state.fontSize()).toInt());
    m_deviceListFontSize->setValue(
        qs.value(settings_keys::kDeviceListFontSize, state.deviceListFontSize()).toInt());
    m_sftpListFontSize->setValue(
        qs.value(settings_keys::kSftpListFontSize, state.sftpListFontSize()).toInt());

    m_sshTimeout->setValue(qs.value(
        settings_keys::kSshTimeout,
        state.sshConnectTimeoutSeconds()).toInt());

    const int encIdx = m_encoding->findText(
        qs.value(settings_keys::kEncoding, QStringLiteral("UTF-8")).toString());
    if (encIdx >= 0)
        m_encoding->setCurrentIndex(encIdx);

    m_scrollback->setValue(
        qs.value(settings_keys::kScrollback, state.scrollbackLines()).toInt());

    m_commandCompletion->setChecked(
        qs.value(settings_keys::kCommandCompletion,
                 state.commandCompletionEnabled()).toBool());

    const int hkv = qs.value(settings_keys::kHostKeyVerification,
                             state.hostKeyVerification()).toInt();
    const int hkvIdx = m_hostKeyVerification->findData(hkv);
    if (hkvIdx >= 0)
        m_hostKeyVerification->setCurrentIndex(hkvIdx);

    m_keepaliveEnabled->setChecked(
        qs.value(settings_keys::kKeepaliveEnabled, state.sshKeepaliveEnabled()).toBool());
    m_keepaliveInterval->setValue(
        qs.value(settings_keys::kKeepaliveInterval, state.sshKeepaliveIntervalSeconds()).toInt());
    m_keepaliveGrace->setValue(
        qs.value(settings_keys::kKeepaliveGrace, state.sshKeepaliveGraceSeconds()).toInt());

    // 全局代理：**只从 GlobalState 读**，不走 QSettings。
    //
    // 上面每一项都是双写（QSettings + theme.json），因为那些键 Python 版也在用。
    // 代理是 C++ 独有的，而真正读它的只有 GlobalState::sshProxyConfig()
    //（SshClient 建连时取），再存一份 QSettings 副本就是造出第二个真相源——
    // 两边一旦不一致，连接期用的是哪一份完全看不出来。
    m_proxy->setConfig(state.sshProxyConfig());

    // 会话日志录制：同为 C++ 独有，读它的只有录制路径（SshTerminalWidget），
    // 与代理同理只走 GlobalState 单一真相源。
    m_logDir->setText(state.sessionLogDir());
    m_logAutoName->setChecked(state.sessionLogAutoName());
    m_logTimestamps->setChecked(state.sessionLogTimestamps());
    m_logMaxMB->setValue(state.sessionLogMaxMB());
    m_logBackupCount->setValue(state.sessionLogBackupCount());

    // 高亮规则：C++ 专属，只走 QSettings（键不存在时回退内置默认）。
    setHighlightRows(loadHighlightRules());
}

int SettingsDialog::sshTimeoutSeconds() const
{
    return m_sshTimeout->value();
}

QString SettingsDialog::terminalEncoding() const
{
    return m_encoding->currentText();
}

// 对应Python: function/theme.py::MainWindow._set_appearance / apply_font_settings
void SettingsDialog::accept()
{
    GlobalState &state = GlobalState::instance();
    QSettings qs;

    // 代理页先校验，不通过就停在原地。填了一半的代理配置比压根不填更糟：
    // 连接期只会得到一句"连接超时"，没人能从那句话看出是代理地址空着。
    QString proxyErr;
    if (!m_proxy->validate(&proxyErr)) {
        if (QWidget *proxyPage = m_proxy->parentWidget())
            m_tabWidget->setCurrentIndex(m_tabWidget->indexOf(proxyPage));
        QMessageBox::warning(this, tr("代理设置"), proxyErr);
        return;   // 刻意不调 QDialog::accept()：对话框留着让用户改
    }

    // 高亮规则：任何一条「启用中」的规则正则非法就停在原地，
    // 与代理页同样的处理方式——留着对话框让用户改那一行。
    QString hlErr;
    int hlBadRow = -1;
    if (!validateHighlightRules(&hlErr, &hlBadRow)) {
        if (QWidget *hlPage = m_highlightTable->parentWidget())
            m_tabWidget->setCurrentIndex(m_tabWidget->indexOf(hlPage));
        if (hlBadRow >= 0)
            m_highlightTable->selectRow(hlBadRow);
        QMessageBox::warning(this, tr("高亮规则"), hlErr);
        return;
    }

    // 主题：实时预览已应用，这里确认并落盘。
    QString appearance = m_originalAppearance;
    if (QListWidgetItem *item = m_themeList->currentItem())
        appearance = item->data(Qt::UserRole).toString();
    const bool appearanceDirty = appearance != state.appearance();
    state.setAppearance(appearance);
    ThemeManager::applyTheme(qApp, appearance);

    const QString family = m_fontFamily->currentFont().family();
    const int size = m_fontSize->value();
    const bool fontDirty = family != state.fontFamily() || size != state.fontSize();
    state.setFont(family, size);

    const int deviceListSize = m_deviceListFontSize->value();
    const bool deviceListFontDirty = deviceListSize != state.deviceListFontSize();
    state.setDeviceListFontSize(deviceListSize);

    const int sftpListSize = m_sftpListFontSize->value();
    const bool sftpListFontDirty = sftpListSize != state.sftpListFontSize();
    state.setSftpListFontSize(sftpListSize);

    state.setSshConnectTimeoutSeconds(m_sshTimeout->value());
    state.setHostKeyVerification(m_hostKeyVerification->currentData().toInt());
    state.setSshKeepaliveEnabled(m_keepaliveEnabled->isChecked());
    state.setSshKeepaliveIntervalSeconds(m_keepaliveInterval->value());
    state.setSshKeepaliveGraceSeconds(m_keepaliveGrace->value());

    // 全局代理：只写 GlobalState（理由见 loadCurrentSettings 里那段）。
    // 口令不在这里——writeJson 不写它，落钥匙串由 MainWindow 在本对话框
    // 返回之后办（见 proxyPasswordEdited()）。
    state.setSshProxyConfig(m_proxy->config());

    // 会话日志录制：同样只写 GlobalState（单一真相源）。
    state.setSessionLogDir(m_logDir->text().trimmed());
    state.setSessionLogAutoName(m_logAutoName->isChecked());
    state.setSessionLogTimestamps(m_logTimestamps->isChecked());
    state.setSessionLogMaxMB(m_logMaxMB->value());
    state.setSessionLogBackupCount(m_logBackupCount->value());

    const int scrollback = m_scrollback->value();
    const bool scrollbackDirty = scrollback != state.scrollbackLines();
    state.setScrollbackLines(scrollback);

    const bool completion = m_commandCompletion->isChecked();
    const bool completionDirty = completion != state.commandCompletionEnabled();
    state.setCommandCompletionEnabled(completion);

    // QSettings 持久化（本对话框全部设置项）。
    qs.setValue(settings_keys::kTheme, appearance);
    qs.setValue(settings_keys::kLanguage, m_language->currentData().toString());
    qs.setValue(settings_keys::kFontFamily, family);
    qs.setValue(settings_keys::kFontSize, size);
    qs.setValue(settings_keys::kDeviceListFontSize, deviceListSize);
    qs.setValue(settings_keys::kSftpListFontSize, sftpListSize);
    qs.setValue(settings_keys::kSshTimeout, m_sshTimeout->value());
    qs.setValue(settings_keys::kEncoding, m_encoding->currentText());
    qs.setValue(settings_keys::kScrollback, scrollback);
    qs.setValue(settings_keys::kCommandCompletion, completion);
    qs.setValue(settings_keys::kHostKeyVerification, m_hostKeyVerification->currentData().toInt());
    qs.setValue(settings_keys::kKeepaliveEnabled, m_keepaliveEnabled->isChecked());
    qs.setValue(settings_keys::kKeepaliveInterval, m_keepaliveInterval->value());
    qs.setValue(settings_keys::kKeepaliveGrace, m_keepaliveGrace->value());

    // theme.json 同步写回（与 Python 版共享配置）。
    QString err;
    if (!state.saveTheme(&err) && !err.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("保存设置失败: %1").arg(err));

    if (appearanceDirty)
        emit appearanceChanged(appearance);
    if (fontDirty)
        emit fontChanged(family, size);
    if (deviceListFontDirty)
        emit deviceListFontSizeChanged(deviceListSize);
    if (sftpListFontDirty)
        emit sftpListFontSizeChanged(sftpListSize);
    if (scrollbackDirty)
        emit scrollbackLinesChanged(scrollback);
    if (completionDirty)
        emit commandCompletionEnabledChanged(completion);

    // 语言切换（LanguageManager 内部会写回 GlobalState 并持久化）。
    const QString langCode = m_language->currentData().toString();
    if (langCode != LanguageManager::instance().currentLanguage()) {
        LanguageManager::instance().setLanguage(langCode);
        // 对应Python: show_language_settings 里的重启提示
        QMessageBox::information(this, tr("语言设置"),
                                 tr("语言设置已更改，请重启应用程序以生效。"));
    }

    // 高亮规则：C++ 专属，只写 QSettings。保存成功后通知主窗口把规则
    // 应用到已打开的终端（无需重开会话）。
    QString hlSaveErr;
    if (saveHighlightRules(currentHighlightRules(), &hlSaveErr)) {
        emit highlightRulesChanged();
    } else {
        QMessageBox::warning(this, windowTitle(),
                             tr("保存高亮规则失败: %1").arg(hlSaveErr));
    }

    QDialog::accept();
}

// Cancel/Esc：撤销主题实时预览，恢复打开对话框时的外观。
void SettingsDialog::reject()
{
    QListWidgetItem *item = m_themeList->currentItem();
    const QString previewed =
        item ? item->data(Qt::UserRole).toString() : m_originalAppearance;
    if (previewed != m_originalAppearance)
        ThemeManager::applyTheme(qApp, m_originalAppearance);
    QDialog::reject();
}

} // namespace cubeshell
