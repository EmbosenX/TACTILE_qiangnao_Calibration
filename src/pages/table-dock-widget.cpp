#include "table-dock-widget.h"

#include "config.h"
#include "csv/csv.h"
#include "dock-titlebar.h"
#include "logging.h"
#include "menu.h"
#include "message.h"
#include "scanner.h"
#include "utils.h"

#include <fmt/format.h>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QTextStream>
#include <QLabel>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

static void InitializeTableView(QTableView *view, QStandardItemModel *model)
{
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);

    view->horizontalHeader()->setDefaultSectionSize(50);
    view->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    view->verticalHeader()->setDefaultSectionSize(50);
    view->verticalHeader()->setDefaultAlignment(Qt::AlignCenter);
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    view->setModel(model);
}

static void FitBrainCoTableHeight(QTableView *view, int row_count)
{
    int height = view->horizontalHeader()->height();
    for (int row = 0; row < row_count; ++row) height += view->verticalHeader()->sectionSize(row);
    height += view->frameWidth() * 2;
    view->setFixedHeight(height);
}

struct BrainCoRegionSlice
{
    int         start;
    int         rows;
    const char *name;
};

static int braincoRegionSlicesForPart(int part, std::array<BrainCoRegionSlice, 3> *slices)
{
    if (!slices) return 0;
    if (part == 1) {
        *slices = { BrainCoRegionSlice{ 0, 8, "指尖" }, BrainCoRegionSlice{ 8, 3, "指根" },
                    BrainCoRegionSlice{} };
        return 2;
    }
    if (part >= 2 && part <= 4) {
        *slices = { BrainCoRegionSlice{ 0, 7, "指尖" }, BrainCoRegionSlice{ 7, 2, "指中" },
                    BrainCoRegionSlice{ 9, 2, "指根" } };
        return 3;
    }
    if (part == 5) {
        *slices = { BrainCoRegionSlice{ 0, 6, "指尖" }, BrainCoRegionSlice{ 6, 1, "指中" },
                    BrainCoRegionSlice{ 7, 2, "指根" } };
        return 3;
    }
    return 0;
}

static void writeAdMatrixCsvBlock(QTextStream& out, const touch::matrix<uint16_t>& data, int global_row_start,
                                  int row_count, uint32_t data_id, ProductModel product_type)
{
    out << QStringLiteral("行\\列");
    for (int col = 0; col < data.cols; ++col) out << ',' << col;
    out << '\n';

    for (int row = 0; row < row_count; ++row) {
        const int global_row = global_row_start + row;
        out << global_row;
        for (int col = 0; col < data.cols; ++col) {
            out << ',';
            bool valid = true;
            if (product_type == ProductModel::BRAINCO_GLOVE_V2 &&
                Utils::is_brainco_csv_template_size(data.rows, data.cols, data_id)) {
                valid = Utils::is_valid_cell_by_brainco_csv(global_row, col, data_id);
            }
            else if (Utils::is_csv_template_size(data.rows, data.cols, data_id)) {
                valid = Utils::is_valid_cell_by_csv(global_row, col, data_id);
            }
            if (valid) out << data(global_row, col);
        }
        out << '\n';
    }
}

TableDockWidget::TableDockWidget(const QString& title, QWidget *parent, Qt::WindowFlags flags)
    : DockWidget(title, parent, flags)
{
    view_  = new QTableView();
    model_ = new QStandardItemModel();
    InitializeTableView(view_, model_);
    layout_->addWidget(view_);
    layout_->setAlignment(Qt::AlignTop);
    for (size_t i = 0; i < brainco_views_.size(); ++i) {
        brainco_labels_[i] = new QLabel(QStringLiteral("区域 %1").arg(i + 1U));
        brainco_labels_[i]->setContentsMargins(0, i == 0 ? 0 : 4, 0, 0);
        brainco_views_[i] = new QTableView();
        brainco_models_[i] = new QStandardItemModel();
        InitializeTableView(brainco_views_[i], brainco_models_[i]);
        brainco_views_[i]->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        brainco_views_[i]->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        brainco_views_[i]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        brainco_labels_[i]->setVisible(false);
        brainco_views_[i]->setVisible(false);
        layout_->addWidget(brainco_labels_[i]);
        layout_->addWidget(brainco_views_[i]);
    }

    initContextMenu();
    initToolbar();
    updateIcons();

    timer_ = new QTimer(this);
    timer_->start(50); // ms
    connect(timer_, &QTimer::timeout, this, &TableDockWidget::refresh);

    connect(App(), &Scanner::StyleChanged, this, &TableDockWidget::updateIcons);
}

