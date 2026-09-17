//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "CompressionSettingsDialog.h"
#include "ui_CompressionSettingsDialog.h"

#include "ExternalToolPathWidget.h"
#include "NgPost.h"
#include "WrappedLabels.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>
#include <climits>
#include <QMessageBox>
#include <QEvent>
#include <QTimer>

CompressionSettingsDialog::CompressionSettingsDialog(NgPost *ngPost, QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::CompressionSettingsDialog)
    , _ngPost(ngPost)
{
    _ui->setupUi(this);

    _ui->compressPathEdit->setText(_ngPost->_tmpPath);
    // The tool rows are built here: ExternalToolPathWidget needs constructor
    // arguments, which a widget promoted in the .ui cannot receive.
    _tool = new QComboBox(this);
    _tool->setObjectName(QStringLiteral("rarTool"));
    _tool->addItem(QStringLiteral("RAR"), QStringLiteral("rar"));
    _tool->addItem(QStringLiteral("7-Zip"), QStringLiteral("7zip"));
    _tool->setCurrentIndex(_tool->findData(_ngPost->_rarTool));
    _ui->toolsForm->insertRow(1, tr("Tool:"), _tool);
    _toolPath = new ExternalToolPathWidget(_ngPost->_rarTool,
                                           _ngPost->_rarPathMode,
                                           _ngPost->_rarPathConfig,
                                           QStringLiteral("rar"),
                                           this);
    _ui->toolsForm->insertRow(2, tr("Path:"), _toolPath);
    _chosenTool = _ngPost->_rarTool;
    connect(_tool, &QComboBox::currentIndexChanged, this, [this] {
        _chosenTool = _tool->currentData().toString();
        _toolPath->selectTool(_chosenTool);
    });
    const auto updateSave = [this] {
        _ui->buttonBox->button(QDialogButtonBox::Save)
            ->setEnabled(_toolPath->mode() == externaltool::PathMode::Automatic
                         || externaltool::executable(_toolPath->customPath()));
    };
    connect(_toolPath, &ExternalToolPathWidget::selectionChanged, this, [this, updateSave] {
        // A custom executable named after an archiver decides the engine: the
        // other archiver's switches would make every compression fail. A name
        // that says neither gives the engine back to the one chosen, so a
        // prefix met while typing ("/opt/7z" of "/opt/7z-tools/winrar-cli") leaves
        // nothing behind.
        const QString archiver = externaltool::archiverForFile(_toolPath->customPath());
        const int named = _tool->findData(archiver.isEmpty() ? _chosenTool : archiver);
        if (named >= 0 && named != _tool->currentIndex()) {
            const QSignalBlocker blocker(_tool);
            _tool->setCurrentIndex(named);
            _toolPath->setTool(_tool->currentData().toString());
        }
        updateSave();
    });

    _ui->rarSizeEdit->setValidator(new QIntValidator(0, 1000000, _ui->rarSizeEdit));
    _ui->rarSizeEdit->setText(QString::number(_ngPost->_rarSize));
    _ui->rarMaxCB->setChecked(_ngPost->_useRarMax);
    _ui->rarMaxSB->setValue(static_cast<int>(qMin(_ngPost->_rarMax, uint(INT_MAX))));
    connect(_ui->rarMaxCB, &QCheckBox::toggled, this, &CompressionSettingsDialog::updateVolumeHelp);
    connect(_ui->rarSizeEdit, &QLineEdit::textChanged, this, &CompressionSettingsDialog::updateVolumeHelp);
    updateVolumeHelp();
    _ui->keepRarDefaultCB->setChecked(_ngPost->_keepRarDefault);

    _ui->rarLengthSB->setRange(5, 50);
    _ui->rarLengthSB->setValue(static_cast<int>(_ngPost->_lengthPass));

    // Issue #48: the text first, so the toggled handler has something to push.
    _ui->rarPassEdit->setText(_ngPost->_rarPassFixed);
    _ui->rarPassCB->setChecked(!_ngPost->_rarPassFixed.isEmpty());
    onPassToggled(_ui->rarPassCB->isChecked());

    connect(_ui->compressPathButton,
            &QAbstractButton::clicked,
            this,
            &CompressionSettingsDialog::onCompressPathClicked);
    connect(_ui->genPass,            &QAbstractButton::clicked, this, &CompressionSettingsDialog::onGenPass);
    connect(_ui->rarPassCB,          &QAbstractButton::toggled, this, &CompressionSettingsDialog::onPassToggled);
    connect(_ui->buttonBox,          &QDialogButtonBox::accepted, this, &CompressionSettingsDialog::accept);
    connect(_ui->buttonBox,          &QDialogButtonBox::rejected, this, &QDialog::reject);
    _ui->buttonBox->button(QDialogButtonBox::Save)->setText(tr("Save"));
    updateSave();
    _ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    _layoutReady = true;
    // The form gives spanning labels their real width during its first layout.
    // Fit once after that pass, before an old narrow size hint leaves a gap.
    QTimer::singleShot(0, this, &QWidget::adjustSize);
}

