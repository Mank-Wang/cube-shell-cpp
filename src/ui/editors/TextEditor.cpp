#include "TextEditor.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include "CodeEditor.h"

namespace cubeshell {

TextEditor::TextEditor(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("textEditor"));
    setWindowTitle(tr("文本编辑"));
    setMinimumSize(720, 480);
    resize(1100, 720);

    m_editor = new CodeEditor(this);
    setFocusProxy(m_editor);

    // --- 顶部工具栏 ---------------------------------------------------------
    // 高频命令全部保留标准快捷键；图标只使用平台标准图标，避免各平台风格割裂。
    auto *toolBar = new QToolBar(tr("编辑工具栏"), this);
    toolBar->setObjectName(QStringLiteral("editorToolBar"));
    toolBar->setMovable(false);
    toolBar->setFloatable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_saveAction = toolBar->addAction(
        style()->standardIcon(QStyle::SP_DialogSaveButton), tr("保存"),
        this, &TextEditor::onSave);
    m_saveAction->setObjectName(QStringLiteral("saveAction"));
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setToolTip(tr("保存 (%1)").arg(QKeySequence(QKeySequence::Save)
                                                        .toString(QKeySequence::NativeText)));
    toolBar->addSeparator();

    m_undoAction = toolBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowBack), tr("撤销"),
        m_editor, &QPlainTextEdit::undo);
    m_undoAction->setObjectName(QStringLiteral("undoAction"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = toolBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowForward), tr("重做"),
        m_editor, &QPlainTextEdit::redo);
    m_redoAction->setObjectName(QStringLiteral("redoAction"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_undoAction->setEnabled(false);
    m_redoAction->setEnabled(false);
    toolBar->addSeparator();

    QAction *findAction = toolBar->addAction(tr("查找"));
    findAction->setObjectName(QStringLiteral("findAction"));
    findAction->setShortcut(QKeySequence::Find);
    findAction->setToolTip(tr("查找 (%1)").arg(QKeySequence(QKeySequence::Find)
                                                       .toString(QKeySequence::NativeText)));
    connect(findAction, &QAction::triggered, this, [this] { showSearchBar(false); });

    QAction *replaceAction = toolBar->addAction(tr("替换"));
    replaceAction->setObjectName(QStringLiteral("replaceAction"));
    replaceAction->setShortcut(QKeySequence::Replace);
    connect(replaceAction, &QAction::triggered, this, [this] { showSearchBar(true); });

    QAction *goToLineAction = toolBar->addAction(tr("跳转到行"));
    goToLineAction->setObjectName(QStringLiteral("goToLineAction"));
    goToLineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
    goToLineAction->setToolTip(tr("跳转到指定行 (Ctrl+G)"));
    connect(goToLineAction, &QAction::triggered, this, &TextEditor::showGoToLineDialog);

    m_wordWrapAction = toolBar->addAction(tr("自动换行"));
    m_wordWrapAction->setObjectName(QStringLiteral("wordWrapAction"));
    m_wordWrapAction->setCheckable(true);
    m_wordWrapAction->setToolTip(tr("在窗口宽度处折行，不改变文件内容"));
    connect(m_wordWrapAction, &QAction::toggled, this, [this](bool enabled) {
        m_editor->setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth
                                          : QPlainTextEdit::NoWrap);
        m_message->setText(enabled ? tr("已开启自动换行") : tr("已关闭自动换行"));
    });

    auto *spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(spacer);
    auto *shortcutHint = new QLabel(tr("F3 下一个 · Shift+F3 上一个"), toolBar);
    shortcutHint->setObjectName(QStringLiteral("shortcutHint"));
    toolBar->addWidget(shortcutHint);

    // --- 查找 / 替换面板 ----------------------------------------------------
    // 查找与替换分成两行，避免旧版所有控件挤在一行；替换行仅在使用时展开。
    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName(QStringLiteral("searchBar"));
    auto *searchLayout = new QVBoxLayout(m_searchBar);
    searchLayout->setContentsMargins(8, 5, 8, 5);
    searchLayout->setSpacing(4);

    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(5);
    m_findEdit = new QLineEdit(m_searchBar);
    m_findEdit->setObjectName(QStringLiteral("findEdit"));
    m_findEdit->setPlaceholderText(tr("查找"));
    m_findEdit->setClearButtonEnabled(true);
    m_findEdit->setMinimumWidth(220);
    findRow->addWidget(m_findEdit, 1);

    m_matchStatus = new QLabel(m_searchBar);
    m_matchStatus->setObjectName(QStringLiteral("matchStatus"));
    m_matchStatus->setMinimumWidth(64);
    m_matchStatus->setAlignment(Qt::AlignCenter);
    findRow->addWidget(m_matchStatus);

    m_findPrevButton = new QPushButton(QStringLiteral("↑"), m_searchBar);
    m_findPrevButton->setObjectName(QStringLiteral("findPreviousButton"));
    m_findPrevButton->setToolTip(tr("上一个 (Shift+F3)"));
    m_findPrevButton->setFixedWidth(30);
    findRow->addWidget(m_findPrevButton);
    m_findNextButton = new QPushButton(QStringLiteral("↓"), m_searchBar);
    m_findNextButton->setObjectName(QStringLiteral("findNextButton"));
    m_findNextButton->setToolTip(tr("下一个 (F3)"));
    m_findNextButton->setFixedWidth(30);
    findRow->addWidget(m_findNextButton);

    // 三个查找选项作为一组保持清晰间距，并禁止窗口变窄时互相挤压。
    auto *searchOptionsLayout = new QHBoxLayout;
    searchOptionsLayout->setContentsMargins(0, 0, 0, 0);
    searchOptionsLayout->setSpacing(20);
    searchOptionsLayout->setSizeConstraint(QLayout::SetMinimumSize);
    m_caseCheck = new QCheckBox(tr("区分大小写"), m_searchBar);
    m_caseCheck->setObjectName(QStringLiteral("caseSensitiveCheck"));
    searchOptionsLayout->addWidget(m_caseCheck);
    m_wordCheck = new QCheckBox(tr("全词匹配"), m_searchBar);
    m_wordCheck->setObjectName(QStringLiteral("wholeWordCheck"));
    searchOptionsLayout->addWidget(m_wordCheck);
    m_regexCheck = new QCheckBox(tr("正则"), m_searchBar);
    m_regexCheck->setObjectName(QStringLiteral("regexCheck"));
    m_regexCheck->setToolTip(tr("使用正则表达式；替换时可用 \\1、\\2 引用捕获组"));
    searchOptionsLayout->addWidget(m_regexCheck);
    findRow->addLayout(searchOptionsLayout);

    auto *closeSearchButton = new QPushButton(QStringLiteral("×"), m_searchBar);
    closeSearchButton->setObjectName(QStringLiteral("closeSearchButton"));
    closeSearchButton->setToolTip(tr("关闭查找 (Esc)"));
    closeSearchButton->setFixedWidth(28);
    findRow->addWidget(closeSearchButton);
    searchLayout->addLayout(findRow);

    m_replaceRow = new QWidget(m_searchBar);
    m_replaceRow->setObjectName(QStringLiteral("replaceRow"));
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(5);
    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setObjectName(QStringLiteral("replaceEdit"));
    m_replaceEdit->setPlaceholderText(tr("替换为"));
    m_replaceEdit->setClearButtonEnabled(true);
    replaceLayout->addWidget(m_replaceEdit, 1);
    m_replaceButton = new QPushButton(tr("替换"), m_replaceRow);
    m_replaceButton->setObjectName(QStringLiteral("replaceButton"));
    m_replaceButton->setToolTip(tr("替换当前匹配项并定位下一项"));
    replaceLayout->addWidget(m_replaceButton);
    m_replaceAllButton = new QPushButton(tr("全部替换"), m_replaceRow);
    m_replaceAllButton->setObjectName(QStringLiteral("replaceAllButton"));
    replaceLayout->addWidget(m_replaceAllButton);
    replaceLayout->addStretch(1);
    searchLayout->addWidget(m_replaceRow);

    // --- 底部状态栏 ---------------------------------------------------------
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(8, 3, 8, 4);
    statusRow->setSpacing(14);
    m_message = new QLabel(tr("就绪"), this);
    m_message->setObjectName(QStringLiteral("editorMessage"));
    statusRow->addWidget(m_message, 1);
    m_cursorStatus = new QLabel(this);
    m_cursorStatus->setObjectName(QStringLiteral("cursorStatus"));
    statusRow->addWidget(m_cursorStatus);
    m_documentStatus = new QLabel(this);
    m_documentStatus->setObjectName(QStringLiteral("documentStatus"));
    statusRow->addWidget(m_documentStatus);
    statusRow->addWidget(new QLabel(tr("空格：4"), this));
    statusRow->addWidget(new QLabel(tr("UTF-8"), this));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolBar);
    layout->addWidget(m_searchBar);
    layout->addWidget(m_editor, 1);
    layout->addLayout(statusRow);
    m_searchBar->setVisible(false);
    m_replaceRow->setVisible(false);

    connect(m_findNextButton, &QPushButton::clicked, this, &TextEditor::onFindNext);
    connect(m_findPrevButton, &QPushButton::clicked, this, &TextEditor::onFindPrev);
    connect(m_replaceButton, &QPushButton::clicked, this, &TextEditor::onReplace);
    connect(m_replaceAllButton, &QPushButton::clicked, this, &TextEditor::onReplaceAll);
    connect(closeSearchButton, &QPushButton::clicked, this, &TextEditor::hideSearchBar);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &TextEditor::onFindNext);
    connect(m_replaceEdit, &QLineEdit::returnPressed, this, &TextEditor::onReplace);
    connect(m_findEdit, &QLineEdit::textChanged, this, [this] { updateSearchResults(true); });
    const auto optionsChanged = [this] { updateSearchResults(false); };
    connect(m_regexCheck, &QCheckBox::toggled, this, optionsChanged);
    connect(m_caseCheck, &QCheckBox::toggled, this, optionsChanged);
    connect(m_wordCheck, &QCheckBox::toggled, this, optionsChanged);

    connect(m_editor, &QPlainTextEdit::undoAvailable,
            m_undoAction, &QAction::setEnabled);
    connect(m_editor, &QPlainTextEdit::redoAvailable,
            m_redoAction, &QAction::setEnabled);
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        updateStatusBar();
        if (m_searchBar->isVisible())
            updateSearchResults(false);
    });
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        updateStatusBar();
        if (m_searchBar->isVisible())
            updateSearchPosition();
    });
    connect(m_editor->document(), &QTextDocument::modificationChanged,
            this, [this] {
                updateWindowTitle();
                updateSaveAction();
            });

    // F3 在查找框和编辑区都可使用；Esc 仅在查找面板可见时关闭面板。
    auto *findNextAction = new QAction(this);
    findNextAction->setShortcut(QKeySequence::FindNext);
    connect(findNextAction, &QAction::triggered, this, &TextEditor::onFindNext);
    addAction(findNextAction);
    auto *findPreviousAction = new QAction(this);
    findPreviousAction->setShortcut(QKeySequence::FindPrevious);
    connect(findPreviousAction, &QAction::triggered, this, &TextEditor::onFindPrev);
    addAction(findPreviousAction);
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchBar);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, &TextEditor::hideSearchBar);

    updateWindowTitle();
    updateSaveAction();
    updateStatusBar();
}