void TableDockWidget::setDataRange(int, const int max)
{
    factor_ = static_cast<float>(max) / 256.0f;

    view_->horizontalHeader()->setDefaultSectionSize(60);
    view_->verticalHeader()->setDefaultSectionSize(30);
    for (auto *view : brainco_views_) {
        view->horizontalHeader()->setDefaultSectionSize(60);
        view->verticalHeader()->setDefaultSectionSize(30);
    }
}

void TableDockWidget::product_model_changed(uint8_t idx)
{
    product_type = static_cast<ProductModel>(idx);
    const bool brainco = product_type == ProductModel::BRAINCO_GLOVE_V2;
    view_->setVisible(!brainco);
    if (brainco) {
        view_->setFixedHeight(0);
        view_->setMaximumHeight(0);
    }
    else {
        view_->setMinimumHeight(0);
        view_->setMaximumHeight(QWIDGETSIZE_MAX);
        view_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        for (size_t i = 0; i < brainco_views_.size(); ++i) {
            brainco_labels_[i]->setVisible(false);
            brainco_views_[i]->setVisible(false);
            brainco_labels_[i]->setMaximumHeight(QWIDGETSIZE_MAX);
            brainco_views_[i]->setFixedHeight(0);
        }
    }
}

void TableDockWidget::setColormap(const cmap::colormap_t cmap)
{
    if (cmap < cmap::colormap_t::AUTUMN || cmap > cmap::colormap_t::DEEPGREEN) return;

    cmap_ = cmap;
}

bool TableDockWidget::appendDisplayedAdCsv(const QString& file_path, AdTableValueKind kind, double value,
                                           QString* error)
{
    std::scoped_lock lock(mtx_);
    if (data_.rows <= 0 || data_.cols <= 0) {
        if (error) *error = QStringLiteral("当前没有可写入的 AD 数据");
        return false;
    }

    const QFileInfo info(file_path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        if (error) *error = QStringLiteral("无法创建目录：%1").arg(info.dir().absolutePath());
        return false;
    }

    const bool new_file = !info.exists() || info.size() == 0;
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法打开文件：%1").arg(file_path);
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    if (new_file) out << QChar(0xFEFF);

    const QString value_key = kind == AdTableValueKind::WeightGram ? QStringLiteral("砝码重量(g)")
                                                                   : QStringLiteral("压强(KPa)");
    out << value_key << ',' << QString::number(value, 'f', 2) << '\n';
    out << '\n';

    const bool brainco = product_type == ProductModel::BRAINCO_GLOVE_V2 &&
                         Utils::is_brainco_csv_template_size(data_.rows, data_.cols, data_id_);
    if (brainco) {
        const int part = static_cast<int>((data_id_ >> 4U) & 0x0FU);
        std::array<BrainCoRegionSlice, 3> slices{};
        const int region_count = braincoRegionSlicesForPart(part, &slices);
        if (region_count <= 0) {
            if (error) *error = QStringLiteral("当前部位区域配置无效，无法分区域写表");
            return false;
        }
        for (int region = 0; region < region_count; ++region) {
            const BrainCoRegionSlice slice = slices[region];
            if (slice.rows <= 0) continue;
            out << QStringLiteral("区域 %1（%2）")
                       .arg(region + 1)
                       .arg(QString::fromUtf8(slice.name))
                << '\n';
            writeAdMatrixCsvBlock(out, data_, slice.start, slice.rows, data_id_, product_type);
            if (region + 1 < region_count) out << '\n';
        }
    }
    else {
        writeAdMatrixCsvBlock(out, data_, 0, data_.rows, data_id_, product_type);
    }
    out << '\n';
    return true;
}

void TableDockWidget::setData(uint32_t id, const touch::matrix<uint16_t>& m)
{
    if (paused_) return;

    { // critical
        std::scoped_lock lock(mtx_);
        data_    = m;
        data_id_ = id;
    }
}

void TableDockWidget::refresh()
{
    if (!alive()) return;
    if (product_type == ProductModel::BRAINCO_GLOVE_V2)
        renderBrainCoRegions();
    else
        render(model_);
}

