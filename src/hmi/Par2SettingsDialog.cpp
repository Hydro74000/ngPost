// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#include "Par2SettingsDialog.h"
#include "ExternalToolPathWidget.h"
#include "NgPost.h"
#include "WrappedLabels.h"
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QEvent>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QtConcurrent>
#include <cmath>
#include <climits>
#include <limits>

namespace {
QWidget *sliderRow(QDoubleSpinBox *spin, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal, row);
    slider->setObjectName(spin->objectName() + QStringLiteral("Slider"));
    slider->setRange(0, 1000);
    layout->addWidget(slider, 1);
    layout->addWidget(spin);
    auto sync = [spin, slider] {
        QSignalBlocker blocker(slider);
        const double lo = std::log(qMax(0.000001, spin->minimum()));
        const double hi = std::log(spin->maximum());
        slider->setValue(qRound(1000 * (std::log(qMax(spin->value(), spin->minimum())) - lo) / (hi - lo)));
        slider->setEnabled(spin->isEnabled());
    };
    QObject::connect(slider, &QSlider::valueChanged, spin, [spin](int value) {
        const double lo = std::log(qMax(0.000001, spin->minimum()));
        const double hi = std::log(spin->maximum());
        double number = std::exp(lo + (hi - lo) * value / 1000.0);
        if (spin->objectName() == QStringLiteral("par2BlockBytes"))
            number = qMax(4.0, std::round(number / 4.0) * 4.0);
        spin->setValue(number);
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, row, sync);
    sync();
    return row;
}
QSpinBox *integer(QWidget *parent, const char *name, int minimum, int maximum)
{
    auto *spin = new QSpinBox(parent);
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(minimum, maximum);
    return spin;
}
template<class E> E choice(QComboBox *combo) { return static_cast<E>(combo->currentData().toInt()); }
template<class E> void select(QComboBox *combo, E value) { combo->setCurrentIndex(combo->findData(int(value))); }
void allow(QComboBox *combo, int value, bool enabled)
{
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    if (model) if (auto *item = model->item(combo->findData(value))) item->setEnabled(enabled);
}
QString bytes(qint64 size)
{
    if (size < 0) return Par2SettingsDialog::tr("indeterminate");
    if (size < 1048576) return QStringLiteral("%1 KiB").arg(size / 1024.0, 0, 'f', 2);
    return QStringLiteral("%1 MiB").arg(size / 1048576.0, 0, 'f', 2);
}
struct ScanResult { QVector<qint64> sizes; bool incomplete = false; };
}

