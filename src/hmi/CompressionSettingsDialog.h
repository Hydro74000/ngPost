//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef COMPRESSIONSETTINGSDIALOG_H
#define COMPRESSIONSETTINGSDIALOG_H

#include <QDialog>

namespace Ui
{
class CompressionSettingsDialog;
}
class NgPost;

//! Where ngPost builds its archives, with what, and under which default
//! password. These are configuration values, not per post choices: they used to
//! be duplicated in every posting tab and in the auto post tab, all of them
//! writing to the same NgPost members, so the last active tab silently won.
//! They now live in one modal, and validating it writes the configuration file.
class CompressionSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CompressionSettingsDialog(NgPost *ngPost, QWidget *parent = nullptr);
    ~CompressionSettingsDialog() override;

    //! The fixed password as the dialog would apply it: empty when the box is
    //! unticked. Lets the caller refresh the tabs without reading the widgets.
    QString fixedPassword() const;

public slots:
    //! Applies to NgPost then saves the configuration file. A setting the user
    //! typed in a dialog should not need them to find the Save button too.
    void accept() override;

private slots:
    void onCompressPathClicked();
    void onRarPathClicked();
    void onGenPass();
    void onPassToggled(bool checked);

private:
    Ui::CompressionSettingsDialog *_ui;
    NgPost                        *_ngPost;
};

#endif // COMPRESSIONSETTINGSDIALOG_H