void TableDockWidget::renderBrainCoRegion(QStandardItemModel *model,
                                          const touch::matrix<uint16_t>& data,
                                          int global_row_offset, uint32_t part_id)
{
    if (data.rows != model->rowCount() || data.cols != model->columnCount()) {
        model->setRowCount(data.rows);
        model->setColumnCount(data.cols);
        QStringList horizontal;
        for (int col = 0; col < data.cols; ++col) horizontal << QStringLiteral("X%1").arg(col);
        QStringList vertical;
        for (int row = 0; row < data.rows; ++row)
            vertical << QStringLiteral("Y%1").arg(global_row_offset + row);
        model->setHorizontalHeaderLabels(horizontal);
        model->setVerticalHeaderLabels(vertical);
    }
    for (int row = 0; row < data.rows; ++row) {
        for (int col = 0; col < data.cols; ++col) {
            const auto index = model->index(row, col);
            QStandardItem *item = model->item(row, col);
            if (!item) {
                item = new QStandardItem();
                model->setItem(row, col, item);
            }
            item->setTextAlignment(Qt::AlignCenter);
            const bool valid = Utils::is_valid_cell_by_brainco_csv(global_row_offset + row, col, part_id);
            if (!valid) {
                model->setData(index, QVariant{});
                item->setBackground(Qt::black);
                item->setForeground(Qt::black);
                continue;
            }
            model->setData(index, data(row, col));
            if (data(row, col) == 0U) {
                item->setBackground(Qt::black);
                item->setForeground(QColor{ 128, 128, 128 });
                continue;
            }
            const auto gray = static_cast<uint8_t>(std::min<int>(data(row, col) / factor_, 255));
            const auto& [r, g, b, _] = lookup(cmap_, gray);
            const QColor background{ r, g, b };
            item->setBackground(background);
            item->setForeground(background.lightness() < 128 ? Qt::white : Qt::black);
        }
    }
}

void TableDockWidget::renderBrainCoRegions()
{
    std::scoped_lock lock(mtx_);
    if (data_.rows <= 0 || data_.cols <= 0) return;

    std::array<BrainCoRegionSlice, 3> slices{};
    const int part = static_cast<int>((data_id_ >> 4U) & 0x0FU);
    const int count = braincoRegionSlicesForPart(part, &slices);
    for (int region = 0; region < static_cast<int>(brainco_views_.size()); ++region) {
        const bool visible = region < count;
        brainco_labels_[region]->setVisible(visible);
        brainco_views_[region]->setVisible(visible);
        if (!visible) {
            brainco_labels_[region]->setMaximumHeight(0);
            brainco_views_[region]->setFixedHeight(0);
            continue;
        }
        brainco_labels_[region]->setMaximumHeight(QWIDGETSIZE_MAX);
        const BrainCoRegionSlice slice = slices[region];
        brainco_labels_[region]->setText(
            QStringLiteral("区域 %1（%2）").arg(region + 1).arg(QString::fromUtf8(slice.name)));
        touch::matrix<uint16_t> region_data(slice.rows, data_.cols, uint16_t{});
        for (int row = 0; row < slice.rows; ++row)
            for (int col = 0; col < data_.cols; ++col)
                region_data(row, col) = data_(slice.start + row, col);
        renderBrainCoRegion(brainco_models_[region], region_data, slice.start, data_id_);
        FitBrainCoTableHeight(brainco_views_[region], slice.rows);
    }
}

void TableDockWidget::render(QStandardItemModel *model)
{
    std::scoped_lock lock(mtx_);

    if (data_.rows <= 0 || data_.cols <= 0) return;

    if (data_.rows != model->rowCount() || data_.cols != model->columnCount()) {
        model->setColumnCount(data_.cols);
        model->setRowCount(data_.rows);

        for (int i = 0; i < model->rowCount(); ++i) {
            for (int j = 0; j < model->columnCount(); ++j) {
                const auto index = model->index(i, j);
                model->setData(index, 0);
                model->item(i, j)->setTextAlignment(Qt::AlignCenter);
            }
        }
    }

    for (int i = 0; i < model->rowCount(); ++i) {
        for (int j = 0; j < model->columnCount(); ++j) {
            const auto index = model->index(i, j);
            bool       valid = true;
            if (product_type == ProductModel::BRAINCO_GLOVE_V2 &&
                Utils::is_brainco_csv_template_size(data_.rows, data_.cols, data_id_)) {
                valid = Utils::is_valid_cell_by_brainco_csv(i, j, data_id_);
            }
            else if (Utils::is_csv_template_size(data_.rows, data_.cols, data_id_)) {
                valid = Utils::is_valid_cell_by_csv(i, j, data_id_);
            }
            if (!valid) {
                model->setData(index, QVariant{});
                model->item(i, j)->setBackground(Qt::black);
                model->item(i, j)->setForeground(Qt::black);
                continue;
            }
            model->setData(index, data_(i, j));
            const auto gray          = static_cast<uint8_t>(std::min<int>(data_(i, j) / factor_, 255));
            const auto& [r, g, b, _] = lookup(cmap_, gray);
            model->item(i, j)->setBackground(QColor{ r, g, b });
        }
    }
}

