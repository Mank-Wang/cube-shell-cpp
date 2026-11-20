#include "CodeEditor.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QTextBlock>

namespace cubeshell {
namespace {

constexpr int kIndentWidth = 4;
constexpr int kMaxPaintedSearchMatches = 2000;

QTextDocument::FindFlags findFlags(bool caseSensitive, bool backward, bool wholeWord)
{
    QTextDocument::FindFlags flags;
    if (caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (backward)
        flags |= QTextDocument::FindBackward;
    if (wholeWord)
        flags |= QTextDocument::FindWholeWords;
    return flags;
}

QRegularExpression searchExpression(const QString &pattern, bool caseSensitive)
{
    QRegularExpression expression(pattern);
    if (!caseSensitive)
        expression.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    return expression;
}

// 判断当前选区是否仍是一次完整匹配，避免“替换”按钮误删用户自行选中的文本。
bool selectionMatches(const QTextCursor &cursor, const QString &text, bool regex,
                      bool caseSensitive, bool wholeWord)
{
    if (!cursor.hasSelection())
        return false;
    if (wholeWord) {
        const auto isWordCharacter = [](const QChar &ch) {
            return ch.isLetterOrNumber() || ch == QLatin1Char('_');
        };
        const QTextDocument *document = cursor.document();
        const int start = cursor.selectionStart();
        const int end = cursor.selectionEnd();
        if ((start > 0 && isWordCharacter(document->characterAt(start - 1)))
            || (end < document->characterCount() - 1
                && isWordCharacter(document->characterAt(end))))
            return false;
    }
    const QString selected = cursor.selectedText();
    if (!regex) {
        const Qt::CaseSensitivity sensitivity = caseSensitive
            ? Qt::CaseSensitive : Qt::CaseInsensitive;
        return selected.compare(text, sensitivity) == 0;
    }

    const QRegularExpression expression = searchExpression(text, caseSensitive);
    if (!expression.isValid())
        return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    constexpr auto anchorOption = QRegularExpression::AnchorAtOffsetMatchOption;
#else
    constexpr auto anchorOption = QRegularExpression::AnchoredMatchOption;
#endif
    const QRegularExpressionMatch match = expression.match(
        selected, 0, QRegularExpression::NormalMatch, anchorOption);
    return match.hasMatch() && match.capturedStart() == 0
        && match.capturedLength() == selected.size();
}

// QString 的正则替换负责展开 \1、\2 等捕获组引用；纯文本替换原样插入。
QString replacementFor(const QTextCursor &match, const QString &pattern,
                       const QString &replacement, bool regex, bool caseSensitive)
{
    if (!regex)
        return replacement;
    QString result = match.selectedText();
    result.replace(searchExpression(pattern, caseSensitive), replacement);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// LineNumberArea
// ---------------------------------------------------------------------------

LineNumberArea::LineNumberArea(CodeEditor *editor)
    : QWidget(editor)
    , m_editor(editor)
{
}

QSize LineNumberArea::sizeHint() const
{
    return QSize(m_editor->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent *event)
{
    m_editor->lineNumberAreaPaintEvent(event);
}

// ---------------------------------------------------------------------------
// Highlighter
// ---------------------------------------------------------------------------

Highlighter::Highlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    QTextCharFormat keywordFormat;
    keywordFormat.setForeground(QColor(QStringLiteral("#ff79c6")));
    keywordFormat.setFontWeight(QFont::Bold);
    const QStringList keywords = {
        QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("class"),
        QStringLiteral("const"), QStringLiteral("def"), QStringLiteral("delete"),
        QStringLiteral("elif"), QStringLiteral("else"), QStringLiteral("enum"),
        QStringLiteral("except"), QStringLiteral("explicit"), QStringLiteral("export"),
        QStringLiteral("extends"), QStringLiteral("false"), QStringLiteral("finally"),
        QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("function"),
        QStringLiteral("if"), QStringLiteral("implements"), QStringLiteral("import"),
        QStringLiteral("in"), QStringLiteral("instanceof"), QStringLiteral("interface"),
        QStringLiteral("let"), QStringLiteral("new"), QStringLiteral("null"),
        QStringLiteral("package"), QStringLiteral("private"), QStringLiteral("protected"),
        QStringLiteral("public"), QStringLiteral("return"), QStringLiteral("static"),
        QStringLiteral("super"), QStringLiteral("switch"), QStringLiteral("this"),
        QStringLiteral("throw"), QStringLiteral("true"), QStringLiteral("try"),
        QStringLiteral("typeof"), QStringLiteral("var"), QStringLiteral("void"),
        QStringLiteral("while"), QStringLiteral("with"), QStringLiteral("yield"),
    };
    // 所有关键字合并为一次扫描，避免每一行重复执行数十个正则表达式。
    m_rules.append({QRegularExpression(
                        QStringLiteral("\\b(?:%1)\\b").arg(keywords.join(QLatin1Char('|')))),
                    keywordFormat});

    // 支持转义字符且不回溯跨越多个字符串，修正旧的贪婪 ".*" 匹配。
    QTextCharFormat stringFormat;
    stringFormat.setForeground(QColor(QStringLiteral("#f1fa8c")));
    m_rules.append({QRegularExpression(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"")),
                    stringFormat});
    m_rules.append({QRegularExpression(QStringLiteral("'(?:\\\\.|[^'\\\\])*'")),
                    stringFormat});

    QTextCharFormat functionFormat;
    functionFormat.setForeground(QColor(QStringLiteral("#50fa7b")));
    m_rules.append({QRegularExpression(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()")),
                    functionFormat});

    QTextCharFormat selfFormat;
    selfFormat.setForeground(QColor(QStringLiteral("#ff5555")));
    selfFormat.setFontWeight(QFont::Bold);
    m_rules.append({QRegularExpression(QStringLiteral("\\bself\\b")), selfFormat});

    QTextCharFormat numberFormat;
    numberFormat.setForeground(QColor(QStringLiteral("#bd93f9")));
    m_rules.append({QRegularExpression(
                        QStringLiteral("\\b(?:0[xX][0-9a-fA-F]+|\\d+(?:\\.\\d+)?)\\b")),
                    numberFormat});

    // 注释最后应用，覆盖注释内部的关键字、数字等格式。
    m_commentFormat.setForeground(QColor(QStringLiteral("#6272a4")));
}

void Highlighter::highlightBlock(const QString &text)
{
    for (const Rule &rule : m_rules)
        applyRule(text, rule.pattern, rule.format);

    static const QRegularExpression hashComment(QStringLiteral("#[^\n]*"));
    static const QRegularExpression slashComment(QStringLiteral("//[^\n]*"));
    applyRule(text, hashComment, m_commentFormat);
    applyRule(text, slashComment, m_commentFormat);
}

void Highlighter::applyRule(const QString &text, const QRegularExpression &expr,
                            const QTextCharFormat &format)
{
    QRegularExpressionMatch match = expr.match(text);
    while (match.hasMatch()) {
        const int index = int(match.capturedStart());
        const int length = int(match.capturedLength());
        if (length <= 0)
            break;
        setFormat(index, length, format);
        match = expr.match(text, index + length);
    }
}

// ---------------------------------------------------------------------------
// CodeEditor
// ---------------------------------------------------------------------------

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("codeEditor"));
    m_lineNumberArea = new LineNumberArea(this);
    m_highlighter = new Highlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

    // 使用平台等宽字体而不是固定 Consolas，macOS、Linux 和鸿蒙都能获得合适字形。
    QFont editorFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    editorFont.setFixedPitch(true);
#ifdef CUBESHELL_PLATFORM_OHOS
    // 鸿蒙的 pointSize 不会按屏幕密度缩放，与文件树共用项目的 DPI 适配约定。
    editorFont.setPixelSize(qRound(14 * 96.0 / 72.0 * devicePixelRatioF()));
#else
    editorFont.setPointSize(14);
#endif
    setFont(editorFont);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * kIndentWidth);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setCursorWidth(2);

