#include "sftp_browser_widget.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressBar>
#include <QResizeEvent>
#include <QShortcut>
#include <QSizePolicy>
#include <QStyle>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "config/GlobalState.h"
#include "ssh/SshClient.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SftpUploaderCore.h"
#include "util/FileUtil.h"
#include "dialogs/CompressDialog.h"
#include "editors/TextEditor.h"
#include "file_browser_common.h"
#include "file_icons.h"
#include "pane_badge.h"

namespace cubeshell {

// 析构时对每个存活 worker 的最长等待；超时则泄漏其引用对象而非无限阻塞 UI。
static constexpr int kWorkerJoinTimeoutMs = 5000;

// SFTP 目录节点的请求序号。加载状态与占位角色由 file_browser_common.h
// 统一提供，本地树和 SFTP 树使用同一套状态定义。
constexpr int kDirLoadSeqRole = Qt::UserRole + 21;

// 条目角色布局（kPathRole/kIsDirRole/kIsUpRole/kModeRole/kSymlinkTargetRole
// 及排序用的 kSizeRole/kMtimeRole）统一由 file_browser_common.h 提供，
// 与本地文件面板共用同一套，便于 sortFileTree/applyFileFilter 直接读。

// 脚本扩展名 → 解释器（没有可执行位时补在命令前）。
// 这张表同时就是右键菜单「执行脚本」的显示白名单：命中即为脚本文件，一处维护。
// 只收 POSIX 侧脚本类型：远端 SFTP 路径按 '/' 拼接，Windows 远端的 .bat/.ps1
// 连路径形态都不一样，硬塞进来只会拼出跑不了的命令。
static const QHash<QString, QString> &scriptInterpreters()
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("sh"), QStringLiteral("bash")},
        {QStringLiteral("bash"), QStringLiteral("bash")},
        {QStringLiteral("zsh"), QStringLiteral("zsh")},
        {QStringLiteral("ksh"), QStringLiteral("ksh")},
        {QStringLiteral("fish"), QStringLiteral("fish")},
        {QStringLiteral("py"), QStringLiteral("python3")},
        {QStringLiteral("pl"), QStringLiteral("perl")},
        {QStringLiteral("rb"), QStringLiteral("ruby")},
        {QStringLiteral("lua"), QStringLiteral("lua")},
        {QStringLiteral("php"), QStringLiteral("php")},
        {QStringLiteral("js"), QStringLiteral("node")},
    };
    return map;
}

// 文件名 → 扩展名（小写，不含点）；无扩展名或以点结尾返回空串。
// 隐藏文件的前导点不算扩展名分隔符（".sh" 是个没有扩展名的文件，不是脚本）。
static QString scriptExtension(const QString &name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return QString();
    return name.mid(dot + 1).toLower();
}

bool SftpBrowserWidget::looksLikeScript(const QString &name, bool isDir)
{
    if (isDir || name.isEmpty())
        return false;
    return scriptInterpreters().contains(scriptExtension(name));
}

QString SftpBrowserWidget::scriptRunCommand(const QString &path, quint32 perm, bool isSymlink)
{
    const QString quoted = FileUtil::shellQuote(path);
    // 符号链接不走"直跑"：链接自身的权限位恒为 rwxrwxrwx，说明不了目标能不能
    // 执行，补上解释器才不会在目标没有 x 位时撞 "Permission denied"。
    if (!isSymlink && (perm & 0111) != 0)
        return quoted;
    const QString interp =
        scriptInterpreters().value(scriptExtension(path.section(QLatin1Char('/'), -1)));
    // 白名单外理论到不了这里（菜单项不显示），兜底直跑而不是拼出半条命令。
    return interp.isEmpty() ? quoted : (interp + QLatin1Char(' ') + quoted);
}

// 文件大小人类可读格式。对应Python: function/util.py::format_file_size
static QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 字节").arg(bytes);
    if (bytes < 1024LL * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 3);
}

// ls 风格权限串（类型位 + rwx 九位），优先取 readdir longname 首列（与远端 ls 完全一致）。
// 对应Python: handle_file_tree_updated 里 item.setText(3, n[0])
static QString permissionText(const SftpFileInfo &e)
{
    const QStringList parts = e.longname.simplified().split(QLatin1Char(' '));
    if (!parts.isEmpty() && parts.first().size() >= 10)
        return parts.first();
    QString s(e.isDirectory() ? QLatin1Char('d')
                              : e.isSymlink() ? QLatin1Char('l') : QLatin1Char('-'));
    static const char bits[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        s += (e.mode & (1u << (8 - i))) ? QLatin1Char(bits[i]) : QLatin1Char('-');
    return s;
}

// 所有者/组列：longname 第 3 列为属主名，取不到时退回 uid 数字。
// 对应Python: handle_file_tree_updated 里 item.setText(4, n[3])（仅属主，不含组）
static QString ownerText(const SftpFileInfo &e)
{
    const QStringList parts = e.longname.simplified().split(QLatin1Char(' '));
    if (parts.size() >= 3)
        return parts.at(2);
    return QString::number(e.uid);
}

SftpBrowserWidget::SftpBrowserWidget(QWidget *parent)
    : QWidget(parent)
{
    // 顶部路径栏：左侧徽章（多分屏时显示序号）+ 路径编辑框 + 右侧筛选开关。
    // 对应Python: add_line_edit(pwd) 在文件树顶部展示当前目录
    m_paneBadge = createPaneBadge(this);
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setText(m_cwd);
    m_pathEdit->setPlaceholderText(tr("正在加载用户目录…"));

    // 筛选开关：点击在路径栏下方弹出检索框，再次点击（或检索框里 Esc）收起。
    m_filterBtn = new QToolButton(this);
    m_filterBtn->setIcon(fileFilterIcon(palette()));
    m_filterBtn->setToolTip(tr("筛选已加载的文件和目录（Ctrl+F）"));
    m_filterBtn->setCheckable(true);
    m_filterBtn->setAutoRaise(true);

    auto *pathBar = new QWidget(this);
    auto *pathLayout = new QHBoxLayout(pathBar);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(4);
    pathLayout->addWidget(m_paneBadge);
    pathLayout->addWidget(m_pathEdit, 1);
    pathLayout->addWidget(m_filterBtn);

    // 检索框行（默认隐藏）：输入即筛选所有已加载层级，Enter 定位第一个匹配项。
    m_filterBar = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(m_filterBar);
    filterLayout->setContentsMargins(0, 2, 0, 2);
    filterLayout->setSpacing(4);
    m_filterEdit = new QLineEdit(m_filterBar);
    m_filterEdit->setPlaceholderText(tr("筛选已加载的文件和目录，Enter 定位，Esc 关闭"));
    m_filterEdit->setClearButtonEnabled(true);
    filterLayout->addWidget(m_filterEdit, 1);
    m_filterBar->setVisible(false);

    // 多层级目录树：目录节点首次展开时才请求子项，其他已展开分支保持可见。
    // 双击由 onItemDoubleClicked 自己切换展开态，避免 Qt 默认行为重复切换。
    m_tree = new QTreeWidget(this);
    // 目录确定前禁止右键写操作和拖放，路径栏仍可用于手动导航/重试。
    m_tree->setEnabled(false);
    m_filterBtn->setEnabled(false);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("文件名"), tr("文件大小"), tr("修改日期"),
                             tr("权限"), tr("所有者/组")});
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(16);
    m_tree->setExpandsOnDoubleClick(false);
    // 构造时读取全局设置；运行中由 MainWindow 接收设置信号并调 setFontSize。
    setFontSize(GlobalState::instance().sftpListFontSize());
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QTreeWidget::ExtendedSelection);
    // 列宽可拖动、总宽超出面板时出现水平滚动条。
    // 对应Python: QTreeWidget 默认 Interactive 表头 + header.setSectionsMovable(True)
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setSectionsMovable(true);
    m_tree->setColumnWidth(0, 200);
    m_tree->setColumnWidth(1, 90);
    m_tree->setColumnWidth(2, 130);
    m_tree->setColumnWidth(3, 100);
    m_tree->setColumnWidth(4, 110);
    // 表头排序：不开 QTreeWidget 内建排序（它只比显示文本，"4.00 KB" 会排在
    // "1 GB" 前），改由 sectionClicked 驱动 sortFileTree 按裸值 role 重排，
    // ".." 条目恒置顶；名称列目录在文件前，其余列全列单调混排（见
    // fileTreeSortLess 注释）。指示箭头由我们手动维护。
    // 注意：QHeaderView 的 sectionsClickable 默认是 false（内建排序开启时会被
    // 顺手置 true），自己接管排序必须显式打开，否则点击表头不发 sectionClicked。
    m_tree->header()->setSectionsClickable(true);
    m_tree->header()->setSortIndicatorShown(true);
    m_tree->header()->setSortIndicator(m_sortColumn, m_sortOrder);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    // 进度条旁的取消按钮：下载（含批量队列）走上层 m_sftp->cancelTransfer()，
    // 上传走 m_uploader->cancelUpload()。取消即"暂停"——断点续传已内建，
    // 半成品文件保留，重新传输自动从断点继续。
    m_cancelBtn = new QToolButton(this);
    m_cancelBtn->setText(QStringLiteral("×"));
    m_cancelBtn->setToolTip(tr("取消传输（已传部分保留，可断点续传）"));
    m_cancelBtn->setAutoRaise(true);
    m_cancelBtn->setVisible(false);
    connect(m_cancelBtn, &QToolButton::clicked, this, &SftpBrowserWidget::cancelTransfers);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    // 长文本（如一整段 SFTP 错误信息）会把 label 的 sizeHint 撑得很宽，
    // 进而撑宽左侧文件面板、挤压右侧终端。水平方向改用 Ignored 策略，
    // 让 label 只占据布局实际分给它的宽度，超出部分省略号截断，
    // 完整内容放进 tooltip（见 setStatusText）。
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setMinimumWidth(0);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);
    layout->addWidget(pathBar);
    layout->addWidget(m_filterBar);
    layout->addWidget(m_tree, 1);
    // 进度条 + 取消按钮一行：按钮仅在传输进行中可见（updateCancelButton）。
    auto *progressRow = new QHBoxLayout();
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(4);
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_cancelBtn);
    layout->addLayout(progressRow);
    layout->addWidget(m_status);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &SftpBrowserWidget::onPathEdited);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &SftpBrowserWidget::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::itemExpanded, this, &SftpBrowserWidget::onItemExpanded);
    connect(m_tree->header(), &QHeaderView::sectionClicked,
            this, &SftpBrowserWidget::onHeaderSectionClicked);

    // 筛选：按钮弹出/收起检索框，输入即筛，回车定位首个匹配。
    connect(m_filterBtn, &QToolButton::toggled, this, &SftpBrowserWidget::setFilterBarVisible);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_filterEdit, &QLineEdit::returnPressed, this, &SftpBrowserWidget::focusFirstFilterMatch);
    // Ctrl+F 唤起检索框。作用域限定在本面板内（WidgetWithChildren），
    // 避免抢占终端自己的 Ctrl+F 搜索。
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, m_filterBtn, &QToolButton::toggle);

    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接分支）
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &SftpBrowserWidget::showContextMenu);

    // 拖拽上传：本树不提供内部拖拽（避免误触发内部移动），外部文件拖放
    // 由 viewport 上的 eventFilter 接管（DragEnter/DragMove/Drop）。
    // 对应Python: cube-shell.py::MainWindow setAcceptDrops + dropEvent
    m_tree->setDragDropMode(QAbstractItemView::NoDragDrop);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->viewport()->installEventFilter(this);
    // 检索框里的 Esc 走 eventFilter（QLineEdit 自身不消化 Esc）。
    m_filterEdit->installEventFilter(this);
}

