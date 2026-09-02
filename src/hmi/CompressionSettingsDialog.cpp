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

#include "NgPost.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>

CompressionSettingsDialog::CompressionSettingsDialog(NgPost *ngPost, QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::CompressionSettingsDialog)
    , _ngPost(ngPost)
{
    _ui->setupUi(this);

    _ui->compressPathEdit->setText(_ngPost->_tmpPath);
    _ui->rarEdit->setText(_ngPost->_rarPath);

    _ui->rarSizeEdit->setValidator(new QIntValidator(1, 1000000, _ui->rarSizeEdit));
    _ui->rarSizeEdit->setText(QString::number(_ngPost->_rarSize));
    _ui->rarMaxCB->setChecked(_ngPost->_useRarMax);
    _ui->keepRarDefaultCB->setChecked(_ngPost->_keepRarDefault);

    _ui->rarLengthSB->setRange(5, 50);
    _ui->rarLengthSB->setValue(static_cast<int>(_ngPost->_lengthPass));

    // Issue #48: the text first, so the toggled handler has something to push.
    _ui->rarPassEdit->setText(_ngPost->_rarPassFixed);
    _ui->rarPassCB->setChecked(!_ngPost->_rarPassFixed.isEmpty());
    onPassToggled(_ui->rarPassCB->isChecked());

    connect(_ui->compressPathButton, &QAbstractButton::clicked, this, &CompressionSettingsDialog::onCompressPathClicked);
    connect(_ui->rarPathButton,      &QAbstractButton::clicked, this, &CompressionSettingsDialog::onRarPathClicked);
    connect(_ui->genPass,            &QAbstractButton::clicked, this, &CompressionSettingsDialog::onGenPass);
    connect(_ui->rarPassCB,          &QAbstractButton::toggled, this, &CompressionSettingsDialog::onPassToggled);
    connect(_ui->buttonBox,          &QDialogButtonBox::accepted, this, &CompressionSettingsDialog::accept);
    connect(_ui->buttonBox,          &QDialogButtonBox::rejected, this, &QDialog::reject);
}

CompressionSettingsDialog::~CompressionSettingsDialog() { delete _ui; }

QString CompressionSettingsDialog::fixedPassword() const
{
    return _ui->rarPassCB->isChecked() ? _ui->rarPassEdit->text() : QString();
}

void CompressionSettingsDialog::accept()
{
    _ngPost->_tmpPath = _ui->compressPathEdit->text();
    _ngPost->_rarPath = _ui->rarEdit->text();

    _ngPost->_rarSize = 0;
    if (!_ui->rarSizeEdit->text().isEmpty())
    {
        bool ok  = true;
        uint val = _ui->rarSizeEdit->text().toUInt(&ok);
        if (ok)
            _ngPost->_rarSize = val;
    }

    _ngPost->_useRarMax  = _ui->rarMaxCB->isChecked();
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

void CompressionSettingsDialog::onRarPathClicked()
{
    QString path = QFileDialog::getOpenFileName(
                this,
                tr("Select rar executable"),
                QFileInfo(_ngPost->_rarPath).absolutePath()
                );

    if (!path.isEmpty())
    {
        QFileInfo fi(path);
        if (fi.isFile() && fi.isExecutable())
            _ui->rarEdit->setText(path);
        else
            _ngPost->error(tr("the selected file is not executable..."));
    }
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