    m_bracketFormat.setForeground(QColor(QStringLiteral("#f8f8f2")));
    m_bracketFormat.setBackground(QColor(QStringLiteral("#6272a4")));
    m_searchFormat.setBackground(QColor(241, 250, 140, 65));

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int maxNum = qMax(1, blockCount());
    while (maxNum >= 10) {
        maxNum /= 10;
        ++digits;
    }
    // 两侧留白让行号与正文保持视觉距离，同时给当前行高亮留出空间。
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection currentLine;
        currentLine.format.setBackground(QColor(68, 71, 90, 50));
        currentLine.format.setProperty(QTextFormat::FullWidthSelection, true);
        currentLine.cursor = textCursor();
        currentLine.cursor.clearSelection();
        selections.append(currentLine);
    }
    matchBrackets(selections);
    selections.append(m_searchSelections);
    setExtraSelections(selections);
}

void CodeEditor::matchBrackets(QList<QTextEdit::ExtraSelection> &selections)
{
    const QString text = document()->toPlainText();
    const int cursorPos = textCursor().position();
    const QString brackets = QStringLiteral("([{)]}");

    int anchor = -1;
    if (cursorPos > 0 && brackets.contains(text.at(cursorPos - 1)))
        anchor = cursorPos - 1;
    else if (cursorPos < text.size() && brackets.contains(text.at(cursorPos)))
        anchor = cursorPos;
    if (anchor < 0)
        return;

    const QChar bracket = text.at(anchor);
    const bool opening = QStringLiteral("([{").contains(bracket);
    const QChar expected = opening
        ? (bracket == QLatin1Char('(') ? QLatin1Char(')')
                                       : bracket == QLatin1Char('[') ? QLatin1Char(']')
                                                                    : QLatin1Char('}'))
        : (bracket == QLatin1Char(')') ? QLatin1Char('(')
                                       : bracket == QLatin1Char(']') ? QLatin1Char('[')
                                                                    : QLatin1Char('{'));
    int depth = 1;
    int matching = -1;

    // 从括号自身之后开始扫描；允许匹配文档第 0 个字符，也避免负坐标选区。
    for (int pos = anchor + (opening ? 1 : -1);
         opening ? pos < text.size() : pos >= 0;
         pos += opening ? 1 : -1) {
        const QChar current = text.at(pos);
        if (current == bracket) {
            ++depth;
        } else if (current == expected && --depth == 0) {
            matching = pos;
            break;
        }
    }

    createBracketSelection(anchor, selections);
    if (matching >= 0)
        createBracketSelection(matching, selections);
}