SftpBrowserWidget::~SftpBrowserWidget()
{
    releaseClient();
}

void SftpBrowserWidget::releaseClient()
{
    // 先让旧信号/排队回调失效，再停任务。disconnect 不能撤销已投递的信号，
    // 所以各回调还要检查它捕获的这一代关停标志。
    m_shuttingDown->store(true);
    ++m_loadSeq;
    if (m_sftp)
        m_sftp->disconnect(this);
    if (m_uploader)
        m_uploader->disconnect(this);
    if (m_client)
        m_client->shutdownSocket(); // 唤醒旧连接上的阻塞 I/O，不释放 session
    if (m_sftp)
        m_sftp->beginTeardown();

    bool joinTimedOut = false;
    if (m_uploader && !m_uploader->shutdown(kWorkerJoinTimeoutMs, true))
        joinTimedOut = true;
    if (m_sftp && !m_sftp->joinTransferWorkers(kWorkerJoinTimeoutMs))
        joinTimedOut = true;
    for (const QPointer<QThread> &worker : std::as_const(m_workers)) {
        if (worker) {
            worker->quit();
            if (!worker->wait(kWorkerJoinTimeoutMs)) {
                qWarning() << "SftpBrowserWidget: worker thread did not finish within"
                           << kWorkerJoinTimeoutMs << "ms, leaking its referenced objects";
                joinTimedOut = true;
            }
        }
    }
    m_workers.clear();
    if (joinTimedOut) {
        // 极端超时下不能拆掉仍有租约的连接池，也不能释放其裸指针指向的 SSH。
        // 沿用关闭路径的泄漏兜底，但必须把旧连接一并保留，而非仅泄漏 SFTP。
        if (m_sftp)
            m_sftp->setParent(nullptr);
        if (m_uploader)
            m_uploader->setParent(nullptr);
        (void)new std::shared_ptr<SshClient>(m_client);
    } else {
        // 底层 socket 已断，不再向旧会话发送 SFTP shutdown。必须在 m_client
        // 释放前显式删除子对象，不能等 ~QObject（那时成员 shared_ptr 已析构）。
        if (m_sftp)
            m_sftp->abandon();
        delete m_sftp;
        delete m_uploader;
    }
    m_sftp = nullptr;
    m_uploader = nullptr;
    m_client.reset();
    m_activeUploads.clear();
    m_activeDownloads.clear();
    m_downloadQueue.clear();
    m_downloadFailures.clear();
    m_downloadBatchDone = 0;
    m_cancelPending = false;
    m_progress->setVisible(false);
    updateCancelButton();
}

// 登记并启动自建 worker 线程：finished 后 deleteLater，QPointer 随删除自动置空；
// 启动前顺手清理登记表中已置空的条目，避免长期会话下无限增长。
void SftpBrowserWidget::startWorker(QThread *worker)
{
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_workers.removeAll(QPointer<QThread>());
    m_workers.append(QPointer<QThread>(worker));
    worker->start();
}

void SftpBrowserWidget::setClient(std::shared_ptr<SshClient> client)
{
    if (client == m_client)
        return;
    releaseClient();
    m_client = std::move(client);
    m_shuttingDown = std::make_shared<std::atomic<bool>>(false);
    m_sftpUnavailable = false;
    setEnabled(bool(m_client));
    if (!m_client) {
        setStatusText(tr("连接已断开"));
        return;
    }
    const auto connection = m_shuttingDown;
    SshClient *raw = m_client.get();
    if (!m_sftp) {
        m_sftp = new SftpClient(raw, this);
        // 下载进度按远端路径记账后聚合（与上传侧同款），批量下载时多个
        // transferProgress 才不会把同一个进度条来回拉扯。
        connect(m_sftp, &SftpClient::transferProgress, this,
                [this, connection](const QString &path, qint64 cur, qint64 total) {
            if (connection->load())
                return;
            auto it = m_activeDownloads.find(path);
            if (it == m_activeDownloads.end())
                return; // 非本面板发起的传输（防御）
            it->done = cur;
            it->total = total;
            refreshDownloadProgress();
        });
        connect(m_sftp, &SftpClient::transferFinished, this,
                [this, connection](const QString &path, bool ok, const QString &msg) {
            if (connection->load())
                return;
            // 之前已处理过本批的任何文件（或队列里还有）都算批量：整批全失败时
            // 最后一个文件也要走汇总文案，而不是退回单文件的"传输失败"。
            const bool wasBatch = (m_downloadBatchDone + m_downloadFailures.size()) > 0
                || !m_downloadQueue.isEmpty();
            // 用户主动取消（cancelTransfer 的收尾消息）不算失败。
            const bool cancelled = !ok && msg == QLatin1String("cancelled");
            m_activeDownloads.remove(path);
            if (ok)
                ++m_downloadBatchDone;
            else if (!cancelled)
                m_downloadFailures.append(QFileInfo(path).fileName());
            // 队列非空：单个文件失败不卡整批（与 removeSelected 收集失败项同款哲学）。
            if (!m_downloadQueue.isEmpty()) {
                dispatchNextDownload();
                refreshDownloadProgress();
                return;
            }
            m_progress->setVisible(false);
            if (cancelled) {
                setStatusText(m_downloadBatchDone > 0
                                  ? tr("下载已取消（已完成 %1 个文件）").arg(m_downloadBatchDone)
                                  : tr("已取消：%1").arg(QFileInfo(path).fileName()));
            } else if (wasBatch) {
                // 整批结束一次性汇总（镜像 removeSelected 的失败清单风格）。
                if (m_downloadFailures.isEmpty()) {
                    setStatusText(tr("已完成 %1 个文件的下载").arg(m_downloadBatchDone));
                } else {
                    setStatusText(tr("下载完成：成功 %1 个，失败 %2 个（%3）")
                                      .arg(m_downloadBatchDone)
                                      .arg(m_downloadFailures.size())
                                      .arg(m_downloadFailures.join(QStringLiteral(", "))));
                }
            } else {
                setStatusText(ok ? tr("已完成：%1").arg(QFileInfo(path).fileName())
                                 : tr("传输失败：%1").arg(msg));
            }
            m_downloadFailures.clear();
            m_downloadBatchDone = 0;
            updateCancelButton();
            if (ok)
                refresh();
        });
        connect(m_sftp, &SftpClient::operationFailed, this, [this, connection](const QString &op, const QString &, const QString &msg) {
            if (connection->load())
                return;
            setStatusText(tr("%1 失败：%2").arg(op, msg));
        });
    }
    if (!m_uploader) {
        // 分片上传核心（进度信号已在内部回投到本线程，按工程约定仍显式 QueuedConnection）。
        // 对应Python: core/uploader/sftp_uploader_core.py 的进度信号接线
        m_uploader = new SftpUploaderCore(raw, this);
        // 字节级进度：多文件并行上传时按 fileId 记账后聚合显示，
        // 否则各文件的信号会把同一个进度条来回拉扯。
        connect(m_uploader, &SftpUploaderCore::progressChanged, this,
                [this, connection](const QString &fileId, qint64 done, qint64 total) {
                    if (connection->load())
                        return;
                    // 用 find 而不是 operator[]：已完成/失败/取消后迟到的进度
                    // （上一个被取消的同名列还在收尾时会再发 progressChanged），
                    // operator[] 会把已删除的条目重新插回去，让进度条复活卡住。
                    auto it = m_activeUploads.find(fileId);
                    if (it == m_activeUploads.end())
                        return;
                    it->done = done;
                    it->total = total;
                    refreshUploadProgress();
                }, Qt::QueuedConnection);
        connect(m_uploader, &SftpUploaderCore::uploadCompleted, this,
                [this, connection](const QString &fileId, const QString &filename) {
                    if (connection->load())
                        return;
                    m_activeUploads.remove(fileId);
                    if (m_activeUploads.isEmpty()) {
                        m_progress->setVisible(false);
                        setStatusText(tr("上传完成：%1").arg(filename));
                    } else {
                        refreshUploadProgress();
                    }
                    updateCancelButton();
                    refresh();
                }, Qt::QueuedConnection);
        connect(m_uploader, &SftpUploaderCore::uploadFailed, this,
                [this, connection](const QString &fileId, const QString &filename, const QString &error) {
                    if (connection->load())
                        return;
                    m_activeUploads.remove(fileId);
                    if (m_activeUploads.isEmpty())
                        m_progress->setVisible(false);
                    else
                        refreshUploadProgress();
                    // 用户主动取消不算失败（cancelUpload 的收尾错误是"上传已取消"）。
                    if (error.contains(QStringLiteral("已取消")))
                        setStatusText(tr("已取消上传：%1").arg(filename));
                    else
                        setStatusText(tr("上传失败：%1（%2）").arg(filename, error));
                    updateCancelButton();
                }, Qt::QueuedConnection);
    }
    // 后台预建并行传输连接：每条要走完整 SSH 握手，等用户点上传时再建
    // 会把握手成本压在首次传输上（高延迟链路上足以让并行反而更慢）。
    m_uploader->prewarmConnections();
    // 首次连接 m_cwd 为空：工作线程先解析 SFTP 登录目录，再一次性展示。
    // 显式指定路径或重连时已有 m_cwd，直接恢复它，不重新定位到用户目录。
    loadPath(m_cwd, LoadReason::Initial);
}

