#pragma once

#include "layout_ops.h"
#include "layout_profile.h"
#include "ui_profile.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QPixmap>
#include <QTimer>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;

namespace rm_terminal
{

    class GlassButton;
    class PaneDetails;
    class UiProfileController;

    // 界面设置(效果图 10~13):一页搞定。最上面一排方案,中间是主界面信息页的真画面,左边是组件库。
    // - 点方案,画面马上换。
    // - 拖面板:放到另一块的边上就插进去(那一块让出地方),放到正中间就两块换位置;插不下时也换位置。
    //   从组件库拖来的插不下就替换掉那一块(换下来的回到组件库)。图传拖到「本车」上就放回本车里。
    //   画面里的面板拖回组件库就是拿掉。原来的地方由旁边补上。
    // - 拖两块中间的缝改大小,两边一起变。
    // - 每块右上角:齿轮调细项(在旁边弹出来),叉删掉。删错了点「撤销」或按 Ctrl+Z。
    // - 组件库只放画面里没有的,每个都是真样子的缩略图。
    // 改了马上存;比赛中主界面会把这一页关掉。
    class LayoutEditor final : public QFrame
    {
        Q_OBJECT
    public:
        explicit LayoutEditor(UiProfileController* controller, QWidget* parent = nullptr);

        // 打开时调用:从存盘的方案开始,清空撤销。
        void begin();
        // 主界面按 working() 排好以后整个窗口的样子;grid 是格子区在窗口里的位置。
        void setPreview(const QPixmap& window, const QRect& grid);
        // 画某个组件真样子的缩略图(主界面给)。
        void setPaneImageSource(std::function<QPixmap(PaneId, QSize)> source);
        // 主界面现在该按哪一份排(拖缝还没松手时是拖到一半的样子)。
        const UiScheme& working() const { return working_; }

        // 下面给测试和截图用,和鼠标走同一套判断。
        QRect canvasRect() const { return canvas_; }
        bool hasPreview() const { return !preview_.isNull(); }
        QRect unitToCanvas(const QRect& units) const;
        QRect tileRect(PaneId pane) const;
        QRect gearRect(PaneId pane) const;
        QRect closeRect(PaneId pane) const;
        bool canUndo() const { return !undo_.empty(); }
        void undo();
        QString toastText() const;
        bool detailsVisible() const;
        std::vector<PaneSlot> cells() const { return working_.cells; }
        QPushButton* schemeButton(const QString& id) const;

    Q_SIGNALS:
        void previewRequested();
        void doneRequested();

    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        void leaveEvent(QEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;

    private:
        enum class DragKind
        {
            None,
            Move,
            Add,
            Seam
        };
        // 松手以后会怎样:插进去、换位置、替换、放回「本车」里、拿掉(拖回组件库)、放不下。
        enum class DropAction
        {
            None,
            Insert,
            Swap,
            Replace,
            Embed,
            Remove
        };
        // 拖到另一块上时那一块分成的区:上、中、下三等分,左右两边各一条窄边。
        enum class DropZone
        {
            Top,
            Middle,
            Bottom,
            Left,
            Right
        };
        struct Drag
        {
            DragKind kind = DragKind::None;
            PaneId pane = PaneId::Map;
            QPoint press;
            QPoint mouse;
            bool moved = false;
            std::optional<PaneId> target;
            DropZone zone = DropZone::Middle;
            DropAction action = DropAction::None;
            std::optional<std::vector<PaneSlot>> result;
            Seam seam;
            int line = 0;
            std::vector<PaneSlot> before;
        };
        // 组件库里的一格:上面一行名字,下面缩略图。坐标相对组件库列表的左上角(没算滚动)。
        struct Tile
        {
            PaneId pane;
            QRect box;
            QRect image;
        };

        void reload();
        void commit(const std::vector<PaneSlot>& cells, const std::set<PaneId>& hidden);
        void pushUndo();
        void relayout();
        void placeStrip();
        void rebuildTabs();
        void rebuildTiles();
        void refreshTileImages();
        void showToast(const QString& text, bool with_undo);
        void openDetails(PaneId pane);
        void openAlerts();
        void placeDetails(const QRect& anchor, const QRect& avoid);
        void closeDetails();
        void removePane(PaneId pane);
        void putVideoBack();
        void resetScheme();
        void saveAsNew();
        void renameScheme();
        void deleteScheme();
        void exportBackup();
        void importBackup();
        void addOperator();
        void renameOperator();
        void removeOperator();
        void rebuildOperatorMenu();
        void rebuildMoreMenu();
        std::optional<QString> askName(const QString& title, const QString& label, const QString& text);
        bool confirm(const QString& title, const QString& text, const QString& yes);
        void updateDrag();
        void cancelDrag();
        // 鼠标指着的缝、齿轮或叉(画高亮、换光标)。
        void updateHover(const QPoint& pos);

        // 组件库里放缩略图的那一段(标题下面),和它现在滚到哪。
        QRect libraryView() const;
        QRect tileOnScreen(const Tile& tile) const;
        QSize paneRenderSize(PaneId pane) const;
        QString dropProblem() const;
        const PaneSlot* cell(PaneId pane) const;
        std::optional<PaneId> paneAt(const QPoint& pixel) const;
        QPointF unitPos(const QPoint& pixel) const;
        DropZone zoneAt(const QRect& box, const QPoint& pixel) const;
        QRect zoneRect(const QRect& box, DropZone zone) const;
        QRect seamRect(const Seam& seam) const;
        bool videoCell() const;
        bool hasDetails(PaneId pane) const;
        bool modified(const UiScheme& scheme) const;

        UiProfileController* controller_;
        Role role_;
        UiScheme working_;
        std::vector<RoleSetup> undo_;
        QPixmap preview_;
        QSize page_size_{1440, 900};
        QRect grid_in_window_{8, 128, 1424, 764};
        QRect grid_canvas_;
        QElapsedTimer tiles_painted_;
        int library_scroll_ = 0;
        int library_content_ = 0;
        std::optional<PaneId> hover_pane_;
        std::optional<QRect> hover_tool_;
        bool tips_visible_ = true;
        std::function<QPixmap(PaneId, QSize)> pane_image_;
        std::map<PaneId, QPixmap> tile_images_;
        std::vector<Tile> tiles_;
        std::optional<PaneId> last_removed_;
        Drag drag_;
        std::optional<Seam> hover_seam_;
        QRect canvas_;
        QRect library_;

        QWidget* top_ = nullptr;
        GlassButton* operator_button_ = nullptr;
        QMenu* operator_menu_ = nullptr;
        QLabel* seat_ = nullptr;
        GlassButton* undo_button_ = nullptr;
        GlassButton* reset_button_ = nullptr;
        GlassButton* more_button_ = nullptr;
        QMenu* more_menu_ = nullptr;
        GlassButton* done_button_ = nullptr;
        QWidget* strip_ = nullptr;
        QHBoxLayout* tabs_layout_ = nullptr;
        std::vector<QPushButton*> tabs_;
        GlassButton* save_new_ = nullptr;
        PaneDetails* details_ = nullptr;
        std::optional<PaneId> details_pane_;
        QFrame* toast_ = nullptr;
        QLabel* toast_label_ = nullptr;
        GlassButton* toast_undo_ = nullptr;
        QLabel* toast_key_ = nullptr;
        QTimer toast_timer_;
    };

} // namespace rm_terminal