void TextEditor::setPlainText(const QString &text)
{
    m_saveInProgress = false;
    m_closeAfterSave = false;
    m_editor->setPlainText(text);
    m_editor->document()->setModified(false);
    m_editor->clearSearchHighlights();
    QTextCursor cursor = m_editor->textCursor();
    cursor.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(cursor);
    m_message->setText(tr("就绪"));
    updateWindowTitle();
    updateSaveAction();
    updateStatusBar();
}

QString TextEditor::toPlainText() const
{
    return m_editor->toPlainText();
}

void TextEditor::setFileLabel(const QString &label)
{
    m_fileLabel = label;
    updateWindowTitle();
}

bool TextEditor::isModified() const
{
    return m_editor->document()->isModified();
}

void TextEditor::updateWindowTitle()
{
    const QString label = m_fileLabel.isEmpty() ? tr("未命名") : m_fileLabel;
    setWindowTitle(tr("%1%2 - 文本编辑")
                       .arg(isModified() ? QStringLiteral("*") : QString(), label));
}

void TextEditor::updateSaveAction()
{
    m_saveAction->setEnabled(isModified() && !m_saveInProgress);
    m_saveAction->setText(m_saveInProgress ? tr("保存中…") : tr("保存"));
}

void TextEditor::updateStatusBar()
{
    const QTextCursor cursor = m_editor->textCursor();
    QString position = tr("行 %1，列 %2")
                           .arg(cursor.blockNumber() + 1)
                           .arg(cursor.positionInBlock() + 1);
    if (cursor.hasSelection())
        position += tr(" · 已选 %1 字符").arg(cursor.selectedText().size());
    m_cursorStatus->setText(position);
    m_documentStatus->setText(tr("%1 行 · %2 字符")
                                  .arg(m_editor->blockCount())
                                  .arg(m_editor->document()->characterCount() - 1));
}