CompressionSettingsDialog::~CompressionSettingsDialog() { delete _ui; }

bool CompressionSettingsDialog::event(QEvent *event)
{
    const bool handled = QDialog::event(event);
    // The volume help and the tool status of the path row both wrap.
    if (_layoutReady
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize
            || event->type() == QEvent::Show))
        fitWrappedLabels(*this);
    return handled;
}

QString CompressionSettingsDialog::fixedPassword() const
{
    return _ui->rarPassCB->isChecked() ? _ui->rarPassEdit->text() : QString();
}

void CompressionSettingsDialog::accept()
{
    if (!_ui->rarSizeEdit->hasAcceptableInput())
    {
        QMessageBox::warning(this,
                             tr("Volume size"),
                             tr("\"%1\" is not a valid volume size, so nothing was saved.\n\n"
                                "Enter a whole number of MiB between 0 and 1000000 "
                                "(0 disables splitting only when the volume limit is off).")
                                     .arg(_ui->rarSizeEdit->text()));
        _ui->rarSizeEdit->setFocus();
        _ui->rarSizeEdit->selectAll();
        return;
    }
    if (_toolPath->mode() == externaltool::PathMode::Custom
        && !externaltool::executable(_toolPath->customPath()))
        return;
    _ngPost->_tmpPath = _ui->compressPathEdit->text();
    const QString nextTool = _tool->currentData().toString();
    // Engine-specific switches cannot be transferred to another archiver.
    if (_ngPost->_rarTool != nextTool)
        _ngPost->_rarArgs.clear();
    _ngPost->_rarTool = nextTool;
    _ngPost->_rarPathMode = _toolPath->mode();
    _ngPost->_rarPathConfig = _toolPath->customPath();
    _ngPost->_rarPath = _toolPath->editor()->text();

    _ngPost->_rarSize = 0;
    if (!_ui->rarSizeEdit->text().isEmpty())
    {
        bool ok  = true;
        uint val = _ui->rarSizeEdit->text().toUInt(&ok);
        if (ok)
            _ngPost->_rarSize = val;
    }

    _ngPost->_useRarMax  = _ui->rarMaxCB->isChecked();
    _ngPost->_rarMax = static_cast<uint>(_ui->rarMaxSB->value());
    // Only the default: _keepRar is what the post being prepared decided, and
    // its tab refreshes it before every job.
    _ngPost->_keepRarDefault = _ui->keepRarDefaultCB->isChecked();
    _ngPost->_lengthPass = static_cast<uint>(_ui->rarLengthSB->value());

    // A ticked box over an empty field is not a password: everything else in
    // ngPost reads an empty _rarPassFixed as "no default password", so the two
    // are kept in step rather than adding a second flag that could disagree.
    QString const pass = fixedPassword();
    _ngPost->_rarPassFixed = pass;
    if (!pass.isEmpty())
        _ngPost->_rarPass = pass;

    _ngPost->saveConfig();

    QDialog::accept();
}

void CompressionSettingsDialog::updateVolumeHelp()
{
    const bool limited = _ui->rarMaxCB->isChecked();
    _ui->rarMaxSB->setEnabled(limited);
    QString text;
    if (_ui->rarSizeEdit->text().toUInt() == 0)
        text = limited ? tr("The volume size is calculated automatically from the source size and the volume limit.")
                       : tr("The archive is not split into volumes.");
    else
        text = limited ? tr("If necessary, ngPost increases the volume size to meet this limit. The volume limit then takes priority over the size entered.")
                       : tr("The requested volume size is kept; the last volume may be smaller.");
    text += QStringLiteral("\n") + tr("1 MiB = 1,048,576 bytes. The calculation uses the source size, with rounding.");
    _ui->volumeHelpLabel->setText(text);
    _ui->rarMaxCB->setToolTip(text);
    _ui->rarMaxSB->setToolTip(text);
    _ui->rarSizeEdit->setToolTip(text);
}

void CompressionSettingsDialog::onCompressPathClicked()
{
    QString path = QFileDialog::getExistingDirectory(
                this,
                tr("Select a Folder"),
                _ui->compressPathEdit->text(),
                QFileDialog::ShowDirsOnly);

    if (!path.isEmpty())
        _ui->compressPathEdit->setText(path);
}


void CompressionSettingsDialog::onGenPass()
{
    _ui->rarPassEdit->setText(_ngPost->randomPass(static_cast<uint>(_ui->rarLengthSB->value())));
}

void CompressionSettingsDialog::onPassToggled(bool checked)
{
    _ui->rarPassEdit->setEnabled(checked);
    _ui->rarLengthSB->setEnabled(checked);
    _ui->genPass->setEnabled(checked);
}