QString SftpBrowserWidget::joinPath(const QString &dir, const QString &name)
{
    if (dir == QLatin1String("/"))
        return QLatin1String("/") + name;
    return dir + QLatin1Char('/') + name;
}

// 对应Python: downloadFile 对 is_dir 的过滤（目录不进下载队列）
void SftpBrowserWidget::partitionDownloadSelection(
    const QList<QPair<QString, bool>> &selected, QStringList &files, QStringList &dirs)
{
    files.clear();
    dirs.clear();
    for (const auto &entry : selected) {
        if (entry.first.isEmpty())
            continue;
        (entry.second ? dirs : files).append(entry.first);
    }
}

QString SftpBrowserWidget::downloadTargetPath(const QString &dir, const QString &remotePath)
{
    const QString name = QFileInfo(remotePath).fileName();
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

// 对应Python: cube-shell.py::_on_cwd_changed（路径没变则不刷新）
void SftpBrowserWidget::setCurrentPath(const QString &path)
{
    QString clean = path;
    while (clean.size() > 1 && clean.endsWith(QLatin1Char('/')))
        clean.chop(1);
    if (clean.isEmpty())
        clean = QStringLiteral("/");
    if (!m_sftp) {
        // 连接尚未就绪：记下路径，setClient 时作为初始目录加载。
        m_cwd = clean;
        m_pathEdit->setText(clean);
        return;
    }
    if (clean == m_cwd)
        return;
    // 已判定该通道不提供文件系统：终端每换一次目录都去试一遍只会白费往返，
    // 还会把说明行刷掉又刷回来。
    if (m_sftpUnavailable)
        return;
    // CwdSync：终端 cwd 与 SFTP 通道未必是同一命名空间，失败不弹错误。
    loadPath(clean, LoadReason::CwdSync);
}

void SftpBrowserWidget::setFontSize(int pointSize)
{
    setFileTreeFontSize(m_tree, pointSize);
}

// 刷新路径栏左侧的分屏徽章。单分屏时隐藏，完整信息走 tooltip。
void SftpBrowserWidget::setPaneIndicator(int paneNumber, int totalPanes,
                                         const QString &tabTitle)
{
    updatePaneBadge(m_paneBadge, paneNumber, totalPanes, tabTitle);
}

void SftpBrowserWidget::setStatusText(const QString &text)
{
    m_statusFullText = text;
    // 按当前可用宽度做省略号截断；完整文本放 tooltip，鼠标悬停可看全。
    // 布局尚未给出宽度（构造初期）时先放全文，随后 resizeEvent 会按实际宽度截断。
    const int w = m_status->width();
    m_status->setText(w > 0 ? m_status->fontMetrics().elidedText(text, Qt::ElideMiddle, w)
                            : text);
    m_status->setToolTip(text);
}

void SftpBrowserWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 面板宽度变化（拖动分栏等）后按新宽度重新截断状态栏文本。
    if (!m_statusFullText.isEmpty())
        setStatusText(m_statusFullText);
}