void TableDockWidget::save()
{
    std::scoped_lock lock(mtx_);

    if (data_.rows <= 0 || data_.cols <= 0) {
        Message::warning(nullptr, tr("No Data."));
        return;
    }

    auto file = config::record::csv::path + "/scanner_frame_" + QTIMESTAMP + ".csv";

    CSV csv_{};

    if (!csv_.open(file.toStdString())) {
        loge("failed to open file: {}", file.toStdString());
        Message::error(nullptr, tr("Failed to open the file."));
        return;
    }

    // COLS
    csv_.write("L|C,");
    for (int j = 0; j < data_.cols; j++) {
        csv_.write("C" + std::to_string(j) + ",");
    }
    csv_.write("\n");

    for (int i = 0; i < data_.rows; i++) {
        csv_.write("L" + std::to_string(i) + ",");
        for (int j = 0; j < data_.cols; j++) {
            csv_.write(std::to_string(data_(i, j)) + ",");
        }
        csv_.write("\n");
    }

    Message::success(nullptr, tr("Saved."));
}

QPixmap TableDockWidget::grabTable()
{
    const auto view  = new QTableView();
    const auto model = new QStandardItemModel();

    view->setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    view->verticalHeader()->setVisible(view_->verticalHeader()->isVisible());
    view->horizontalHeader()->setVisible(view_->horizontalHeader()->isVisible());

    InitializeTableView(view, model);
    render(model);

    QSize ts{ view_->verticalHeader()->width(), view_->horizontalHeader()->height() };
    for (int i = 0; i < model->rowCount(); i++) {
        ts.rheight() += view->verticalHeader()->sectionSize(i);
    }

    for (int i = 0; i < model->columnCount(); i++) {
        ts.rwidth() += view->horizontalHeader()->sectionSize(i);
    }

    view->resize(ts);

    QPixmap pixmap(view->size());
    view->render(&pixmap);

    delete view;

    return pixmap;
}

void TableDockWidget::saveImage(std::wstring path)
{
    const auto pixmap = grabTable();

    auto filename = QFileDialog::getSaveFileName(
        nullptr, tr("Save Image"), config::record::image::path + "/scanner_table_" + QTIMESTAMP,
        ".png(*.png);;.jpg(*.jpg)");

    if (filename.isEmpty()) return;

    (pixmap.save(filename)) ? Message::success(nullptr, tr("Saved."))
                            : Message::error(nullptr, tr("Failed to save."));
    if (pixmap.isNull()) {
        Message::error(nullptr, tr("No invalid data."));
        return;
    }
    emit grabbed(pixmap, path);
}

void TableDockWidget::hideHeaders(bool en)
{
    view_->horizontalHeader()->setVisible(!en);
    view_->verticalHeader()->setVisible(!en);
    for (auto *view : brainco_views_) {
        view->horizontalHeader()->setVisible(!en);
        view->verticalHeader()->setVisible(!en);
    }
}

void TableDockWidget::initContextMenu()
{
    // download
    save_action_ = new QAction(QIcon::fromTheme("download"), tr("Save as CSV"));
    connect(save_action_, &QAction::triggered, [this] { save(); });

    // image
    //    save_img_action_ = new QAction(tr("Save as Image"));
    //    connect(save_img_action_, &QAction::triggered, [this] { saveImage(); });

    // pause
    pause_action_ = new QAction(tr("Pause"));
    pause_action_->setCheckable(true);
    connect(pause_action_, &QAction::toggled, [this](auto&& paused) { paused_ = paused; });

    //
    hide_headers_action_ = new QAction(tr("Hide Headers"));
    hide_headers_action_->setCheckable(true);
    connect(hide_headers_action_, &QAction::toggled, this, &TableDockWidget::hideHeaders);

    // menu
    menu_ = new Menu(this);
    menu_->addAction(save_action_);
    if (save_img_action_) {
        menu_->addAction(save_img_action_);
    }
    menu_->addSeparator();
    menu_->addAction(pause_action_);
    menu_->addSeparator();
    menu_->addAction(hide_headers_action_);
}

void TableDockWidget::initToolbar()
{
    const auto pause = new QToolButton();
    pause->setDefaultAction(pause_action_);

    //    const auto save_image = new QToolButton();
    //    save_image->setDefaultAction(save_img_action_);

    const auto save = new QToolButton();
    save->setDefaultAction(save_action_);

    titlebar_->addWidget(pause, 0, Qt::AlignRight);
    //    titlebar_->addWidget(save_image, 0, Qt::AlignRight);
    titlebar_->addWidget(save, 0, Qt::AlignRight);
}

void TableDockWidget::updateIcons()
{
    const QString theme = config::theme == "dark" ? "light" : "dark";

    {
        QIcon icon{};
        icon.addPixmap(QPixmap(":/icons/pause-" + theme), QIcon::Normal, QIcon::Off);
        icon.addPixmap(QPixmap(":/icons/play-" + theme), QIcon::Normal, QIcon::On);
        pause_action_->setIcon(icon);
    }
}

void TableDockWidget::contextMenuEvent(QContextMenuEvent *event) { menu_->exec(event->globalPos()); }