void CodeEditor::createBracketSelection(
    int pos, QList<QTextEdit::ExtraSelection> &selections) const
{
    if (pos < 0 || pos >= document()->characterCount() - 1)
        return;
    QTextEdit::ExtraSelection selection;
    selection.format = m_bracketFormat;
    selection.cursor = textCursor();
    selection.cursor.setPosition(pos);
    selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    selections.append(selection);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), QColor(QStringLiteral("#282a36")));
    painter.setFont(font());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = int(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + int(blockBoundingRect(block).height());
    const int currentLine = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(blockNumber == currentLine
                               ? QColor(QStringLiteral("#f8f8f2"))
                               : QColor(QStringLiteral("#6272a4")));
            painter.drawText(0, top, m_lineNumberArea->width() - 7,
                             fontMetrics().height(), Qt::AlignRight,
                             QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + int(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

bool CodeEditor::findText(const QString &text, bool regex, bool caseSensitive,
                          bool backward, bool wholeWord)
{
    if (text.isEmpty())
        return false;
    const QTextDocument::FindFlags flags = findFlags(caseSensitive, backward, wholeWord);
    QRegularExpression expression;
    if (regex) {
        expression = searchExpression(text, caseSensitive);
        if (!expression.isValid())
            return false;
    }

    QTextCursor found = regex ? document()->find(expression, textCursor(), flags)
                              : document()->find(text, textCursor(), flags);
    if (found.isNull()) {
        QTextCursor from = textCursor();
        from.clearSelection();
        from.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        found = regex ? document()->find(expression, from, flags)
                      : document()->find(text, from, flags);
    }

    if (found.isNull())
        return false;
    setTextCursor(found);
    return true;
}

bool CodeEditor::replaceText(const QString &text, const QString &newText,
                             bool regex, bool caseSensitive, bool wholeWord)
{
    if (text.isEmpty())
        return false;
    QTextCursor cursor = textCursor();
    if (!selectionMatches(cursor, text, regex, caseSensitive, wholeWord))
        return findText(text, regex, caseSensitive, false, wholeWord);

    cursor.insertText(replacementFor(cursor, text, newText, regex, caseSensitive));
    setTextCursor(cursor);
    // 与现代编辑器一致：替换当前项后自动选中下一项，便于连续确认替换。
    findText(text, regex, caseSensitive, false, wholeWord);
    return true;
}

int CodeEditor::replaceAll(const QString &text, const QString &newText,
                           bool regex, bool caseSensitive, bool wholeWord)
{
    if (text.isEmpty())
        return 0;
    const QTextDocument::FindFlags flags = findFlags(caseSensitive, false, wholeWord);
    QRegularExpression expression;
    if (regex) {
        expression = searchExpression(text, caseSensitive);
        if (!expression.isValid())
            return 0;
    }

    int count = 0;
    QTextCursor editCursor(document());
    editCursor.movePosition(QTextCursor::Start);
    editCursor.beginEditBlock();
    while (count < 100000) { // 防止异常零宽正则导致无限循环。
        QTextCursor found = regex ? document()->find(expression, editCursor, flags)
                                  : document()->find(text, editCursor, flags);
        if (found.isNull())
            break;

        found.insertText(replacementFor(found, text, newText, regex, caseSensitive));
        ++count;
        editCursor = found;
        if (!found.hasSelection()) {
            // 零宽匹配插入后显式前进一个字符，确保下一次查找严格向前推进。
            const int next = found.position() + 1;
            if (next >= document()->characterCount())
                break;
            editCursor.setPosition(next);
        }
    }
    editCursor.endEditBlock();
    setTextCursor(editCursor);
    return count;
}

int CodeEditor::highlightMatches(const QString &text, bool regex,
                                 bool caseSensitive, bool wholeWord)
{
    m_searchMatches.clear();
    m_searchSelections.clear();
    if (text.isEmpty()) {
        highlightCurrentLine();
        return 0;
    }

    const QTextDocument::FindFlags flags = findFlags(caseSensitive, false, wholeWord);
    QRegularExpression expression;
    if (regex) {
        expression = searchExpression(text, caseSensitive);
        if (!expression.isValid())
            return -1;
    }

    QTextCursor from(document());
    from.movePosition(QTextCursor::Start);
    while (true) {
        QTextCursor found = regex ? document()->find(expression, from, flags)
                                  : document()->find(text, from, flags);
        if (found.isNull())
            break;
        m_searchMatches.append(found);

        // 匹配计数与绘制分离：超大日志仍能显示真实总数，但不会创建海量绘制项。
        if (m_searchSelections.size() < kMaxPaintedSearchMatches) {
            QTextEdit::ExtraSelection selection;
            selection.format = m_searchFormat;
            selection.cursor = found;
            m_searchSelections.append(selection);
        }

        const bool zeroWidth = !found.hasSelection();
        const int next = found.selectionEnd() + (zeroWidth ? 1 : 0);
        if (next >= document()->characterCount() - (zeroWidth ? 0 : 1))
            break;
        from = found;
        from.setPosition(next);
    }

    highlightCurrentLine();
    return m_searchMatches.size();
}

void CodeEditor::clearSearchHighlights()
{
    m_searchMatches.clear();
    m_searchSelections.clear();
    highlightCurrentLine();
}

int CodeEditor::currentSearchMatchIndex() const
{
    const QTextCursor current = textCursor();
    if (!current.hasSelection())
        return 0;
    for (int i = 0; i < m_searchMatches.size(); ++i) {
        const QTextCursor &match = m_searchMatches.at(i);
        if (match.selectionStart() == current.selectionStart()
            && match.selectionEnd() == current.selectionEnd())
            return i + 1;
    }
    return 0;
}

bool CodeEditor::goToLine(int line)
{
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (line <= 0 || !block.isValid())
        return false;
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
    return true;
}

void CodeEditor::indentSelectedLines(bool indent)
{
    QTextCursor original = textCursor();
    int start = original.selectionStart();
    int end = original.selectionEnd();
    // 选区恰好结束于行首时，最后一行并未被用户真正选中，不应额外缩进。
    if (original.hasSelection() && end > start
        && document()->findBlock(end).position() == end)
        --end;

    const int firstBlock = document()->findBlock(start).blockNumber();
    const int lastBlock = document()->findBlock(end).blockNumber();
    QTextBlock block = document()->findBlockByNumber(firstBlock);
    QTextCursor edit(document());
    edit.beginEditBlock();
    while (block.isValid() && block.blockNumber() <= lastBlock) {
        edit.setPosition(block.position());
        if (indent) {
            edit.insertText(QString(kIndentWidth, QLatin1Char(' ')));
        } else if (block.text().startsWith(QLatin1Char('\t'))) {
            edit.deleteChar();
        } else {
            // 每行最多去掉一个缩进层级，避免吃掉正文中的其他空格。
            const int spaces = qMin(kIndentWidth, [&block] {
                int count = 0;
                while (count < block.text().size()
                       && block.text().at(count) == QLatin1Char(' '))
                    ++count;
                return count;
            }());
            for (int i = 0; i < spaces; ++i)
                edit.deleteChar();
        }
        block = block.next();
    }
    edit.endEditBlock();
    setTextCursor(original);
}

void CodeEditor::insertAutoIndentedNewline()
{
    QTextCursor cursor = textCursor();
    QTextCursor start = cursor;
    start.setPosition(cursor.selectionStart());
    const QString before = start.block().text().left(start.positionInBlock());
    const QString after = cursor.block().text().mid(cursor.positionInBlock());

    QString indentation;
    for (const QChar ch : before) {
        if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t'))
            break;
        indentation.append(ch);
    }

    auto lastNonSpace = [](const QString &value) {
        for (int i = value.size() - 1; i >= 0; --i) {
            if (!value.at(i).isSpace())
                return value.at(i);
        }
        return QChar();
    };
    auto firstNonSpace = [](const QString &value) {
        for (const QChar ch : value) {
            if (!ch.isSpace())
                return ch;
        }
        return QChar();
    };

    const QChar previous = lastNonSpace(before);
    const QChar next = firstNonSpace(after);
    const bool opensBlock = previous == QLatin1Char('{')
        || previous == QLatin1Char('[') || previous == QLatin1Char('(')
        || previous == QLatin1Char(':');
    const QString innerIndent = indentation
        + (opensBlock ? QString(kIndentWidth, QLatin1Char(' ')) : QString());

    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("\n") + innerIndent);
    const bool closesBlock = (previous == QLatin1Char('{') && next == QLatin1Char('}'))
        || (previous == QLatin1Char('[') && next == QLatin1Char(']'))
        || (previous == QLatin1Char('(') && next == QLatin1Char(')'));
    if (closesBlock) {
        // 在 {}、[]、() 中间回车时展开成两行，并把光标留在内部缩进位置。
        cursor.insertText(QStringLiteral("\n") + indentation);
        cursor.setPosition(cursor.position() - indentation.size() - 1);
    }
    cursor.endEditBlock();
    setTextCursor(cursor);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    // Kube/Nginx 等窗口复用本控件做只读预览，自定义编辑按键必须尊重只读状态。
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    const bool plainKey = !event->modifiers().testFlag(Qt::ControlModifier)
        && !event->modifiers().testFlag(Qt::AltModifier)
        && !event->modifiers().testFlag(Qt::MetaModifier);
    if (plainKey && event->key() == Qt::Key_Tab) {
        if (textCursor().hasSelection())
            indentSelectedLines(true);
        else
            insertPlainText(QString(kIndentWidth, QLatin1Char(' ')));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Backtab) {
        indentSelectedLines(false);
        event->accept();
        return;
    }
    if (plainKey && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        insertAutoIndentedNewline();
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

} // namespace cubeshell
