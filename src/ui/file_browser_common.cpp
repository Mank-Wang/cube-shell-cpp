#include "file_browser_common.h"

#include <QItemSelectionModel>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <functional>

namespace cubeshell {

// ---- 筛选 ---------------------------------------------------------------

bool fileMatchesFilter(const QString &name, const QString &needle)
{
    return needle.isEmpty() || name.contains(needle, Qt::CaseInsensitive);
}

int applyFileFilter(QTreeWidget *tree, const QString &needle)
{
    if (!tree)
        return 0;
    const QString n = needle.trimmed();
    // 返回值：该子树里名称自身匹配的条目数。父目录即使自身不匹配，只要
    // 后代有命中也保持可见，否则多层级树会把命中项连同祖先一起藏掉。
    const std::function<int(QTreeWidgetItem *)> applyToItem =
        [&](QTreeWidgetItem *item) -> int {
        const bool isUp = item->data(0, kIsUpRole).toBool();
        const bool isPlaceholder = item->data(0, kIsPlaceholderRole).toBool();
        int childMatches = 0;
        for (int i = 0; i < item->childCount(); ++i)
            childMatches += applyToItem(item->child(i));
        const bool isEntry = !isUp && !isPlaceholder;
        const bool selfMatch = isEntry && fileMatchesFilter(item->text(0), n);
        const bool show = n.isEmpty() || selfMatch || childMatches > 0;
        item->setHidden(!show);
        const int ownCount = n.isEmpty() ? int(isEntry) : int(selfMatch);
        return ownCount + childMatches;
    };
    int visible = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        const bool isUp = item->data(0, kIsUpRole).toBool();
        const int matches = applyToItem(item);
        if (n.isEmpty() && isUp)
            item->setHidden(false);
        else if (!n.isEmpty() && isUp)
            item->setHidden(true);
        if (!isUp)
            visible += matches;
    }
    return visible;
}

// ---- 排序 ---------------------------------------------------------------

// ls 风格名称键：忽略隐藏文件前导点、忽略大小写（与原 populate 预排序语义一致）。
static QString nameSortKey(const QString &name)
{
    QString k = name;
    while (k.startsWith(QLatin1Char('.')))
        k.remove(0, 1);
    return k.isEmpty() ? name.toLower() : k.toLower();
}

bool fileTreeSortLess(int column, Qt::SortOrder order,
                      const FileSortKey &a, const FileSortKey &b)
{
    // 目录恒在文件前仅适用于名称列（升降序都保持）。大小/日期等列若保持目录
    // 分组，目录与文件的分界处列值会"倒退"（目录组排到 2026-08-14，文件组
    // 又从 2026-08-08 重新开始），整列看起来是乱的——用户预期是全列单调，
    // 所以非名称列全局混排，保证列值从上到下严格单调。
    if (column == 0 && a.isDir != b.isDir)
        return a.isDir;

    // 三态比较再按升降序翻号（直接对 less 取反会破坏严格弱序：相等元素
    // comp(a,a) 必须为 false）。
    int cmp = 0;
    switch (column) {
    case 1:  // 大小：裸字节数
        cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
        break;
    case 2:  // 修改日期：裸 epoch 秒
        cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
        break;
    case 3:  // 权限串
        cmp = QString::compare(a.perm, b.perm);
        break;
    case 4:  // 所有者/组
        cmp = QString::compare(a.owner, b.owner, Qt::CaseInsensitive);
        break;
    default:
        break;
    }
    // 名称是第 0 列的主键，也是其余列的平局裁决键，保证排序结果稳定。
    if (cmp == 0)
        cmp = QString::compare(nameSortKey(a.name), nameSortKey(b.name));

    return order == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
}

