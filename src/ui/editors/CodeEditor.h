#pragma once

// CodeEditor.h — 带行号、语法高亮、括号匹配与查找/替换的代码编辑器。
// 对应Python: ui/code_editor.py::CodeEditor / Highlighter / LineNumberArea

#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;

namespace cubeshell {

class CodeEditor;

// 对应Python: ui/code_editor.py::LineNumberArea
class LineNumberArea : public QWidget {
    Q_OBJECT
public:
    explicit LineNumberArea(CodeEditor *editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodeEditor *m_editor;
};

// 简单关键字 + 正则规则高亮（Dracula 配色）。
// 对应Python: ui/code_editor.py::Highlighter
class Highlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit Highlighter(QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    void applyRule(const QString &text, const QRegularExpression &expr,
                   const QTextCharFormat &format);

    QVector<Rule> m_rules;
    QTextCharFormat m_commentFormat;
};

// 对应Python: ui/code_editor.py::CodeEditor
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    // 查找支持正则、大小写、全词和反向查找；到文档边界后自动循环。
    bool findText(const QString &text, bool regex = false,
                  bool caseSensitive = false, bool backward = false,
                  bool wholeWord = false);
    // 当前选区确为查找结果时才替换，随后定位下一处；否则只定位第一处。
    bool replaceText(const QString &text, const QString &newText,
                     bool regex = false, bool caseSensitive = false,
                     bool wholeWord = false);
    // 全文替换并归入同一次撤销操作，返回实际替换次数。
    int replaceAll(const QString &text, const QString &newText,
                   bool regex = false, bool caseSensitive = false,
                   bool wholeWord = false);

    // 统计并高亮全文匹配项。返回匹配数，正则无效时返回 -1。
    // 最多绘制 2000 处高亮，但匹配计数始终覆盖全文，避免大文档卡顿。
    int highlightMatches(const QString &text, bool regex = false,
                         bool caseSensitive = false, bool wholeWord = false);
    void clearSearchHighlights();
    int searchMatchCount() const { return m_searchMatches.size(); }
    // 当前光标选区在全部匹配项中的序号（从 1 开始；不在匹配项上返回 0）。
    int currentSearchMatchIndex() const;

    // 跳转到指定行并滚动到可视区域；行号从 1 开始。
    bool goToLine(int line);
    bool wordWrapEnabled() const { return lineWrapMode() != QPlainTextEdit::NoWrap; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

private:
    void matchBrackets(QList<QTextEdit::ExtraSelection> &selections);
    void createBracketSelection(int pos,
                                QList<QTextEdit::ExtraSelection> &selections) const;
    void indentSelectedLines(bool indent);
    void insertAutoIndentedNewline();

    LineNumberArea *m_lineNumberArea = nullptr;
    Highlighter *m_highlighter = nullptr;
    QTextCharFormat m_bracketFormat;
    QTextCharFormat m_searchFormat;
    QList<QTextCursor> m_searchMatches;
    QList<QTextEdit::ExtraSelection> m_searchSelections;
};

} // namespace cubeshell
