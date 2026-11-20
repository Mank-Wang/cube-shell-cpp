#pragma once

// TextEditor.h — 独立文件编辑窗口：编辑工具栏、双行查找替换、状态栏和可靠保存。
// 文件 I/O 仍由本地/SFTP 浏览器负责，编辑器只管理内容与交互状态。
// 对应Python: ui/text_editor.py::Ui_MainWindow + cube-shell.py 中远程文件编辑逻辑

#include <QWidget>

class QAction;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace cubeshell {

class CodeEditor;

class TextEditor : public QWidget {
    Q_OBJECT
public:
    explicit TextEditor(QWidget *parent = nullptr);

    void setPlainText(const QString &text);
    QString toPlainText() const;

    // 编辑目标显示名（窗口标题，如本地路径或远程文件路径）。
    void setFileLabel(const QString &label);

    CodeEditor *editor() const { return m_editor; }
    bool isModified() const;
    bool isSaving() const { return m_saveInProgress; }

    // 保存持有者必须在写回结束后调用其中一个方法。
    // savedContent 用于识别“保存途中又继续编辑”的情况：此时后台内容已经落盘，
    // 但当前新修改仍保持未保存状态，不能被旧请求的完成回调误清除。
    void notifySaveSucceeded(const QString &savedContent);
    void notifySaveFailed(const QString &message);

signals:
    // 用户按 Ctrl+S / 点保存时发射，由持有者负责实际写回（本地或 SFTP）。
    // 只有收到 notifySaveSucceeded 后，编辑器才会将对应内容标记为已保存。
    void saveRequested(const QString &content);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSave();
    void onFindNext();
    void onFindPrev();
    void onReplace();
    void onReplaceAll();
    void showSearchBar(bool replace);
    void hideSearchBar();
    void updateSearchResults(bool selectMatch);
    void updateSearchPosition();
    void updateWindowTitle();
    void updateSaveAction();
    void updateStatusBar();
    void showGoToLineDialog();

private:
    CodeEditor *m_editor = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_wordWrapAction = nullptr;
    QWidget *m_searchBar = nullptr;
    QWidget *m_replaceRow = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QCheckBox *m_regexCheck = nullptr;
    QCheckBox *m_caseCheck = nullptr;
    QCheckBox *m_wordCheck = nullptr;
    QPushButton *m_findNextButton = nullptr;
    QPushButton *m_findPrevButton = nullptr;
    QPushButton *m_replaceButton = nullptr;
    QPushButton *m_replaceAllButton = nullptr;
    QLabel *m_matchStatus = nullptr;
    QLabel *m_message = nullptr;
    QLabel *m_cursorStatus = nullptr;
    QLabel *m_documentStatus = nullptr;
    QString m_fileLabel;
    bool m_saveInProgress = false;
    bool m_closeAfterSave = false;
};

} // namespace cubeshell
