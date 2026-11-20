#pragma once

// file_browser_common.h — 本地/远程文件树面板共用的条目角色、筛选与排序逻辑。
//
// 本地与 SFTP 树均为五列多层级懒加载视图；两者共用同一套
// 条目角色、筛选和按兄弟节点排序的逻辑。树根首行可能是 ".." 返回上级条目。
// 排序与筛选都是纯内存操作：只处理已经加载的节点，不触发额外网络往返。
//
// 可测性约定与本工程一致：比较/匹配抽成纯函数（fileMatchesFilter /
// fileTreeSortLess），QTreeWidget 只是它们的一层薄壳（sortFileTree /
// applyFileFilter），真值表钉在 tests/file_browser_test.cpp。

#include <Qt>
#include <QtGlobal>
#include <QString>

class QIcon;
class QPalette;
class QTreeWidget;

namespace cubeshell {

// ---- 条目角色布局（两棵树共用同一套，便于共用函数直接读） -----------------
constexpr int kPathRole = Qt::UserRole;        // 完整路径（远端/本地）
constexpr int kIsDirRole = Qt::UserRole + 1;   // bool
constexpr int kModeRole = Qt::UserRole + 2;    // 权限位 quint32（仅 SFTP 树用）
constexpr int kIsUpRole = Qt::UserRole + 3;    // ".." 返回上级条目
constexpr int kSymlinkTargetRole = Qt::UserRole + 4; // 符号链接目标（仅 SFTP 树用）
constexpr int kSizeRole = Qt::UserRole + 5;    // 裸字节数 qint64（排序用，显示文本是格式化串）
constexpr int kMtimeRole = Qt::UserRole + 6;   // 裸修改时间 qint64 秒级 epoch（同上）
constexpr int kIsPlaceholderRole = Qt::UserRole + 7; // 未加载目录的占位子项
constexpr int kChildrenStateRole = Qt::UserRole + 20; // 本地/SFTP 目录加载状态

enum ChildrenState { ChildrenNotLoaded = 0, ChildrenLoading, ChildrenLoaded };

// ---- 筛选 ---------------------------------------------------------------
// 文件名子串匹配（忽略大小写）；空 needle 恒真。抽成纯函数便于单测。
bool fileMatchesFilter(const QString &name, const QString &needle);

// 递归处理所有已加载节点：不匹配项隐藏；目录自身不匹配但后代匹配时保留
// 该目录，避免把命中项的祖先藏掉。占位项与 ".." 不参与匹配计数。
// 返回名称自身匹配的条目数（不含仅为命中项保留的祖先目录）。
int applyFileFilter(QTreeWidget *tree, const QString &needle);

// ---- 排序 ---------------------------------------------------------------
// 排序所需的条目裸值快照（显示文本已格式化，不能直接比）。
struct FileSortKey {
    QString name;       // 文件名（不含路径）
    bool isDir = false;
    qint64 size = 0;    // 字节
    qint64 mtime = 0;   // 秒级 epoch
    QString perm;       // 权限串（"drwxr-xr-x"）
    QString owner;      // 所有者/组
};

// 树条目排序比较（less 语义，可直接喂 std::sort）：
//  - column: 0 名称（忽略前导点、忽略大小写，沿用原 ls 风格；此列目录恒在
//    文件前，升降序都保持）/ 1 大小 / 2 修改日期 / 3 权限 / 4 所有者；
//  - 非名称列全局混排（目录不提前），保证列值整列单调——目录分组会在
//    目录/文件分界处造成"顺序倒退"，用户看到的就是"排序是乱的"；
//  - 列值平局时以名称排序裁决，保证结果稳定；
//  - 不处理 ".." 条目：由 sortFileTree 先摘出、恒置顶（升降序都不动）。
bool fileTreeSortLess(int column, Qt::SortOrder order,
                      const FileSortKey &a, const FileSortKey &b);

// 递归对每组兄弟条目按 column/order 重排：顶层 ".." 条目恒置顶，其余按
// fileTreeSortLess 排序后原序插回（不销毁 item，选中态/展开态/隐藏态都保留）。
void sortFileTree(QTreeWidget *tree, int column, Qt::SortOrder order);

// ---- 列表字号 -------------------------------------------------------------
// 设置文件目录树的字号，不逐个修改 item：已有节点与后续懒加载节点都会继承
// 树控件字体。桌面平台使用磅值；OHOS 使用像素值并显式考虑屏幕密度，规避
// 平台插件 setPointSize 不随 devicePixelRatio 缩放的问题。非法字号直接忽略。
void setFileTreeFontSize(QTreeWidget *tree, int pointSize);

// ---- 筛选按钮图标 ---------------------------------------------------------
// 手绘放大镜（QPainter），随调色板着色，深浅色主题下都可见；
// 资源包里本没有搜索图标，免得为一个 16px 图标新增二进制资源。
QIcon fileFilterIcon(const QPalette &pal);

} // namespace cubeshell