void TextEditor::onSave()
{
    if (m_saveInProgress)
        return;
    if (!isModified()) {
        m_message->setText(tr("没有需要保存的修改"));
        return;
    }

    // 不在信号发出后立即清除 modified：真正的写回可能失败，也可能仍在后台进行。
    m_saveInProgress = true;
    m_message->setText(tr("正在保存…"));
    updateSaveAction();
    emit saveRequested(m_editor->toPlainText());
}

void TextEditor::notifySaveSucceeded(const QString &savedContent)
{
    m_saveInProgress = false;
    const bool currentContentSaved = toPlainText() == savedContent;
    if (currentContentSaved)
        m_editor->document()->setModified(false);
    m_message->setText(currentContentSaved
                           ? tr("已保存")
                           : tr("上一版本已保存，当前仍有未保存修改"));
    updateWindowTitle();
    updateSaveAction();

    if (m_closeAfterSave) {
        m_closeAfterSave = false;
        // 本地保存会在 closeEvent 的调用栈内同步完成，延迟关闭避免重入。
        if (currentContentSaved)
            QTimer::singleShot(0, this, [this] { close(); });
    }
}

void TextEditor::notifySaveFailed(const QString &message)
{
    m_saveInProgress = false;
    m_closeAfterSave = false;
    m_message->setText(tr("保存失败：%1").arg(message));
    updateWindowTitle();
    updateSaveAction();
}