void SftpBrowserWidget::loadPath(const QString &path, LoadReason reason, bool preserveTree)
{
    if (!m_sftp)
        return;
    // 关键：不在此处清空树、也不改路径栏。网络等待期间旧目录内容与路径栏
    // 保持一致（均为 m_cwd），依赖 m_cwd 的操作（新建/上传等）不会被误导；
    // 数据就绪后由 populate() 在挂起重绘的状态下一次性原子替换，消除“先空后满”的闪烁。
    // 对应Python: refreshDirs/handle_file_tree_updated（新数据就绪前不动旧树）
    const bool resolveHome = path.isEmpty();
    setStatusText(resolveHome ? tr("正在加载用户目录…") : tr("正在加载 %1 …").arg(path));

    // 陈旧响应防护：每次加载递增序号，回调只接受最新一次请求的结果，
    // 防止快速连续切换根目录时乱序返回的旧数据覆盖新目录。
    const quint64 seq = ++m_loadSeq;

    // Fetch on a worker thread (listdirAttr is blocking under the session lock).
    SftpClient *sftp = m_sftp;
    const auto connection = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, sftp, path, seq, reason, preserveTree,
                                       resolveHome, connection]() {
        SshError err;
        const QString targetPath = resolveHome ? sftp->realpath(QStringLiteral("."), err) : path;
        SftpFileInfoList entries;
        if (!targetPath.isEmpty() && !connection->load())
            entries = sftp->listdirAttr(targetPath, err);
        const QString errMsg = err.message;
        if (connection->load() || !self)
            return;
        // 回主线程渲染（与 editSelected/decompressSelected 同款接线）。
        QMetaObject::invokeMethod(self, [this, path = targetPath, seq, reason, preserveTree,
                                        resolveHome, entries, errMsg, connection]() {
            if (connection->load() || seq != m_loadSeq) // 换连接或已有更新的请求
                return;
            if (entries.isEmpty() && !errMsg.isEmpty()) {
                if (resolveHome) {
                    // 初始目录失败时允许刷新/手动输入路径，不隐式加载并展示 "/"。
                    setStatusText(tr("无法加载用户目录：%1").arg(errMsg));
                    m_pathEdit->setText(m_cwd);
                    m_pathEdit->setPlaceholderText(tr("请输入远程目录路径"));
                    return;
                }
                if (reason == LoadReason::UserRequested) {
                    // 用户主动去的地方去不了，必须说清楚：旧树原样保留，路径栏
                    // 回退到 m_cwd（手输无效路径时残留的错误文本会破坏
                    // “路径栏 == m_cwd == 树内容”不变量），状态栏给出目标路径
                    // 与失败原因，避免误认旧内容为目标目录。
                    setStatusText(tr("加载 %1 失败：%2").arg(path, errMsg));
                    m_pathEdit->setText(m_cwd);
                    return;
                }
                // 推测性加载（终端 cwd 联动 / 首次加载）失败：这是我们替用户猜的，
                // 不能弹错误。终端 cwd 与 SFTP 通道可能不是同一个命名空间——
                // 跳板机代理、chroot 过的 sftp 子系统都会这样。
                qInfo("SFTP: 推测性加载 %s 失败（%s），退回根目录",
                      qPrintable(path), qPrintable(errMsg));
                m_pathEdit->setText(m_cwd);
                // 退回根目录，让面板落到一个可用的位置，而不是留着陈旧空树。
                // 已经在试根目录了就别再递归，直接判定这条通道没有文件系统。
                if (path != QLatin1String("/")) {
                    loadPath(QStringLiteral("/"), LoadReason::Initial);
                    return;
                }
                if (m_bastionProxied) {
                    // 树里放的不再是任何目录的内容，路径栏跟着落到刚探测过的根
                    // 目录（此处 path 恒为 "/"），否则会停在探测失败的旧路径上，
                    // 破坏“路径栏 == m_cwd == 树内容”不变量。
                    m_cwd = path;
                    m_pathEdit->setText(m_cwd);
                    showUnavailableNotice();
                } else
                    setStatusText(tr("无法列出目录：%1").arg(errMsg));
                return;
            }
            // 代理端连根目录都是空的 ⇒ 它不提供资产文件系统，给出准确说明，
            // 而不是让用户对着一棵空树猜。
            if (sftpLooksUnavailable(m_bastionProxied, path, int(entries.size()))) {
                m_cwd = path;
                m_pathEdit->setText(m_cwd);
                showUnavailableNotice();
                return;
            }
            // 成功后才一次性提交：保证任意时刻路径栏 == m_cwd == 树内容所属目录
            m_sftpUnavailable = false;
            // 换了目录就清掉旧筛选词（留在原样会让用户误以为新目录是空的）；
            // 同目录刷新（path == m_cwd）保留筛选，populate 会重应用。
            if (path != m_cwd && !m_filterEdit->text().isEmpty())
                m_filterEdit->clear();
            const bool preserveLoadedTree = preserveTree && path == m_cwd;
            m_cwd = path;
            m_pathEdit->setText(m_cwd);
            populate(path, entries, preserveLoadedTree);
            setStatusText(tr("%1 · %2 项").arg(path).arg(entries.size()));
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 只在 path 为根时判定：非根路径列不出来可能只是权限或路径不存在。
bool SftpBrowserWidget::sftpLooksUnavailable(bool bastionProxied, const QString &path,
                                             int entryCount)
{
    return bastionProxied && path == QLatin1String("/") && entryCount == 0;
}

// 对应Python: cube-shell.py::_on_file_tree_unavailable
void SftpBrowserWidget::showUnavailableNotice()
{
    m_sftpUnavailable = true;
    m_tree->setEnabled(true);
    // 没有文件系统可给：筛选无意义，入口禁掉并收起检索框（说明行不该被筛选藏掉）。
    m_filterEdit->clear();
    m_filterBtn->setChecked(false);
    m_filterBtn->setEnabled(false);
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    // 一行不可选中的说明，占位在原本列目录的地方（Python 侧是 add_line_edit）。
    auto *item = new QTreeWidgetItem(m_tree);
    item->setFirstColumnSpanned(true);
    item->setText(0, tr("此连接经 JumpServer 代理，代理端未提供 SFTP 文件浏览。"
                        "文件传输请在 JumpServer 网页端进行。"));
    item->setFlags(Qt::NoItemFlags);   // 不可选中/不可双击，避免当成目录点进去
    m_tree->setUpdatesEnabled(true);
    setStatusText(tr("已连接 · SFTP 不可用"));
}

// 写操作闸门：不可用态下统一给说明，别让用户逐个撞 libssh2 原始错误。
bool SftpBrowserWidget::blockedByUnavailable(const QString &title)
{
    if (!m_sftpUnavailable)
        return false;
    QMessageBox::information(this, title,
                             tr("此连接经 JumpServer 代理，代理端未提供 SFTP 文件传输。"
                                "请在 JumpServer 网页端进行文件传输。"));
    return true;
}

// 渲染根目录。preserveTree=true 时按名称更新顶层条目，保留各目录已加载的
// 子孙与展开状态；切换根目录则仍一次性替换整棵树，避免旧数据闪烁。
void SftpBrowserWidget::populate(const QString &path, const SftpFileInfoList &entries,
                                 bool preserveTree)
{
    m_tree->setUpdatesEnabled(false);

    if (!preserveTree) {
        m_tree->clear();
        // ".." 只出现在树根，用于把整棵树切换到上级目录；展开树内节点不再
        // 改变根目录，因此子目录里不需要重复的 ".." 行。
        if (path != QLatin1String("/")) {
            QString up = path.section(QLatin1Char('/'), 0, -2);
            if (up.isEmpty())
                up = QStringLiteral("/");
            auto *upItem = new QTreeWidgetItem(m_tree);
            upItem->setText(0, QStringLiteral(".."));
            upItem->setIcon(0, iconForFile(QString(), true));
            upItem->setData(0, kPathRole, up);
            upItem->setData(0, kIsDirRole, true);
            upItem->setData(0, kIsUpRole, true);
        }
    }

    populateDirectory(nullptr, path, entries);
    m_entryCount = int(entries.size());
    // 排序与筛选跨刷新保持，递归应用到所有已经加载的目录。
    sortFileTree(m_tree, m_sortColumn, m_sortOrder);
    if (!m_filterEdit->text().trimmed().isEmpty())
        applyFileFilter(m_tree, m_filterEdit->text());
    // 成功列出目录 ⇒ 通道可用，开放文件操作与筛选。
    m_tree->setEnabled(true);
    m_filterBtn->setEnabled(true);
    m_tree->setUpdatesEnabled(true);
}

// 填充某一层的直接子项。刷新时复用同名同类型节点，已加载子树不会被清掉；
// 新目录先挂一个不可选占位子项，QTreeWidget 因而立即显示展开箭头。
void SftpBrowserWidget::populateDirectory(QTreeWidgetItem *parent, const QString &path,
                                          const SftpFileInfoList &entries)
{
    QHash<QString, QTreeWidgetItem *> existing;
    const int oldCount = parent ? parent->childCount() : m_tree->topLevelItemCount();
    for (int i = oldCount - 1; i >= 0; --i) {
        QTreeWidgetItem *item = parent ? parent->child(i) : m_tree->topLevelItem(i);
        if (item->data(0, kIsUpRole).toBool())
            continue;
        if (item->data(0, kIsPlaceholderRole).toBool()
            || item->data(0, kPathRole).toString().isEmpty()) {
            delete item;
            continue;
        }
        existing.insert(item->text(0), item);
    }

    for (const SftpFileInfo &e : entries) {
        QTreeWidgetItem *item = existing.take(e.filename);
        if (item && item->data(0, kIsDirRole).toBool() != e.isDirectory()) {
            delete item; // 远端条目换了类型，旧子树不能沿用
            item = nullptr;
        }
        const bool isNew = !item;
        if (!item)
            item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);

        const QString fullPath = joinPath(path, e.filename);
        item->setText(0, e.filename);
        // 目录也展示大小（4096 → "4.00 KB"），与 Python 侧 ls 一致
        item->setText(1, formatFileSize(e.size));
        item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setText(3, permissionText(e));
        item->setText(4, ownerText(e));
        item->setData(0, kPathRole, fullPath);
        item->setData(0, kIsDirRole, e.isDirectory());
        item->setData(0, kModeRole, e.permissions());
        item->setData(0, kSymlinkTargetRole, e.symlinkTarget);
        // 排序用裸值：显示列是格式化文本，直接比字符串会把 "1 GB" 排到 "4.00 KB" 前。
        item->setData(0, kSizeRole, e.size);
        item->setData(0, kMtimeRole, e.mtime);
        item->setIcon(0, iconForFile(e.filename, e.isDirectory(), e.isSymlink(), e.mode));

        if (e.isDirectory()) {
            if (isNew)
                item->setData(0, kChildrenStateRole, ChildrenNotLoaded);
            const int state = item->data(0, kChildrenStateRole).toInt();
            if (state != ChildrenLoaded && item->childCount() == 0) {
                auto *placeholder = new QTreeWidgetItem(item);
                placeholder->setText(0, tr("正在加载…"));
                placeholder->setData(0, kIsPlaceholderRole, true);
                placeholder->setFlags(Qt::NoItemFlags);
            }
        }
    }
    // 远端已经不存在的条目连同其子树一起移除。
    for (QTreeWidgetItem *item : std::as_const(existing))
        delete item;
}

QTreeWidgetItem *SftpBrowserWidget::findDirectoryItem(const QString &path) const
{
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        QTreeWidgetItem *item = *it;
        if (!item->data(0, kIsUpRole).toBool()
            && item->data(0, kIsDirRole).toBool()
            && item->data(0, kPathRole).toString() == path)
            return item;
        ++it;
    }
    return nullptr;
}

// 文件操作的目标目录：选中目录就是该目录，选中文件就是其父目录；
// 空白或 ".." 仍回落到路径栏所示的树根目录。
QString SftpBrowserWidget::currentDirectoryPath() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || item->data(0, kIsUpRole).toBool())
        return m_cwd;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty())
        return item->parent() ? item->parent()->data(0, kPathRole).toString() : m_cwd;
    if (item->data(0, kIsDirRole).toBool())
        return path;
    QString parent = path.section(QLatin1Char('/'), 0, -2);
    return parent.isEmpty() ? QStringLiteral("/") : parent;
}