Par2SettingsDialog::Par2SettingsDialog(NgPost *ngPost, const QFileInfoList &files,
                                     bool beforeCompression, bool percentageOverride, QWidget *parent)
    : QDialog(parent), _ngPost(ngPost), _cancelScan(std::make_shared<std::atomic_bool>(false)),
      _beforeCompression(beforeCompression), _percentageOverride(percentageOverride)
{
    setWindowTitle(tr("PAR2 Settings"));
    setObjectName(QStringLiteral("par2SettingsDialog"));
    resize(760, 520);
    auto *dialogLayout = new QVBoxLayout(this);
    // The entire form must scroll on small screens/high DPI. Scrolling only
    // Advanced leaves the fixed controls taller than a 720p desktop at 200%.
    auto *settingsScroll = new QScrollArea(this);
    settingsScroll->setObjectName(QStringLiteral("par2SettingsScroll"));
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    auto *contents = new QWidget(settingsScroll);
    settingsScroll->setWidget(contents);
    dialogLayout->addWidget(settingsScroll, 1);
    auto *outer = new QVBoxLayout(contents);
    outer->setSizeConstraint(QLayout::SetMinimumSize);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *intro = new QLabel(tr("Global settings for future posts. Each post can override the redundancy percentage; queued posts keep their settings."), this);
    intro->setWordWrap(true);
    outer->addWidget(intro);
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    outer->addLayout(form);
    _tool = new QComboBox(this);
    _tool->setObjectName(QStringLiteral("par2Tool"));
    _tool->addItem(tr("Automatic"), int(par2::Tool::Auto));
    _tool->addItem(QStringLiteral("ParPar"), int(par2::Tool::ParPar));
    _tool->addItem(QStringLiteral("par2cmdline"), int(par2::Tool::Par2cmdline));
    _tool->addItem(QStringLiteral("MultiPar"), int(par2::Tool::MultiPar));
#ifndef Q_OS_WIN
    allow(_tool, int(par2::Tool::MultiPar), false);
#endif
    select(_tool, ngPost->_par2Tool);
    form->addRow(tr("Tool:"), _tool);
    _toolPath = new ExternalToolPathWidget(par2::toolName(ngPost->_par2Tool),
                                           ngPost->_par2PathMode,
                                           ngPost->_par2PathConfig,
                                           QStringLiteral("par2"),
                                           this);
    _path = _toolPath->editor();
    form->addRow(tr("Path:"), _toolPath);
    _toolStatus = new QLabel(this);
    _toolStatus->setWordWrap(true);
    form->addRow(_toolStatus);
    _percentage = integer(this, "par2DefaultPct", 0, 100);
    _percentage->setSuffix(QStringLiteral(" %"));
    _percentage->setValue(int(ngPost->_par2PctDefault));
    auto *pctRow = new QHBoxLayout;
    auto *pctSlider = new QSlider(Qt::Horizontal, this);
    pctSlider->setObjectName(QStringLiteral("par2PercentageSlider"));
    pctSlider->setRange(0, 100);
    pctSlider->setValue(_percentage->value());
    connect(pctSlider, &QSlider::valueChanged, _percentage, &QSpinBox::setValue);
    connect(_percentage, &QSpinBox::valueChanged, pctSlider, &QSlider::setValue);
    pctRow->addWidget(pctSlider, 1);
    pctRow->addWidget(_percentage);
    form->addRow(tr("Default redundancy:"), pctRow);
    _volumes = new QComboBox(this);
    _volumes->setObjectName(QStringLiteral("par2VolumeMode"));
    _volumes->addItem(tr("Automatic"), int(par2::Volumes::Automatic));
    _volumes->addItem(tr("Limit to the largest source file"), int(par2::Volumes::LargestInput));
    _volumes->addItem(tr("Target maximum size"), int(par2::Volumes::Size));
    _volumes->addItem(tr("Number of recovery volumes"), int(par2::Volumes::Count));
    form->addRow(tr("Recovery volumes:"), _volumes);
    _volumeMiB = new QDoubleSpinBox(this);
    _volumeMiB->setObjectName(QStringLiteral("par2VolumeMiB"));
    // Six decimals of MiB can round an existing byte target down by one byte,
    // which loses a whole recovery block when MultiPar converts it to /lr.
    _volumeMiB->setDecimals(9);
    _volumeMiB->setRange(4.0 / 1048576, 1048576);
    _volumeMiB->setSuffix(QStringLiteral(" MiB"));
    form->addRow(tr("Target size:"), sliderRow(_volumeMiB, this));
    _distribution = new QComboBox(this);
    _distribution->setObjectName(QStringLiteral("par2Distribution"));
    _distribution->addItem(tr("Automatic"), int(par2::Distribution::Automatic));
    _distribution->addItem(tr("Equal"), int(par2::Distribution::Equal));
    _distribution->addItem(tr("Uniform"), int(par2::Distribution::Uniform));
    _distribution->addItem(tr("Powers of two"), int(par2::Distribution::PowersOfTwo));
    _distribution->addItem(tr("Decimal weights"), int(par2::Distribution::Decimal));
    form->addRow(tr("Distribution:"), _distribution);
    auto *volumeHelp = new QLabel(tr("Sizes are targets for recovery data. PAR2 metadata can make the final files larger. par2cmdline's native limit follows the largest source file."), this);
    volumeHelp->setWordWrap(true);
    form->addRow(volumeHelp);
    _form = form;

    _gpu = new QCheckBox(tr("Enable GPU acceleration"), this);
    _gpu->setObjectName(QStringLiteral("par2Gpu"));
    form->addRow(_gpu);
    _deviceRow = new QHBoxLayout;
    _device = new QComboBox(this);
    _device->setObjectName(QStringLiteral("par2GpuDevice"));
    _device->setEditable(true);
    // The tool's own wording for "let OpenCL pick": a bare ":" in the list said
    // nothing to anyone. The value still travels in the item data.
    _device->addItem(tr("Automatic (default device)"), QStringLiteral(":"));
    auto *find = _findGpuButton = new QPushButton(tr("Find OpenCL devices"), this);
    find->setObjectName(QStringLiteral("par2FindGpus"));
    _deviceRow->addWidget(_device, 1);
    _deviceRow->addWidget(find);
    form->addRow(tr("OpenCL device:"), _deviceRow);
    connect(find, &QPushButton::clicked, this, &Par2SettingsDialog::findGpus);

    auto *advancedButton = new QToolButton(this);
    advancedButton->setText(tr("Advanced"));
    advancedButton->setCheckable(true);
    advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedButton->setArrowType(Qt::RightArrow);
    outer->addWidget(advancedButton);
    auto *advanced = new QWidget(contents);
    auto *details = new QFormLayout(advanced);
    details->setRowWrapPolicy(QFormLayout::WrapLongRows);
    details->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(advanced);
    outer->addStretch();
    advanced->hide();
    connect(advancedButton, &QToolButton::toggled, this, [advanced, advancedButton](bool open) {
        advanced->setVisible(open);
        advancedButton->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    _blocks = new QComboBox(this);
    _blocks->setObjectName(QStringLiteral("par2BlockMode"));
    _blocks->addItem(tr("Automatic"), int(par2::Blocks::Automatic));
    _blocks->addItem(tr("Exact block size"), int(par2::Blocks::Size));
    _blocks->addItem(tr("Target source block count"), int(par2::Blocks::Count));
    details->addRow(tr("Blocks:"), _blocks);
    _blockBytes = new QDoubleSpinBox(this);
    _blockBytes->setObjectName(QStringLiteral("par2BlockBytes"));
    _blockBytes->setDecimals(0);
    _blockBytes->setRange(4, 2147483644.0);
    _blockBytes->setSingleStep(4);
    _blockBytes->setSuffix(tr(" bytes"));
    details->addRow(tr("Block size:"), sliderRow(_blockBytes, this));
    _blockCount = integer(this, "par2BlockCount", 1, 32768);
    details->addRow(tr("Source blocks:"), _blockCount);
    _volumeCount = integer(this, "par2VolumeCount", 1, 65535);
    details->addRow(tr("Recovery volumes:"), _volumeCount);
    _threads = integer(this, "par2Threads", 0, INT_MAX);
    _threads->setSpecialValueText(tr("Automatic"));
    details->addRow(tr("CPU threads:"), _threads);
    _memory = integer(this, "par2Memory", 0, 1048576);
    _memory->setSpecialValueText(tr("Automatic"));
    _memoryLabel = new QLabel(this);
    details->addRow(_memoryLabel, _memory);
    _custom = new QCheckBox(tr("Use custom arguments"), this);
    _custom->setObjectName(QStringLiteral("par2Custom"));
    details->addRow(_custom);
    _arguments = new QPlainTextEdit(this);
    _arguments->setObjectName(QStringLiteral("par2Arguments"));
    _arguments->setMaximumHeight(95);
    details->addRow(_arguments);
    auto *customHelp = new QLabel(tr("Custom arguments are kept verbatim; changing tools does not translate them. The post's redundancy overrides recognized redundancy arguments. Output and input paths are supplied by ngPost."), this);
    customHelp->setWordWrap(true);
    details->addRow(customHelp);
    _argumentsPreview = new QLabel(this);
    _argumentsPreview->setTextFormat(Qt::PlainText);
    _argumentsPreview->setWordWrap(true);
    _argumentsPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addRow(tr("Arguments:"), _argumentsPreview);

    _preview = new QLabel(tr("Prepare a post to display an estimate."), this);
    _preview->setObjectName(QStringLiteral("par2Estimate"));
    _preview->setWordWrap(true);
    _preview->setTextFormat(Qt::PlainText);
    dialogLayout->addWidget(_preview);
    _status = new QLabel(this);
    _status->setObjectName(QStringLiteral("par2Status"));
    _status->setWordWrap(true);
    dialogLayout->addWidget(_status);
    _buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    _buttons->button(QDialogButtonBox::Save)->setText(tr("Save"));
    _buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    dialogLayout->addWidget(_buttons);
    connect(_buttons, &QDialogButtonBox::accepted, this, &Par2SettingsDialog::accept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    _initial = par2::Settings::read(effectiveTool(), ngPost->_par2Args);
    select(_blocks, _initial.blocks);
    _blockBytes->setValue(_initial.blockBytes);
    _blockCount->setValue(_initial.blockCount);
    select(_volumes, _initial.volumes);
    _volumeMiB->setValue(_initial.volumeBytes / 1048576.0);
    _volumeCount->setValue(_initial.volumeCount);
    select(_distribution, _initial.distribution);
    _threads->setValue(_initial.threads);
    _memory->setValue(_initial.memory);
    _gpu->setChecked(_initial.gpu);
    if (!_initial.device.isEmpty()) {
        const int known = _device->findData(_initial.device);
        if (known >= 0)
            _device->setCurrentIndex(known);
        else
            _device->setCurrentText(_initial.device);
    }
    _custom->setChecked(_initial.custom);
    _arguments->setPlainText(ngPost->_par2Args);
    if (_initial.custom) advancedButton->setChecked(true);
    if (effectiveTool() == par2::Tool::MultiPar) {
        _threads->setMaximum(32);
        _memory->setMaximum(7);
    }
    if (effectiveTool() == par2::Tool::Par2cmdline)
        _volumeCount->setMaximum(31);
    for (auto *combo : {_blocks, _volumes, _distribution})
        connect(combo, &QComboBox::currentIndexChanged, this, &Par2SettingsDialog::changed);
    connect(_device, &QComboBox::currentTextChanged, this, &Par2SettingsDialog::changed);
    for (auto *spin : {_percentage, _blockCount, _volumeCount, _threads, _memory})
        connect(spin, &QSpinBox::valueChanged, this, &Par2SettingsDialog::changed);
    for (auto *spin : {_blockBytes, _volumeMiB})
        connect(spin, &QDoubleSpinBox::valueChanged, this, &Par2SettingsDialog::changed);
    connect(_gpu, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && !_loading && effectiveTool() == par2::Tool::ParPar)
            findGpus();
        changed();
    });
    connect(_custom, &QCheckBox::toggled, this, [this](bool custom) {
        if (custom && !_loading && _arguments->toPlainText().isEmpty())
            _arguments->setPlainText(par2::joinArguments(settings().arguments(uint(_percentage->value()))));
        if (!custom && !_loading && _gpu->isChecked() && _gpuScan == GpuScan::Unknown)
            findGpus();
        changed();
    });
    connect(_arguments, &QPlainTextEdit::textChanged, this, &Par2SettingsDialog::changed);
    connect(_tool, &QComboBox::currentIndexChanged, this, &Par2SettingsDialog::selectTool);
    const auto inspectPath = [this] {
        probeTool();
        QTimer::singleShot(0, this, [this] {
            if (_gpu->isChecked() && !_custom->isChecked() && _gpuScan == GpuScan::Unknown)
                findGpus();
        });
    };
    connect(_toolPath, &ExternalToolPathWidget::selectionChanged, this, [this, inspectPath] {
        if (_loading)
            return;
        resetGpuScan();
        changed();
        // Both checks run the executable, and any prefix of a path being typed
        // may be another one: typing waits for editingFinished. Browse, the
        // mode and the engine set the whole path at once (setText() clears
        // isModified()), so they are checked at once.
        if (!_path->isModified()) {
            inspectPath();
            return;
        }
        _probedPath.clear();
        _toolStatus->clear();
    });
    connect(_path, &QLineEdit::editingFinished, this, [this, inspectPath] {
        // Checked now: a later mode change that keeps this text is not typing.
        _path->setModified(false);
        inspectPath();
    });
    _loading = false;
    updateControls();
    probeTool();
    // A configuration saved on another machine can arrive with GPU work already
    // enabled: check the devices now rather than when the post fails.
    if (_gpu->isChecked() && effectiveTool() == par2::Tool::ParPar) findGpus();
    if (!files.isEmpty()) {
        _scanning = true;
        _preview->setText(tr("Reading source sizes…"));
        auto *watcher = new QFutureWatcher<ScanResult>(this);
        auto cancel = _cancelScan;
        connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher] {
            auto result = watcher->result();
            _scanning = false;
            _sizes = result.sizes;
            _scanIncomplete = result.incomplete;
            watcher->deleteLater();
            updatePreview();
        });
        watcher->setFuture(QtConcurrent::run([files, cancel] {
            ScanResult result;
            for (const auto &file : files) {
                if (*cancel) break;
                if (!file.exists() || !file.isReadable()) { result.incomplete = true; continue; }
                if (file.isFile()) result.sizes << file.size();
                else if (file.isDir()) {
                    QDirIterator it(file.absoluteFilePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                    QDirIterator::Subdirectories);
                    while (!*cancel && it.hasNext()) {
                        it.next();
                        const auto info = it.fileInfo();
                        if (!info.isReadable() || (info.isDir() && info.isSymLink())) result.incomplete = true;
                        else if (info.isFile()) result.sizes << info.size();
                    }
                } else result.incomplete = true;
            }
            return result;
        }));
    }
    _layoutReady = true;
    if (screen()) resize(size().boundedTo(screen()->availableGeometry().size() * 0.9));
}
bool Par2SettingsDialog::event(QEvent *event)
{
    const bool handled = QDialog::event(event);
    if (_layoutReady && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Show))
        fitWrappedLabels(*this);
    return handled;
}
Par2SettingsDialog::~Par2SettingsDialog()
{
    *_cancelScan = true;
    for (auto *process : findChildren<QProcess *>())
        if (process && process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(1000); }
}
par2::Tool Par2SettingsDialog::selectedTool() const { return choice<par2::Tool>(_tool); }
par2::Tool Par2SettingsDialog::effectiveTool() const
{
    return selectedTool() == par2::Tool::Auto ? par2::detectTool(_path->text()) : selectedTool();
}
par2::Settings Par2SettingsDialog::settings() const
{
    auto s = _initial;
    s.tool = effectiveTool();
    s.blocks = choice<par2::Blocks>(_blocks);
    s.blockBytes = qint64(_blockBytes->value());
    s.blockCount = _blockCount->value();
    s.volumes = choice<par2::Volumes>(_volumes);
    s.volumeBytes = qRound64(_volumeMiB->value() * 1048576.0);
    s.volumeCount = _volumeCount->value();
    s.distribution = choice<par2::Distribution>(_distribution);
    s.threads = _threads->value();
    s.memory = _memory->value();
    s.gpu = _gpu->isChecked();
    s.device = _device->currentText();
    if (_device->currentIndex() >= 0 && s.device == _device->itemText(_device->currentIndex())
        && !_device->currentData().toString().isEmpty()) s.device = _device->currentData().toString();
    s.custom = _custom->isChecked();
    s.originalArguments = _arguments->toPlainText();
    return s;
}
void Par2SettingsDialog::changed()
{
    if (_loading) return;
    _dirty = true;
    updateControls();
    updatePreview();
}
void Par2SettingsDialog::updateControls()
{
    const bool manual = _custom->isChecked();
    const auto tool = effectiveTool();
    const auto volumes = choice<par2::Volumes>(_volumes);
    const bool cmd = tool == par2::Tool::Par2cmdline, multi = tool == par2::Tool::MultiPar;
    _volumeMiB->setMaximum(multi && !manual ? 1999999999.0 / 1048576 : 1048576);
    _blocks->setItemText(_blocks->findData(int(par2::Blocks::Size)),
                        multi ? tr("Requested block size") : tr("Exact block size"));
    _blockBytes->setToolTip(multi ? tr("MultiPar may adjust the requested block size to the source files and block-count limit. It is not saved as PAR2_BLOCK_SIZE.") : QString());
    for (auto *combo : {_blocks, _volumes, _distribution}) combo->setEnabled(!manual);
    _arguments->setEnabled(manual);
    _blockBytes->setEnabled(!manual && choice<par2::Blocks>(_blocks) == par2::Blocks::Size);
    _blockCount->setEnabled(!manual && choice<par2::Blocks>(_blocks) == par2::Blocks::Count);
    _volumeMiB->setEnabled(!manual && !cmd && volumes == par2::Volumes::Size);
    _volumeCount->setEnabled(!manual && volumes == par2::Volumes::Count);
    // The sliders live beside their editors and follow the same availability.
    for (auto *spin : {_blockBytes, _volumeMiB})
        if (auto *slider = spin->parentWidget()->findChild<QSlider *>(spin->objectName() + QStringLiteral("Slider")))
            slider->setEnabled(spin->isEnabled());
    _threads->setEnabled(!manual && (_threadsSupported || _threads->value() != 0));
    _threads->setToolTip(_threadsSupported ? QString() : tr("This par2cmdline build does not support thread selection."));
    _memory->setEnabled(!manual);
    _memoryLabel->setText(multi ? tr("Memory (eighths of free RAM):") : tr("Memory (MiB):"));
    _gpu->setEnabled(!manual && (!cmd || _gpu->isChecked()));
    _device->setEnabled(!manual && tool == par2::Tool::ParPar && _gpu->isChecked());
    _findGpuButton->setEnabled(_device->isEnabled() && _gpuScan != GpuScan::Running);
    _form->setRowVisible(_gpu, !cmd || _gpu->isChecked());
    _form->setRowVisible(_deviceRow, tool == par2::Tool::ParPar);
    allow(_volumes, int(par2::Volumes::Size), !cmd);
    allow(_distribution, int(par2::Distribution::Equal), !cmd);
    allow(_distribution, int(par2::Distribution::Uniform), !multi && !(cmd && volumes == par2::Volumes::LargestInput));
    allow(_distribution, int(par2::Distribution::Decimal), multi && volumes != par2::Volumes::Count);
    allow(_distribution, int(par2::Distribution::PowersOfTwo), !(multi && volumes == par2::Volumes::Count));
    auto s = settings();
    QString error = s.validate();
    if (!manual && cmd && _gpu->isChecked()) error = tr("GPU acceleration is not supported by par2cmdline. Disable it before changing tools.");
    // ParPar stops with "Unable to obtain OpenCL device info" and writes no par2
    // at all, which aborts the post. MultiPar just falls back to the CPU.
    if (!manual && tool == par2::Tool::ParPar && _gpu->isChecked()) {
        if (_gpuScan == GpuScan::Unknown || _gpuScan == GpuScan::Running)
            error = tr("Checking OpenCL devices. Wait for the result or disable GPU acceleration.");
        else if (_gpuScan == GpuScan::Failed)
            error = tr("OpenCL could not be checked. Check the executable and OpenCL runtime, retry detection, or disable GPU acceleration.");
        else if (_gpuScan == GpuScan::None)
            error = tr("No OpenCL device was found: ParPar would fail and abort the post. "
                       "Disable GPU acceleration or install an OpenCL driver.");
    }
    if (!manual && !_threadsSupported && _threads->value() != 0)
        error = tr("This par2cmdline build does not support thread selection.");
    QFileInfo executable(_path->text());
    if (_toolPath->mode() == externaltool::PathMode::Custom
        && (!executable.isFile() || !executable.isExecutable()))
        error = tr("The selected executable is unavailable. Select an installed tool or Automatic.");
    _status->setText(error);
    _buttons->button(QDialogButtonBox::Save)->setEnabled(error.isEmpty());
    _argumentsPreview->setText(manual ? _arguments->toPlainText() : par2::joinArguments(s.arguments(uint(_percentage->value()))));
}
void Par2SettingsDialog::selectTool()
{
    if (_loading) return;
    resetGpuScan();
    _loading = true;
    const auto next = selectedTool();
    _toolPath->selectTool(par2::toolName(next));
    const bool multi = effectiveTool() == par2::Tool::MultiPar;
    // A unit switch is explicit; start at the new tool's native automatic
    // setting instead of treating MiB as eighths of available RAM.
    if (!_custom->isChecked()) _memory->setValue(0);
    _threads->setMaximum(multi ? 32 : INT_MAX);
    _memory->setMaximum(multi ? 7 : 1048576);
    // par2cmdline refuses more than 31 recovery files; the spin box clamps the
    // value it already holds rather than letting Save fail on it later.
    _volumeCount->setMaximum(effectiveTool() == par2::Tool::Par2cmdline ? 31 : 65535);
    _loading = false;
    changed();
    probeTool();
    if (_gpu->isChecked() && effectiveTool() == par2::Tool::ParPar)
        findGpus();
}
void Par2SettingsDialog::probeTool()
{
    const auto path = _path->text();
    const auto tool = effectiveTool();
    if (path == _probedPath && tool == _probedTool && _probe) return;
    _probedPath = path;
    _probedTool = tool;
    if (_probe) {
        _probe->disconnect(this);
        if (_probe->state() == QProcess::NotRunning) _probe->deleteLater();
        else {
            connect(_probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), _probe, &QObject::deleteLater);
            _probe->kill();
        }
    }
    _probe = new QProcess(this);
    _probe->setProcessChannelMode(QProcess::MergedChannels);
    _threadsSupported = true;
    if (!externaltool::executable(path)) {
        _toolStatus->clear();
        return;
    }
    _toolStatus->setText(tr("Checking executable…"));
    auto *process = _probe;
    connect(process, &QProcess::errorOccurred, this, [this, path, tool](QProcess::ProcessError) {
        if (_path->text() != path || effectiveTool() != tool) return;
        _toolStatus->setText(tr("The executable could not be checked."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, path, tool] {
        if (_path->text() != path || effectiveTool() != tool) return;
        const QString output = QString::fromLocal8Bit(process->readAll());
        _toolStatus->setText(output.trimmed().isEmpty() || process->exitStatus() != QProcess::NormalExit
            ? tr("The executable could not be checked.") : output.section(QLatin1Char('\n'), 0, 0).left(200));
        if (tool == par2::Tool::Par2cmdline) _threadsSupported = output.contains(QStringLiteral("-t"));
        updateControls();
    });
    QTimer::singleShot(5000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(path, tool == par2::Tool::MultiPar ? QStringList{} : QStringList{QStringLiteral("--help")});
}
void Par2SettingsDialog::resetGpuScan()
{
    if (_gpuProbe) {
        _gpuProbe->disconnect(this);
        if (_gpuProbe->state() == QProcess::NotRunning) _gpuProbe->deleteLater();
        else {
            connect(_gpuProbe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), _gpuProbe, &QObject::deleteLater);
            _gpuProbe->kill();
        }
        _gpuProbe = nullptr;
    }
    _gpuScan = GpuScan::Unknown;
    const auto selected = settings().device;
    const QSignalBlocker blocker(_device);
    while (_device->count() > 1) _device->removeItem(1);
    _device->setCurrentIndex(0);
    if (!selected.isEmpty() && selected != QStringLiteral(":")) _device->setCurrentText(selected);
    _device->setToolTip(QString());
}
void Par2SettingsDialog::findGpus()
{
    if (effectiveTool() != par2::Tool::ParPar || (_gpuProbe && _gpuProbe->state() != QProcess::NotRunning)) return;
    resetGpuScan();
    _gpuProbe = new QProcess(this);
    _gpuScan = GpuScan::Running;
    updateControls();
    auto *process = _gpuProbe;
    const auto path = _path->text();
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::errorOccurred, this, [this, path](QProcess::ProcessError) {
        if (_path->text() != path || effectiveTool() != par2::Tool::ParPar) return;
        _gpuScan = GpuScan::Failed;
        _toolStatus->setText(tr("The executable could not be checked."));
        updateControls();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, path] {
        if (_path->text() != path || effectiveTool() != par2::Tool::ParPar) return;
        const auto output = process->readAll();
        _device->setToolTip(QString::fromUtf8(output));
        if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
            _gpuScan = GpuScan::Failed;
            _toolStatus->setText(tr("The executable could not be checked."));
            updateControls();
            return;
        }
        // The text listing filters devices before numbering them. JSON keeps
        // original platform/device indices, including CPUs and unavailable GPUs.
        const auto selected = settings().device;
        const QSignalBlocker blocker(_device);
        const auto devices = par2::openClDevices(output, false);
        for (const auto &device : devices)
            if (_device->findData(device.id) < 0)
                _device->addItem(QString("%1 (%2)").arg(device.name, device.id), device.id);
        const int selection = _device->findData(selected);
        if (selection >= 0) _device->setCurrentIndex(selection);
        const int all = par2::openClDeviceCount(output);
        _gpuScan = all < 0 ? GpuScan::Failed : !all ? GpuScan::None
            : par2::openClDevices(output).isEmpty() ? GpuScan::NoGpu : GpuScan::Found;
        if (_gpuScan == GpuScan::Failed)
            _toolStatus->setText(tr("The executable could not be checked."));
        else if (_gpuScan == GpuScan::None)
            _toolStatus->setText(tr("No OpenCL device is installed on this machine."));
        else if (_gpuScan == GpuScan::NoGpu)
            _toolStatus->setText(tr("OpenCL devices are available without a physical GPU. CPU devices such as OpenCLOn12 can be selected."));
        else
            _toolStatus->setText(tr("GPU details are available in the device selector tooltip; a device name or platform:device ID can also be entered."));
        updateControls();
    });
    QTimer::singleShot(5000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(path, {QStringLiteral("--opencl-list"), QStringLiteral("--json")});
}
void Par2SettingsDialog::updatePreview()
{
    if (_scanning) { _preview->setText(tr("Reading source sizes…")); return; }
    if (_scanIncomplete) { _preview->setText(tr("Estimate unavailable: some source sizes could not be read.")); return; }
    if (_sizes.isEmpty()) { _preview->setText(tr("Prepare a post to display an estimate.")); return; }
    auto sizes = _sizes;
    if (_beforeCompression) {
        qint64 total = 0;
        for (auto size : sizes) {
            if (size < 0 || total > std::numeric_limits<qint64>::max() - size) {
                _preview->setText(tr("Estimate indeterminate for these arguments or block limits."));
                return;
            }
            total += size;
        }
        qint64 volumeMiB = _ngPost->_rarSize;
        if (_ngPost->_useRarMax && _ngPost->_rarMax > 0) {
            const qint64 sourceMiB = total / 1048576;
            if (volumeMiB == 0 || sourceMiB / volumeMiB > _ngPost->_rarMax)
                volumeMiB = sourceMiB / _ngPost->_rarMax + 1;
        }
        sizes.clear();
        const qint64 volume = volumeMiB > 0 ? volumeMiB * 1048576 : qMax<qint64>(1, total);
        // An unreasonable volume count is not useful as an interactive estimate.
        if (total / volume > 32768) { _preview->setText(tr("Estimate unavailable: too many source volumes.")); return; }
        while (total > 0) { sizes << qMin(total, volume); total -= sizes.last(); }
    }
    const auto e = settings().estimate(sizes, uint(_percentage->value()));
    if (!e.valid) {
        _preview->setText(tr("Estimate indeterminate for these arguments or block limits."));
        return;
    }
    QString text = _beforeCompression ? tr("Estimate before compression (source sizes and rounding):")
                                     : tr("Estimate for the current post:");
    text += QLatin1Char('\n') + tr("%1 source blocks of about %2; %3 recovery blocks, about %4.")
        .arg(e.sourceBlocks).arg(bytes(e.blockBytes)).arg(e.recoveryBlocks).arg(bytes(e.recoveryBytes));
    text += QLatin1Char('\n') + tr("Recovery volumes: %1; largest recovery data volume: %2 (metadata excluded).")
        .arg(e.volumeCount < 0 ? tr("indeterminate") : QString::number(e.volumeCount)).arg(bytes(e.largestRecoveryBytes));
    if (_percentageOverride) text += QLatin1Char('\n') + tr("This preview uses the global default; the current post has its own redundancy override.");
    _preview->setText(text);
}
void Par2SettingsDialog::accept()
{
    updateControls();
    if (!_buttons->button(QDialogButtonBox::Save)->isEnabled()) return;
    if (_dirty) {
        const auto s = settings();
        // Persist the engine whose arguments were generated, even when the
        // user initially selected automatic engine discovery.
        _ngPost->_par2Tool = externaltool::executable(_path->text()) ? effectiveTool()
                                                                     : selectedTool();
        _ngPost->_par2Path = _path->text();
        _ngPost->_par2PathMode = _toolPath->mode();
        _ngPost->_par2PathConfig = _toolPath->customPath();
        _ngPost->_par2PctDefault = uint(_percentage->value());
        _ngPost->_par2Args = s.custom ? _arguments->toPlainText()
                                    : par2::joinArguments(s.arguments(_ngPost->_par2PctDefault));
        if (!s.custom) _ngPost->_par2BlockSize = s.exactBlockBytes();
        emit _ngPost->par2DefaultsChanged();
        _ngPost->saveConfig();
    }
    QDialog::accept();
}