void TextEditor::showSearchBar(bool replace)
{
    m_searchBar->setVisible(true);
    m_replaceRow->setVisible(replace);

    const QString selected = m_editor->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)
        && !selected.contains(QLatin1Char('\n')))
        m_findEdit->setText(selected);
    updateSearchResults(!m_findEdit->text().isEmpty());
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void TextEditor::hideSearchBar()
{
    m_searchBar->setVisible(false);
    m_editor->clearSearchHighlights();
    m_editor->setFocus();
}

void TextEditor::updateSearchResults(bool selectMatch)
{
    if (!m_searchBar->isVisible())
        return;
    const QString pattern = m_findEdit->text();
    if (pattern.isEmpty()) {
        m_editor->clearSearchHighlights();
        m_matchStatus->clear();
        m_findEdit->setToolTip(QString());
        m_findEdit->setStyleSheet(QString());
    } else if (m_regexCheck->isChecked()) {
        QRegularExpression expression(pattern);
        if (!m_caseCheck->isChecked())
            expression.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid()) {
            m_editor->clearSearchHighlights();
            m_matchStatus->setText(tr("正则无效"));
            m_findNextButton->setEnabled(false);
            m_findPrevButton->setEnabled(false);
            m_replaceButton->setEnabled(false);
            m_replaceAllButton->setEnabled(false);
            m_findEdit->setToolTip(expression.errorString());
            m_findEdit->setStyleSheet(
                QStringLiteral("QLineEdit { border: 1px solid #ff5555; }"));
            return;
        }
    }

    const int count = pattern.isEmpty() ? 0 : m_editor->highlightMatches(
        pattern, m_regexCheck->isChecked(), m_caseCheck->isChecked(),
        m_wordCheck->isChecked());
    m_findEdit->setStyleSheet(QString());
    m_findEdit->setToolTip(QString());
    const bool hasMatches = count > 0;
    m_findNextButton->setEnabled(hasMatches);
    m_findPrevButton->setEnabled(hasMatches);
    m_replaceButton->setEnabled(hasMatches);
    m_replaceAllButton->setEnabled(hasMatches);

    if (selectMatch && hasMatches) {
        m_editor->findText(pattern, m_regexCheck->isChecked(),
                           m_caseCheck->isChecked(), false,
                           m_wordCheck->isChecked());
    }
    updateSearchPosition();
}