// 展开目录的独立懒加载请求。不能复用根目录的“只接受最后一次请求”策略：
// 树里允许同时有多个目录在加载，每个节点只淘汰自己的旧请求。
void SftpBrowserWidget::loadChildren(QTreeWidgetItem *item, bool force)
{
    if (!item || !m_sftp || item->data(0, kIsUpRole).toBool()
        || !item->data(0, kIsDirRole).toBool())
        return;
    const int oldState = item->data(0, kChildrenStateRole).toInt();
    if (oldState == ChildrenLoading || (!force && oldState == ChildrenLoaded))
        return;

    const bool hadLoadedChildren = oldState == ChildrenLoaded;
    const QString path = item->data(0, kPathRole).toString();
    const quint64 seq = ++m_dirLoadSeq;
    item->setData(0, kDirLoadSeqRole, seq);
    item->setData(0, kChildrenStateRole, ChildrenLoading);
    setStatusText(tr("正在加载 %1 …").arg(path));

    SftpClient *sftp = m_sftp;
    const auto connection = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, sftp, path, seq,
                                       hadLoadedChildren, connection]() {
        SshError err;
        const SftpFileInfoList entries = sftp->listdirAttr(path, err);
        const QString errMsg = err.message;
        if (connection->load() || !self)
            return;
        QMetaObject::invokeMethod(self, [this, path, seq, hadLoadedChildren,
                                         entries, errMsg, connection]() {
            if (connection->load())
                return;
            // 不直接捕获 QTreeWidgetItem：刷新/换根可能已把它删除。结果回来后
            // 按完整路径找当前节点，再核对节点上的请求序号。
            QTreeWidgetItem *target = findDirectoryItem(path);
            if (!target || target->data(0, kDirLoadSeqRole).toULongLong() != seq)
                return;
            target->setData(0, kDirLoadSeqRole, qulonglong(0));
            if (entries.isEmpty() && !errMsg.isEmpty()) {
                target->setData(0, kChildrenStateRole,
                                hadLoadedChildren ? ChildrenLoaded : ChildrenNotLoaded);
                if (!hadLoadedChildren)
                    target->setExpanded(false); // 保留占位子项，下次展开可重试
                setStatusText(tr("加载 %1 失败：%2").arg(path, errMsg));
                return;
            }
            populateDirectory(target, path, entries);
            target->setData(0, kChildrenStateRole, ChildrenLoaded);
            sortFileTree(m_tree, m_sortColumn, m_sortOrder);
            if (!m_filterEdit->text().trimmed().isEmpty())
                applyFileFilter(m_tree, m_filterEdit->text());
            setStatusText(tr("%1 · %2 项").arg(path).arg(entries.size()));
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

void SftpBrowserWidget::onItemExpanded(QTreeWidgetItem *item)
{
    if (item && item->data(0, kIsDirRole).toBool()
        && item->data(0, kChildrenStateRole).toInt() == ChildrenNotLoaded)
        loadChildren(item);
}

void SftpBrowserWidget::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
    if (!item)
        return;
    if (item->data(0, kIsUpRole).toBool()) {
        loadPath(item->data(0, kPathRole).toString());
        return;
    }
    if (!item->data(0, kIsDirRole).toBool())
        return;
    m_tree->setCurrentItem(item);
    const bool expand = !item->isExpanded();
    item->setExpanded(expand); // itemExpanded 会在首次展开时启动懒加载
    if (expand && item->data(0, kChildrenStateRole).toInt()
            == ChildrenNotLoaded)
        loadChildren(item);
}

void SftpBrowserWidget::goUp()
{
    if (m_cwd.isEmpty() || m_cwd == QLatin1String("/"))
        return;
    QString up = m_cwd.section(QLatin1Char('/'), 0, -2);
    if (up.isEmpty())
        up = QStringLiteral("/");
    loadPath(up);
}

void SftpBrowserWidget::refresh()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (item && !item->data(0, kIsUpRole).toBool()) {
        QTreeWidgetItem *directory = item->data(0, kIsDirRole).toBool()
            ? item : item->parent();
        if (directory) {
            loadChildren(directory, true);
            return;
        }
    }
    // 当前项位于顶层：刷新树根，但保留已加载/展开的分支。
    loadPath(m_cwd, LoadReason::UserRequested, true);
}

void SftpBrowserWidget::onPathEdited()
{
    const QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty())
        refresh();
    else
        loadPath(path);
}

// 表头点击：同列再点切换升降序，换列从升序开始（文件管理器惯例）。
void SftpBrowserWidget::onHeaderSectionClicked(int logical)
{
    if (logical == m_sortColumn) {
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder
                                                          : Qt::AscendingOrder;
    } else {
        m_sortColumn = logical;
        m_sortOrder = Qt::AscendingOrder;
    }
    m_tree->header()->setSortIndicator(m_sortColumn, m_sortOrder);
    sortFileTree(m_tree, m_sortColumn, m_sortOrder);
}

void SftpBrowserWidget::setFilterBarVisible(bool on)
{
    m_filterBar->setVisible(on);
    if (on) {
        m_filterEdit->setFocus();
        m_filterEdit->selectAll();
    } else {
        // 收起即还原完整列表；text 为空时 clear 不发 textChanged，手动补一次。
        if (!m_filterEdit->text().isEmpty())
            m_filterEdit->clear();
        else
            applyFilter();
        m_tree->setFocus();
    }
}

void SftpBrowserWidget::applyFilter()
{
    if (m_sftpUnavailable)
        return;
    const QString needle = m_filterEdit->text().trimmed();
    const int visible = applyFileFilter(m_tree, m_filterEdit->text());
    // 有筛选词时状态栏报匹配数；清空后还原成目录条目计数。
    if (!needle.isEmpty())
        setStatusText(tr("%1 · 匹配 %2 项").arg(m_cwd).arg(visible));
    else
        setStatusText(tr("%1 · %2 项").arg(m_cwd).arg(m_entryCount));
}

// 回车定位：在所有已加载层级中选中第一个可见匹配项，展开其祖先并滚动到
// 该节点；随后把焦点还给树，用户可直接用方向键继续移动。
void SftpBrowserWidget::focusFirstFilterMatch()
{
    const QString needle = m_filterEdit->text().trimmed();
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        QTreeWidgetItem *item = *it;
        if (!item->isHidden() && !item->data(0, kIsUpRole).toBool()
            && !item->data(0, kIsPlaceholderRole).toBool()
            && fileMatchesFilter(item->text(0), needle)) {
            for (QTreeWidgetItem *p = item->parent(); p; p = p->parent())
                p->setExpanded(true);
            m_tree->setCurrentItem(item);
            m_tree->scrollToItem(item);
            m_tree->setFocus();
            return;
        }
        ++it;
    }
}

QString SftpBrowserWidget::selectedRemotePath() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    // ".." 不参与针对选中项的操作（删除/重命名/下载等）。
    if (!item || item->data(0, kIsUpRole).toBool())
        return QString();
    return item->data(0, kPathRole).toString();
}

// 批量选中项的远端路径。对应本地侧 LocalFileBrowserWidget::selectedPaths。
QStringList SftpBrowserWidget::selectedRemotePaths() const
{
    QStringList paths;
    const QList<QTreeWidgetItem *> items = m_tree->selectedItems();
    for (QTreeWidgetItem *item : items) {
        if (item->data(0, kIsUpRole).toBool())
            continue;
        const QString p = item->data(0, kPathRole).toString();
        if (!p.isEmpty())
            paths.append(p);
    }
    return paths;
}

void SftpBrowserWidget::mkdir()
{
    if (blockedByUnavailable(tr("创建文件夹")))
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件夹"), tr("文件夹名称："),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = joinPath(currentDirectoryPath(), name.trimmed());
    SshError err;
    // Quick op; run directly (short under the lock).
    if (m_sftp->mkdir(path, 0755, &err))
        refresh();
    else
        QMessageBox::warning(this, tr("创建文件夹"), err.message);
}

// 删除：支持批量（多选时逐个递归删除）。对应Python: remove（批量 + 确认框）
void SftpBrowserWidget::removeSelected()
{
    const QStringList paths = selectedRemotePaths();
    if (paths.isEmpty())
        return;
    // 多选可能横跨多个已展开目录；删除后逐个刷新这些父目录，而不是刷新
    // 可能已经消失的选中目录自身。
    QStringList refreshPaths;
    const QList<QTreeWidgetItem *> selectedItems = m_tree->selectedItems();
    for (QTreeWidgetItem *item : selectedItems) {
        if (item->data(0, kIsUpRole).toBool())
            continue;
        QTreeWidgetItem *parent = item->parent();
        const QString parentPath = parent ? parent->data(0, kPathRole).toString() : m_cwd;
        if (!refreshPaths.contains(parentPath))
            refreshPaths.append(parentPath);
    }
    // 单项沿用原文案（含完整路径），多项时给出数量与文件名清单。
    QString prompt;
    if (paths.size() == 1) {
        prompt = tr("确定要递归删除 %1 吗？").arg(paths.first());
    } else {
        QStringList names;
        for (const QString &p : paths)
            names.append(p.section(QLatin1Char('/'), -1));
        prompt = tr("确定要递归删除选中的 %1 项吗？\n\n%2")
                     .arg(paths.size())
                     .arg(names.join(QLatin1Char('\n')));
    }
    if (QMessageBox::question(this, tr("删除"), prompt) != QMessageBox::Yes)
        return;
    SftpClient *sftp = m_sftp;
    // 整批在同一 worker 里顺序删除：SftpClient 内部串行化 libssh2，
    // 并发多线程删除无收益且会争锁。失败项收集后一次性汇报。
    QThread *worker = QThread::create([sftp, paths]() {
        QStringList failed;
        for (const QString &path : paths) {
            SshError err;
            if (!sftp->removeRecursive(path, &err))
                failed.append(path.section(QLatin1Char('/'), -1));
        }
        if (!failed.isEmpty()) {
            const QString msg = failed.join(QStringLiteral(", "));
            QMetaObject::invokeMethod(sftp, [sftp, msg]() {
                emit sftp->operationFailed(QStringLiteral("remove"), QString(), msg);
            }, Qt::QueuedConnection);
        }
    });
    const auto connection = m_shuttingDown;
    connect(worker, &QThread::finished, this, [this, connection, refreshPaths]() {
        if (connection->load())
            return;
        for (const QString &path : refreshPaths) {
            if (path == m_cwd) {
                loadPath(m_cwd, LoadReason::UserRequested, true);
            } else if (QTreeWidgetItem *directory = findDirectoryItem(path)) {
                loadChildren(directory, true);
            }
        }
    });
    startWorker(worker);
}

// 聚合所有在传文件的字节数刷新进度条。并行上传下单个文件的百分比没有参考
// 意义（几个文件同时推进），这里统一按总字节算，状态栏显示文件数和并行度。
void SftpBrowserWidget::refreshUploadProgress()
{
    if (m_activeUploads.isEmpty()) {
        m_progress->setVisible(false);
        updateCancelButton();
        return;
    }
    qint64 done = 0, total = 0;
    for (const UploadProgress &p : std::as_const(m_activeUploads)) {
        done += p.done;
        total += p.total;
    }
    m_progress->setVisible(true);
    m_progress->setMaximum(100);
    const int percent = total > 0 ? int(qMin<qint64>(100, done * 100 / total)) : 0;
    m_progress->setValue(percent);

    const int fileCount = m_activeUploads.size();
    const int streams = m_uploader ? m_uploader->activeTransferConnections() : 1;
    if (fileCount > 1) {
        setStatusText(tr("正在上传 %1 个文件 … %2%（%3 条并行连接）")
                              .arg(fileCount).arg(percent).arg(streams));
    } else {
        setStatusText(tr("正在上传 %1 … %2%（%3 条并行连接）")
                              .arg(QFileInfo(m_activeUploads.constBegin().key()).fileName())
                              .arg(percent).arg(streams));
    }
    updateCancelButton();
}