// 从条目读出排序快照。
static FileSortKey sortKeyOf(const QTreeWidgetItem *item)
{
    FileSortKey k;
    k.name = item->text(0);
    k.isDir = item->data(0, kIsDirRole).toBool();
    k.size = item->data(0, kSizeRole).toLongLong();
    k.mtime = item->data(0, kMtimeRole).toLongLong();
    k.perm = item->text(3);
    k.owner = item->text(4);
    return k;
}

void sortFileTree(QTreeWidget *tree, int column, Qt::SortOrder order)
{
    if (!tree)
        return;
    // take/add 会让视图丢失展开和选中状态；节点本身不销毁，先记下指针，
    // 排序结束后统一恢复。
    QList<QTreeWidgetItem *> expandedItems, selectedItems;
    QTreeWidgetItem *currentItem = tree->currentItem();
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->isExpanded())
            expandedItems.append(*it);
        if ((*it)->isSelected())
            selectedItems.append(*it);
        ++it;
    }

    // 每组兄弟节点独立排序，不能把不同目录的条目混在一起。take/add 只改变
    // 顺序，不销毁节点，因此已加载子树保留。
    const std::function<void(QTreeWidgetItem *)> sortLevel =
        [&](QTreeWidgetItem *parent) {
        QList<QTreeWidgetItem *> ups, rest;
        const auto takeAt = [&](int index) -> QTreeWidgetItem * {
            return parent ? parent->takeChild(index) : tree->takeTopLevelItem(index);
        };
        const auto add = [&](QTreeWidgetItem *item) {
            if (parent)
                parent->addChild(item);
            else
                tree->addTopLevelItem(item);
        };
        while ((parent ? parent->childCount() : tree->topLevelItemCount()) > 0) {
            QTreeWidgetItem *item = takeAt(0);
            (item->data(0, kIsUpRole).toBool() ? ups : rest).append(item);
        }
        std::stable_sort(rest.begin(), rest.end(),
                         [column, order](const QTreeWidgetItem *x, const QTreeWidgetItem *y) {
                             return fileTreeSortLess(column, order, sortKeyOf(x), sortKeyOf(y));
                         });
        for (QTreeWidgetItem *item : std::as_const(ups))
            add(item);
        for (QTreeWidgetItem *item : std::as_const(rest)) {
            add(item);
            sortLevel(item);
        }
    };
    sortLevel(nullptr);
    for (QTreeWidgetItem *item : std::as_const(expandedItems))
        item->setExpanded(true);
    for (QTreeWidgetItem *item : std::as_const(selectedItems))
        item->setSelected(true);
    if (currentItem)
        tree->setCurrentItem(currentItem, 0, QItemSelectionModel::NoUpdate);
}

void setFileTreeFontSize(QTreeWidget *tree, int pointSize)
{
    if (!tree || pointSize <= 0)
        return;
    QFont font = tree->font(); // 保留系统字体族、字重等既有外观，只覆盖字号
#ifdef CUBESHELL_PLATFORM_OHOS
    // pt → px → 物理像素：96/72 是标准 DPI 换算，devicePixelRatioF 补上
    // 鸿蒙平台插件遗漏的屏幕密度。树控件会自行把行高和表头一起更新。
    font.setPixelSize(qRound(pointSize * 96.0 / 72.0 * tree->devicePixelRatioF()));
#else
    font.setPointSize(pointSize);
#endif
    tree->setFont(font);
}

// ---- 筛选按钮图标 ---------------------------------------------------------

QIcon fileFilterIcon(const QPalette &pal)
{
    // 2x 像素密度手绘，高分屏不糊；颜色取 ButtonText，深浅主题自适应。
    const int logical = 16;
    const qreal dpr = 2.0;
    QPixmap pm(int(logical * dpr), int(logical * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(pal.color(QPalette::ButtonText));
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(1.8, 1.8, 8.4, 8.4));   // 镜片
    p.drawLine(QPointF(9.0, 9.0), QPointF(14.2, 14.2)); // 手柄
    return QIcon(pm);
}

} // namespace cubeshell