void TextEditor::updateSearchPosition()
{
    const int count = m_editor->searchMatchCount();
    if (count == 0) {
        m_matchStatus->setText(m_findEdit->text().isEmpty() ? QString() : tr("无结果"));
        return;
    }
    const int current = m_editor->currentSearchMatchIndex();
    m_matchStatus->setText(current > 0 ? tr("%1 / %2").arg(current).arg(count)
                                      : tr("%1 处").arg(count));
}

void TextEditor::onFindNext()
{
    if (m_findEdit->text().isEmpty())
        return;
    const bool found = m_editor->findText(
        m_findEdit->text(), m_regexCheck->isChecked(), m_caseCheck->isChecked(),
        false, m_wordCheck->isChecked());
    if (!found)
        m_matchStatus->setText(tr("无结果"));
    else
        updateSearchPosition();
}

void TextEditor::onFindPrev()
{
    if (m_findEdit->text().isEmpty())
        return;
    const bool found = m_editor->findText(
        m_findEdit->text(), m_regexCheck->isChecked(), m_caseCheck->isChecked(),
        true, m_wordCheck->isChecked());
    if (!found)
        m_matchStatus->setText(tr("无结果"));
    else
        updateSearchPosition();
}

void TextEditor::onReplace()
{
    if (m_findEdit->text().isEmpty())
        return;
    m_editor->replaceText(m_findEdit->text(), m_replaceEdit->text(),
                          m_regexCheck->isChecked(), m_caseCheck->isChecked(),
                          m_wordCheck->isChecked());
    updateSearchResults(false);
}

void TextEditor::onReplaceAll()
{
    if (m_findEdit->text().isEmpty())
        return;
    const int count = m_editor->replaceAll(
        m_findEdit->text(), m_replaceEdit->text(), m_regexCheck->isChecked(),
        m_caseCheck->isChecked(), m_wordCheck->isChecked());
    m_message->setText(tr("已替换 %1 处").arg(count));
    updateSearchResults(false);
}

void TextEditor::showGoToLineDialog()
{
    const int currentLine = m_editor->textCursor().blockNumber() + 1;
    bool ok = false;
    const int line = QInputDialog::getInt(
        this, tr("跳转到行"), tr("行号（1-%1）：").arg(m_editor->blockCount()),
        currentLine, 1, m_editor->blockCount(), 1, &ok);
    if (ok && m_editor->goToLine(line)) {
        m_editor->setFocus();
        updateStatusBar();
    }
}

void TextEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_searchBar->isVisible()) {
        hideSearchBar();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TextEditor::closeEvent(QCloseEvent *event)
{
    if (m_saveInProgress) {
        // 允许用户在断线等异常情况下放弃等待，避免永远关不掉窗口；QPointer
        // 会阻止迟到的远程回调访问已销毁编辑器。
        const auto ret = QMessageBox::question(
            this, tr("保存进行中"),
            tr("保存尚未完成。确定放弃等待并关闭编辑器吗？"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (ret == QMessageBox::Discard) {
            m_closeAfterSave = false;
            event->accept();
        } else {
            m_message->setText(tr("正在保存，请稍候…"));
            event->ignore();
        }
        return;
    }

    if (isModified()) {
        const auto ret = QMessageBox::question(
            this, tr("未保存的修改"),
            tr("文档已修改，关闭前是否保存？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (ret == QMessageBox::Save) {
            m_closeAfterSave = true;
            onSave();
            event->ignore();
            return;
        }
        if (ret == QMessageBox::Cancel) {
            m_closeAfterSave = false;
            event->ignore();
            return;
        }
    }
    m_closeAfterSave = false;
    event->accept();
}

} // namespace cubeshell