void SftpBrowserWidget::uploadFiles()
{
    if (!m_uploader)
        return;
    if (blockedByUnavailable(tr("上传文件")))
        return;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("上传文件"));
    const QString targetDir = currentDirectoryPath();
    for (const QString &local : files)
        enqueueUpload(local, joinPath(targetDir, QFileInfo(local).fileName()));
    refreshUploadProgress();
}

// 单个上传任务入队：进度记账 + 分发到 SftpUploaderCore 线程池。
// 分片上传（断点续传）；fileId 用远端路径。多文件会被线程池并行分发到不同
// 传输连接，单个大文件还会再拆成多条流并行 —— 见 SftpTransferPool.h。
// 对应Python: sftp_uploader_core.py::upload_file
void SftpBrowserWidget::enqueueUpload(const QString &local, const QString &remote)
{
    m_cancelPending = false;   // 新一轮传输：取消闸门复位（见 updateCancelButton）
    m_activeUploads.insert(remote, UploadProgress{0, QFileInfo(local).size()});
    m_uploader->uploadFile(remote, local, remote);
}

// ---- 拖拽上传 ---------------------------------------------------------------

// 视图自己的 DragEnter/Drop 默认处理只对内部拖放有意义；外部 URL 拖放在
// eventFilter 里 preempt 掉，统一走 handleDropEvent。
bool SftpBrowserWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_filterEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Esc：收起检索框并还原列表（清空由 setFilterBarVisible 负责）。
        if (ke->key() == Qt::Key_Escape) {
            m_filterBtn->setChecked(false);
            return true;
        }
    }
    if (obj == m_tree->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *de = static_cast<QDragMoveEvent *>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            handleDropEvent(static_cast<QDropEvent *>(event));
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

QString SftpBrowserWidget::dropTargetDir(const QString &itemPath, bool itemIsDir,
                                         const QString &cwd)
{
    // 落在目录条目上进该目录；".." 条目的 kPathRole 即上级路径，天然覆盖。
    return itemIsDir ? itemPath : cwd;
}

QList<QPair<QString, QString>>
SftpBrowserWidget::collectUploadTasks(const QString &targetDir, const QStringList &localPaths)
{
    // 目标目录去掉尾斜杠（根 "/" 除外），免得 joinPath 拼出 "//name"。
    QString base = targetDir;
    while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QList<QPair<QString, QString>> tasks;
    for (const QString &local : localPaths) {
        if (local.isEmpty())
            continue;
        const QFileInfo info(local);
        if (info.isDir()) {
            // 文件夹递归：远端 = 目标目录 + 顶层文件夹名 + 相对路径，
            // 保留目录结构；远端父目录由 SftpUploaderCore 上传时自动补建。
            const QString topName = info.fileName();
            const QString basePath = info.absoluteFilePath();
            QDirIterator it(basePath, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString filePath = it.next();
                QString rel = filePath.mid(basePath.size() + 1);
                rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
                tasks.append({filePath, joinPath(joinPath(base, topName), rel)});
            }
        } else if (info.isFile()) {
            tasks.append({info.absoluteFilePath(), joinPath(base, info.fileName())});
        }
    }
    return tasks;
}

void SftpBrowserWidget::handleDropEvent(QDropEvent *event)
{
    if (!m_uploader)
        return;
    if (blockedByUnavailable(tr("上传文件")))
        return;

    QStringList localPaths;
    const QList<QUrl> urls = event->mimeData()->urls();
    localPaths.reserve(urls.size());
    for (const QUrl &url : urls) {
        const QString local = url.toLocalFile();
        if (!local.isEmpty())
            localPaths.append(local);
    }

    const QTreeWidgetItem *item = m_tree->itemAt(event->position().toPoint());
    if (item && item->data(0, kIsPlaceholderRole).toBool())
        item = item->parent(); // 加载提示行代表其父目录，不能误落到根目录
    // 落到深层文件上时上传到它所在的目录，而不是路径栏所示的树根。
    QString fallbackDir = m_cwd;
    if (item && !item->data(0, kIsDirRole).toBool()) {
        fallbackDir = item->data(0, kPathRole).toString().section(QLatin1Char('/'), 0, -2);
        if (fallbackDir.isEmpty())
            fallbackDir = QStringLiteral("/");
    }
    const QString targetDir = item
        ? dropTargetDir(item->data(0, kPathRole).toString(),
                        item->data(0, kIsDirRole).toBool(), fallbackDir)
        : m_cwd;

    const QList<QPair<QString, QString>> tasks = collectUploadTasks(targetDir, localPaths);
    if (tasks.isEmpty()) {
        setStatusText(tr("没有可上传的文件"));
        return;
    }
    for (const auto &task : tasks)
        enqueueUpload(task.first, task.second);
    refreshUploadProgress();
    event->acceptProposedAction();
}

// 下载：支持批量。单选文件保持"另存为"行为不变；多选时选目标文件夹，
// 入队后串行逐文件下载（队列语义见 .h 的 m_downloadQueue 注释）。
// 对应Python: downloadFile（多选时逐个下载到所选目录）
void SftpBrowserWidget::downloadSelected()
{
    QList<QPair<QString, bool>> selected;
    const QList<QTreeWidgetItem *> items = m_tree->selectedItems();
    for (QTreeWidgetItem *item : items) {
        if (item->data(0, kIsUpRole).toBool())
            continue;
        const QString p = item->data(0, kPathRole).toString();
        if (!p.isEmpty())
            selected.append({p, item->data(0, kIsDirRole).toBool()});
    }
    QStringList files, dirs;
    partitionDownloadSelection(selected, files, dirs);
    if (files.isEmpty() && dirs.isEmpty())
        return;

    if (files.size() == 1 && dirs.isEmpty()) {
        // 单文件：维持原有"另存为"交互。
        const QString local = QFileDialog::getSaveFileName(
            this, tr("另存为"), QFileInfo(files.first()).fileName());
        if (local.isEmpty())
            return;
        m_progress->setVisible(true);
        m_cancelPending = false;   // 新一轮传输：取消闸门复位
        m_activeDownloads.insert(files.first(), UploadProgress{0, 0});
        updateCancelButton();
        m_sftp->download(files.first(), local);
        return;
    }

    // 多文件/含目录：选保存文件夹（与本地侧 LocalFileBrowserWidget::downloadSelected 同款对话框）。
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择保存文件夹"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty())
        return;
    m_downloadQueue.clear();
    m_downloadFailures.clear();
    m_downloadBatchDone = 0;
    // 散文件直接入队。
    for (const QString &remote : files)
        m_downloadQueue.enqueue(DownloadTask{remote, downloadTargetPath(dir, remote)});

    if (dirs.isEmpty()) {
        m_progress->setVisible(true);
        dispatchNextDownload();
        return;
    }
    // 含目录：worker 线程递归展开成文件任务，并入队列后点火。
    expandDirsAndDownload(dirs, dir);
}

// 含目录下载的入口：递归展开每个远端目录为平铺文件任务，保持目录结构落到
// localRoot/<目录名>/ 下。展开是阻塞 listdir 链，放 worker 线程避免卡 UI。
void SftpBrowserWidget::expandDirsAndDownload(const QStringList &dirs,
                                              const QString &localRoot)
{
    if (!m_sftp)
        return;
    setStatusText(tr("正在扫描文件夹…"));
    SftpClient *sftp = m_sftp;
    const auto connection = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, sftp, dirs, localRoot, connection]() {
        QList<DownloadTask> tasks;
        QStringList failed;
        for (const QString &d : dirs) {
            QString err;
            // 本地落点：所选目录名作为 localRoot 下的一级子目录。
            collectDownloadRecursive(sftp, d, downloadTargetPath(localRoot, d),
                                     tasks, err);
            if (!err.isEmpty())
                failed.append(QStringLiteral("%1（%2）").arg(d, err));
        }
        if (connection->load() || !self)
            return;
        QMetaObject::invokeMethod(self, [this, tasks, failed, connection]() {
            if (connection->load())
                return;
            for (const DownloadTask &t : tasks)
                m_downloadQueue.enqueue(t);
            m_downloadFailures.append(failed);
            if (tasks.isEmpty()) {
                // 没扫到任何文件（全空目录或全部失败）：不进队列，直接汇报。
                m_progress->setVisible(false);
                if (!failed.isEmpty())
                    setStatusText(tr("文件夹扫描失败：%1").arg(failed.join(tr("；"))));
                else
                    setStatusText(tr("所选文件夹为空"));
                updateCancelButton();
                return;
            }
            m_progress->setVisible(true);
            dispatchNextDownload();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 递归把 remoteDir 展开成 (remote, local) 文件任务。子目录递归、本地目录随之
// mkpath（空目录也因此被保留）；符号链接不跟随，避免成环。出错写 err 后返回，
// 已收集的任务保留（尽力而为）。
void SftpBrowserWidget::collectDownloadRecursive(SftpClient *sftp,
                                                 const QString &remoteDir,
                                                 const QString &localDir,
                                                 QList<DownloadTask> &out,
                                                 QString &err)
{
    if (!QDir().mkpath(localDir)) {
        err = tr("无法创建本地目录 %1").arg(localDir);
        return;
    }
    SshError e;
    const SftpFileInfoList entries = sftp->listdirAttr(remoteDir, e);
    if (entries.isEmpty() && !e.message.isEmpty()) {
        err = e.message;
        return;
    }
    for (const SftpFileInfo &info : entries) {
        const QString childRemote = joinPath(remoteDir, info.filename);
        const QString childLocal = localDir + QLatin1Char('/') + info.filename;
        if (info.isSymlink())
            continue;   // 不跟随符号链接，防环
        if (info.isDirectory())
            collectDownloadRecursive(sftp, childRemote, childLocal, out, err);
        else
            out.append(DownloadTask{childRemote, childLocal});
    }
}

// 串行点火下一个下载任务。记账 total 先填 0，首个 transferProgress 会带回真实大小。
void SftpBrowserWidget::dispatchNextDownload()
{
    if (m_downloadQueue.isEmpty())
        return;
    const DownloadTask task = m_downloadQueue.dequeue();
    m_cancelPending = false;   // 新一轮传输：取消闸门复位
    m_activeDownloads.insert(task.remote, UploadProgress{0, 0});
    updateCancelButton();
    m_sftp->download(task.remote, task.local);
}

// 与 refreshUploadProgress 同构：按总字节聚合，状态栏显示批量进度。
void SftpBrowserWidget::refreshDownloadProgress()
{
    if (m_activeDownloads.isEmpty()) {
        if (m_downloadQueue.isEmpty())
            m_progress->setVisible(false);
        updateCancelButton();
        return;
    }
    qint64 done = 0, total = 0;
    for (const UploadProgress &p : std::as_const(m_activeDownloads)) {
        done += p.done;
        total += p.total;
    }
    // 队列中未点火的任务大小未知，只按已知部分算百分比（串行下最多一个在传）。
    m_progress->setVisible(true);
    m_progress->setMaximum(100);
    const int percent = total > 0 ? int(qMin<qint64>(100, done * 100 / total)) : 0;
    m_progress->setValue(percent);

    const int remaining = m_activeDownloads.size() + m_downloadQueue.size();
    if (remaining > 1) {
        setStatusText(tr("正在下载 %1 个文件 … %2%")
                              .arg(remaining).arg(percent));
    } else {
        setStatusText(tr("正在下载 %1 … %2%")
                              .arg(QFileInfo(m_activeDownloads.constBegin().key()).fileName())
                              .arg(percent));
    }
}

void SftpBrowserWidget::updateCancelButton()
{
    if (!m_cancelBtn)
        return;
    const bool busy = !m_activeUploads.isEmpty()
                      || !m_activeDownloads.isEmpty()
                      || !m_downloadQueue.isEmpty();
    // 在传集合排空 = 上一次取消已经收敛，闸门复位，下一轮传输的取消按钮可用。
    if (!busy)
        m_cancelPending = false;
    m_cancelBtn->setVisible(busy);
    // 取消已请求但还没停稳：置灰。工作线程只在分片边界看取消标志，正在飞的
    // 那片 4MB 写完才停，这中间按钮若还是可点的，用户会以为没生效而反复点。
    m_cancelBtn->setEnabled(!m_cancelPending);
    m_cancelBtn->setToolTip(m_cancelPending
                                ? tr("正在取消，等待当前分片结束…")
                                : tr("取消传输（已传部分保留，可断点续传）"));
}

// 取消全部在传传输。取消即"暂停"：下载/上传都支持断点续传，半成品文件
// 保留，重新传输自动从断点继续。
void SftpBrowserWidget::cancelTransfers()
{
    // 已经请求过、还没收敛：忽略重复点击（按钮此时也是置灰的）。
    if (m_cancelPending)
        return;
    // 下载：清空批量队列（整批取消），再取消当前在传文件。
    m_downloadQueue.clear();
    if (m_sftp)
        m_sftp->cancelTransfer();
    // 上传：逐文件取消（已入队的跳过、在传的分片边界停止）。
    if (m_uploader) {
        const QList<QString> ids = m_activeUploads.keys();
        for (const QString &fileId : ids)
            m_uploader->cancelUpload(fileId);
    }
    // 立刻给反馈：真正停下来要等分片边界（大文件分片下可达数秒），期间
    // 状态栏若还写着"正在上传 … 87%"，看着就像这一下没点着。
    const bool busy = !m_activeUploads.isEmpty() || !m_activeDownloads.isEmpty();
    if (busy) {
        m_cancelPending = true;
        setStatusText(tr("正在取消传输…"));
    }
    updateCancelButton();
}

// 右键菜单：与 Python 已连接分支逐字对齐（项目、顺序、分隔线）。
// 对应Python: cube-shell.py::treeRight elif self.isConnected 分支
void SftpBrowserWidget::showContextMenu(const QPoint &pos)
{
    if (!m_sftp)
        return;
    // 右键定位当前项，但不能破坏既有多选：setCurrentItem 的单参重载在
    // ExtendedSelection 下等价于 ClearAndSelect，会把批量选中的文件全部取消。
    // 点在已选中项上时用 NoUpdate 只移动 current、保留整个选区（与系统文件
    // 管理器一致）；点在未选中项上才重设选择为该项。
    QTreeWidgetItem *clicked = m_tree->itemAt(pos);
    if (clicked && clicked->data(0, kIsPlaceholderRole).toBool())
        return;
    if (clicked) {
        if (clicked->isSelected())
            m_tree->setCurrentItem(clicked, 0, QItemSelectionModel::NoUpdate);
        else
            m_tree->setCurrentItem(clicked);
    }

    QMenu menu(this);
    // 图标与文字的间距样式与 Python 侧一致
    menu.setStyleSheet(QStringLiteral(
        "QMenu::item { padding-left: 5px; }"
        "QMenu::icon { padding-right: 0px; }"));
    // macOS 默认隐藏菜单图标，需逐项 setIconVisibleInMenu(true)。
    // 对应Python: 每个 QAction 均调用 setIconVisibleInMenu(True)
    const auto addItem = [this, &menu](const QString &icon, const QString &text,
                                       void (SftpBrowserWidget::*slot)()) {
        QAction *act = menu.addAction(QIcon(icon), text, this, slot);
        act->setIconVisibleInMenu(true);
        return act;
    };
    addItem(QStringLiteral(":/Download.png"), tr("下载文件"), &SftpBrowserWidget::downloadSelected);
    addItem(QStringLiteral(":/Upload.png"), tr("上传文件"), &SftpBrowserWidget::uploadFiles);
    addItem(QStringLiteral(":/Edit.png"), tr("编辑文本"), &SftpBrowserWidget::editSelected);
    // 「执行脚本」只在右键点到的那个条目是脚本文件时出现（目录、普通文件、
    // ".." 行都不显示）。刻意不放在首项：首项是"下载文件"，用户对菜单第一项
    // 有肌肉记忆，把"真的会在远端跑起来"的动作摆那儿容易误触。
    if (clicked && !clicked->data(0, kIsUpRole).toBool()
        && looksLikeScript(clicked->text(0), clicked->data(0, kIsDirRole).toBool()))
        addItem(QStringLiteral(":/icons8-exec-48.png"), tr("执行脚本"),
                &SftpBrowserWidget::runScriptSelected);
    addItem(QStringLiteral(":/createdirector.png"), tr("创建文件夹"), &SftpBrowserWidget::mkdir);
    addItem(QStringLiteral(":/createfile.png"), tr("创建文件"), &SftpBrowserWidget::createFileHere);
    addItem(QStringLiteral(":/refresh.png"), tr("刷新"), &SftpBrowserWidget::refresh);
    addItem(QStringLiteral(":/permissions-48.png"), tr("权限"), &SftpBrowserWidget::showPermissions);
    menu.addSeparator();
    addItem(QStringLiteral(":/remove.png"), tr("删除"), &SftpBrowserWidget::removeSelected);
    addItem(QStringLiteral(":/icons-rename-48.png"), tr("重命名"), &SftpBrowserWidget::renameSelected);
    menu.addSeparator();
    addItem(QStringLiteral(":/icons-unzip-48.png"), tr("解压"), &SftpBrowserWidget::decompressSelected);
    addItem(QStringLiteral(":/icons8-zip-48.png"), tr("新建压缩"), &SftpBrowserWidget::compressSelected);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// 创建空文件。对应Python: cube-shell.py::createFile（sftp.file(path,'w') 空写）
void SftpBrowserWidget::createFileHere()
{
    if (!m_sftp)
        return;
    if (blockedByUnavailable(tr("创建文件")))
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件"), tr("文件名字:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = joinPath(currentDirectoryPath(), name.trimmed());
    SshError err;
    if (m_sftp->writeFile(path, QByteArray(), err))
        refresh();
    else
        QMessageBox::warning(this, tr("创建文件"), err.message);
}

// 权限对话框：用户/分组/其他 × R/W/X 九个复选框 → chmod。
// 对应Python: cube-shell.py::show_auth + Auth.ok_auth（ui/auth.py）
void SftpBrowserWidget::showPermissions()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !item || !m_sftp)
        return;
    const quint32 perm = item->data(0, kModeRole).toUInt();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("权限设置"));
    auto *grid = new QGridLayout;
    const QStringList rows = {tr("用户"), tr("分组"), tr("其他")};
    const QStringList cols = {QStringLiteral("R"), QStringLiteral("W"), QStringLiteral("X")};
    QVector<QCheckBox *> boxes;   // 顺序：rwxrwxrwx（高位在前）
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows[r], &dlg), r, 0);
        for (int c = 0; c < 3; ++c) {
            auto *box = new QCheckBox(cols[c], &dlg);
            const int bit = 8 - (r * 3 + c);   // 8..0
            box->setChecked(perm & (1u << bit));
            grid->addWidget(box, r, c + 1);
            boxes.append(box);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(grid);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // 对应Python: util.symbolic_to_octal 后 chmod
    int mode = 0;
    for (int i = 0; i < boxes.size(); ++i)
        if (boxes[i]->isChecked())
            mode |= 1 << (8 - i);
    SshError err;
    if (m_sftp->chmod(path, mode, &err)) {
        // 权限列属于父目录中的该条目；刷新目录自身只会更新子项，更新不了它。
        QTreeWidgetItem *parent = m_tree->currentItem()
            ? m_tree->currentItem()->parent() : nullptr;
        if (parent)
            loadChildren(parent, true);
        else
            loadPath(m_cwd, LoadReason::UserRequested, true);
    } else
        QMessageBox::warning(this, tr("权限设置"), err.message);
}

// 远端解压：按后缀选择 unzip/tar 在远端执行。
// 对应Python: cube-shell.py::unzip → core/compressor.py::DecompressThread
void SftpBrowserWidget::decompressSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !m_client)
        return;

    const QString targetDir = currentDirectoryPath();
    QString cmd;
    if (path.endsWith(QLatin1String(".zip")))
        cmd = QStringLiteral("unzip -o '%1' -d '%2'").arg(path, targetDir);
    else if (path.endsWith(QLatin1String(".tar.gz")) || path.endsWith(QLatin1String(".tgz")))
        cmd = QStringLiteral("tar -xzvf '%1' -C '%2'").arg(path, targetDir);
    else if (path.endsWith(QLatin1String(".tar")))
        cmd = QStringLiteral("tar -xvf '%1' -C '%2'").arg(path, targetDir);
    else {
        QMessageBox::warning(this, tr("解压"),
                             tr("不支持的压缩格式：%1").arg(path.section(QLatin1Char('/'), -1)));
        return;
    }

    setStatusText(tr("正在解压..."));
    SshClient *client = m_client.get();
    // 关停标志以共享指针捕获：析构置位后 runCommand 在下一轮读循环退出，
    // 且超时泄漏的线程读到的标志仍有效。
    auto shuttingDown = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, client, cmd, shuttingDown]() {
        const ExecResult res = CommandExecutor::runCommand(client, cmd, false, 120000, {},
                                                           shuttingDown.get());
        if (shuttingDown->load() || !self)
            return;   // 关停中：this 可能已析构，不再回投
        const bool ok = res.ok() && res.exitCode == 0;
        const QString msg = ok ? QString() : (res.errorMessage.isEmpty() ? res.stderrText : res.errorMessage);
        QMetaObject::invokeMethod(self, [this, ok, msg, shuttingDown]() {
            if (shuttingDown->load())
                return;
            setStatusText(ok ? tr("解压任务已完成") : tr("解压失败：%1").arg(msg));
            if (ok)
                refresh();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 「执行脚本」：拼出命令行交给本会话的终端执行（接线见 SshSessionTab）。
// 与解压/压缩走 CommandExecutor 不同——脚本可能跑很久、可能要交互、可能要
// Ctrl-C，这些只有在真终端里才成立，所以这里只负责把命令递出去。
void SftpBrowserWidget::runScriptSelected()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    const QString path = selectedRemotePath();
    if (!item || path.isEmpty())
        return;
    // 菜单项本就只在脚本上显示，这里再校验一次：防守住后续可能新增的入口
    // （快捷键、工具栏）把非脚本也送去执行。
    if (!looksLikeScript(item->text(0), item->data(0, kIsDirRole).toBool()))
        return;
    // 符号链接判定取权限列首字符：kModeRole 只存了 mode & 07777，类型位没留下；
    // 权限列由 permissionText() 生成（优先用远端 ls 的 longname 首列），
    // 是本面板对条目类型的既有事实来源。
    const bool isSymlink = item->text(3).startsWith(QLatin1Char('l'));
    const QString cmd = scriptRunCommand(path, item->data(0, kModeRole).toUInt(), isSymlink);
    emit terminalCommandRequested(cmd);
    setStatusText(tr("已在终端执行：%1").arg(cmd));
}

// 对应Python: cube-shell.py::rename_file
void SftpBrowserWidget::renameSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty())
        return;
    const QString oldName = path.section(QLatin1Char('/'), -1);
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("重命名"), tr("新名称："),
                                                  QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == oldName)
        return;
    const QString dir = path.section(QLatin1Char('/'), 0, -2);
    SshError err;
    if (m_sftp->rename(path, joinPath(dir.isEmpty() ? QStringLiteral("/") : dir, newName.trimmed()), &err)) {
        // 目录自身可能被重命名，不能再用旧路径刷新它；刷新其父目录。
        QTreeWidgetItem *parent = m_tree->currentItem()
            ? m_tree->currentItem()->parent() : nullptr;
        if (parent)
            loadChildren(parent, true);
        else
            loadPath(m_cwd, LoadReason::UserRequested, true);
    } else
        QMessageBox::warning(this, tr("重命名"), err.message);
}

