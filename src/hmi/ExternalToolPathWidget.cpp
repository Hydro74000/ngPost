// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#include "ExternalToolPathWidget.h"
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QSignalBlocker>

ExternalToolPathWidget::ExternalToolPathWidget(const QString &tool,
                                               externaltool::PathMode mode,
                                               const QString &path,
                                               const QString &prefix,
                                               QWidget *parent)
    : QWidget(parent)
    , _tool(tool)
    , _customPath(path)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _mode = new QComboBox(this);
    _mode->setObjectName(prefix + QStringLiteral("PathMode"));
    _mode->addItem(tr("Automatic (recommended)"), int(externaltool::PathMode::Automatic));
    _mode->addItem(tr("Custom…"), int(externaltool::PathMode::Custom));
    _mode->setCurrentIndex(_mode->findData(int(mode)));
    layout->addWidget(_mode);
    auto *row = new QHBoxLayout;
    _path = new QLineEdit(this);
    _path->setObjectName(prefix == QLatin1String("rar") ? QStringLiteral("rarEdit")
                                                        : prefix + QStringLiteral("Path"));
    _browse = new QPushButton(tr("Browse…"), this);
    row->addWidget(_path, 1);
    row->addWidget(_browse);
    layout->addLayout(row);
    _status = new QLabel(this);
    _status->setObjectName(prefix + QStringLiteral("PathStatus"));
    _status->setWordWrap(true);
    _status->setTextFormat(Qt::PlainText);
    layout->addWidget(_status);
    _details = new QToolButton(this);
    _details->setObjectName(prefix + QStringLiteral("PathDetails"));
    _details->setText(tr("Show path"));
    _details->setCheckable(true);
    layout->addWidget(_details, 0, Qt::AlignLeft);
    connect(_details, &QToolButton::toggled, this, [this] { refresh(); });
    connect(_mode, &QComboBox::currentIndexChanged, this, [this] {
        refresh();
        emit selectionChanged();
    });
    connect(_path, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (_updating)
            return;
        _customPath = text;
        const QSignalBlocker blocker(_mode);
        _mode->setCurrentIndex(_mode->findData(int(externaltool::PathMode::Custom)));
        refresh();
        emit selectionChanged();
    });
    connect(_browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getOpenFileName(this,
                                                            tr("Select executable"),
                                                            _path->text());
        if (!chosen.isEmpty())
            _path->setText(chosen);
    });
    refresh();
}
externaltool::PathMode ExternalToolPathWidget::mode() const
{
    return externaltool::PathMode(_mode->currentData().toInt());
}
QString ExternalToolPathWidget::customPath() const
{
    return mode() == externaltool::PathMode::Custom ? _customPath : QString();
}
void ExternalToolPathWidget::selectTool(const QString &tool)
{
    if (_tool != tool)
        _customPath.clear();
    const QSignalBlocker blocker(_mode);
    _mode->setCurrentIndex(_mode->findData(int(externaltool::PathMode::Automatic)));
    setTool(tool);
    emit selectionChanged();
}
void ExternalToolPathWidget::setTool(const QString &tool)
{
    _tool = tool;
    refresh();
}
void ExternalToolPathWidget::refresh()
{
    _updating = true;
    const auto resolved = externaltool::resolve(_tool, mode(), _customPath);
    const bool custom = mode() == externaltool::PathMode::Custom;
    // While the user types a custom path, the text already is that path:
    // setText() would move the cursor to the end and clear the undo history.
    if (_path->text() != resolved.path)
        _path->setText(resolved.path);
    _path->setReadOnly(!custom);
    // The button that shows the detected path goes with that path: without it,
    // a path shown earlier would stay on screen, empty, with no way to hide it.
    // Its checked state is kept for when a detected tool comes back.
    const bool detected = !custom && resolved.available();
    _path->setVisible(custom || (detected && _details->isChecked()));
    _browse->setVisible(custom);
    _details->setVisible(detected);
    _status->setToolTip(resolved.path);
    if (!resolved.available())
        _status->setText(
            custom ? tr("Executable unavailable. Choose an executable file or use Automatic.")
                   : tr("This tool is not included or installed. Install it, choose another tool, "
                        "or set a custom path. Posts requiring it cannot start."));
    else if (resolved.origin == externaltool::Origin::Bundled)
        _status->setText(tr("Included with ngPost · ready"));
    else if (resolved.origin == externaltool::Origin::System)
        _status->setText(tr("Found on this computer · ready"));
    else
        _status->setText(tr("Custom executable · ready"));
    _updating = false;
}