// 远程文件编辑：读取 → TextEditor → 保存回写。
// 对应Python: cube-shell.py::edit_file + text_editor 保存回调
void SftpBrowserWidget::editSelected()
{
    const QString remote = selectedRemotePath();
    if (remote.isEmpty() || !m_sftp)
        return;
    QTreeWidgetItem *item = m_tree->currentItem();
    if (item && item->data(0, kIsDirRole).toBool())
        return;

    setStatusText(tr("正在打开 %1 …").arg(remote));
    SftpClient *sftp = m_sftp;
    const auto connection = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, sftp, remote, connection]() {
        SshError err;
        const QByteArray data = sftp->readFile(remote, err);
        const QString errMsg = err.message;
        if (connection->load() || !self)
            return;
        // 回主线程开编辑器（跨线程 → QueuedConnection）。
        QMetaObject::invokeMethod(self, [this, remote, data, errMsg, connection]() {
            if (connection->load())
                return;
            if (!errMsg.isEmpty()) {
                setStatusText(tr("打开失败：%1").arg(errMsg));
                return;
            }
            setStatusText(m_cwd);
            auto *editor = new TextEditor(nullptr);
            editor->setAttribute(Qt::WA_DeleteOnClose);
            editor->setFileLabel(remote);
            editor->setPlainText(QString::fromUtf8(data));
            QPointer<TextEditor> editorGuard(editor);
            connect(editor, &TextEditor::saveRequested, this,
                    [this, editorGuard, remote](const QString &content) {
                        SftpClient *sftp = m_sftp;
                        if (!sftp) {
                            if (editorGuard)
                                editorGuard->notifySaveFailed(tr("连接已断开"));
                            return;
                        }
                        const QByteArray bytes = content.toUtf8();
                        const auto connection = m_shuttingDown;
                        QPointer<SftpBrowserWidget> self(this);
                        QThread *w = QThread::create(
                            [this, self, editorGuard, sftp, remote, bytes, content, connection]() {
                                SshError err;
                                const bool ok = sftp->writeFile(remote, bytes, err);
                                const QString msg = err.message;
                                if (connection->load() || !self)
                                    return;
                                QMetaObject::invokeMethod(
                                    self,
                                    [this, editorGuard, remote, content, ok, msg, connection]() {
                                        if (connection->load())
                                            return;
                                        setStatusText(ok ? tr("已保存：%1").arg(remote)
                                                         : tr("保存失败：%1").arg(msg));
                                        // 编辑器可能在后台 I/O 期间被销毁，QPointer 防止悬空回调。
                                        // content 用于识别写回期间是否又产生了新修改。
                                        if (editorGuard) {
                                            if (ok)
                                                editorGuard->notifySaveSucceeded(content);
                                            else
                                                editorGuard->notifySaveFailed(msg);
                                        }
                                    },
                                    Qt::QueuedConnection);
                            });
                        startWorker(w);
                    });
            editor->show();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 远端压缩：tar/zip 命令在远端执行。对应Python: cube-shell.py::compress_file
void SftpBrowserWidget::compressSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !m_client)
        return;
    const QString baseName = path.section(QLatin1Char('/'), -1);

    CompressDialog dlg(this, baseName);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString dir = currentDirectoryPath();
    QString cmd;
    if (dlg.format() == QLatin1String(".zip"))
        cmd = QStringLiteral("cd '%1' && zip -r '%2' '%3'").arg(dir, dlg.fileName(), baseName);
    else
        cmd = QStringLiteral("tar -czf '%1' -C '%2' '%3'")
                  .arg(joinPath(dir, dlg.fileName()), dir, baseName);

    setStatusText(tr("正在压缩 %1 …").arg(baseName));
    SshClient *client = m_client.get();
    // 阻塞执行（无信号，线程安全的静态入口）；关停标志接线同 decompressSelected。
    auto shuttingDown = m_shuttingDown;
    QPointer<SftpBrowserWidget> self(this);
    QThread *worker = QThread::create([this, self, client, cmd, shuttingDown]() {
        const ExecResult res = CommandExecutor::runCommand(client, cmd, false, 120000, {},
                                                           shuttingDown.get());
        if (shuttingDown->load() || !self)
            return;   // 关停中：this 可能已析构，不再回投
        const bool ok = res.ok() && res.exitCode == 0;
        const QString msg = ok ? QString() : (res.errorMessage.isEmpty() ? res.stderrText : res.errorMessage);
        QMetaObject::invokeMethod(self, [this, ok, msg, shuttingDown]() {
            if (shuttingDown->load())
                return;
            setStatusText(ok ? tr("压缩完成") : tr("压缩失败：%1").arg(msg));
            if (ok)
                refresh();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

} // namespace cubeshell
