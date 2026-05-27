//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class MainWindow;
class NgPost;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(NgPost *ngPost, MainWindow *mainWindow,
                            QWidget *parent = nullptr);

private slots:
    void onApply();
    void onAccept();
    void onBrowseNzbPath();
    void onGeneratePoster();
    void onGenerateArchivePassword();
    void onFixedArchivePasswordToggled(bool checked);
    void onAddServer();
    void onRemoveServer();
    void onInstallVpn();
    void onUninstallVpn();
    void onNewVpnProfile();
    void onEditVpnProfile();
    void onDeleteVpnProfile();
    void onVpnProfilesChanged();

private:
    QWidget *_buildGeneralTab();
    QWidget *_buildServersTab();
    QWidget *_buildVpnTab();

    void _populateFromNgPost();
    void _populateServers();
    void _populateVpnProfiles();
    void _refreshVpnSetupUi();
    bool _applyToNgPost();

    QTableWidgetItem *_textItem(const QString &text,
                                Qt::ItemFlags extraFlags = Qt::ItemIsEditable) const;
    QTableWidgetItem *_checkItem(bool checked) const;
    QString _serverCellText(int row, int col) const;
    bool _serverCellChecked(int row, int col) const;

    NgPost           *_ngPost;
    MainWindow       *_mainWindow;
    bool              _populating;

    QLineEdit        *_fromEdit;
    QPushButton      *_genPosterBtn;
    QCheckBox        *_saveFromCB;
    QCheckBox        *_fixedArchivePassCB;
    QLineEdit        *_fixedArchivePassEdit;
    QSpinBox         *_fixedArchivePassLengthSB;
    QPushButton      *_genArchivePassBtn;
    QLineEdit        *_articleSizeEdit;
    QSpinBox         *_nbRetrySB;
    QSpinBox         *_threadSB;
    QCheckBox        *_obfuscateArticlesCB;
    QCheckBox        *_obfuscateFileNameCB;
    QCheckBox        *_autoCompressCB;
    QCheckBox        *_autoCloseCB;
    QCheckBox        *_keepNfoExtensionCB;
    QCheckBox        *_checkForUpdatesCB;
    QLineEdit        *_nzbPathEdit;
    QPushButton      *_nzbPathBtn;
    QComboBox        *_langCB;

    QTableWidget     *_serversTable;
    QPushButton      *_addServerBtn;
    QPushButton      *_removeServerBtn;

    QCheckBox        *_vpnRouteAllCB;
    QComboBox        *_vpnDefaultProfileCB;
    QPushButton      *_vpnInstallBtn;
    QPushButton      *_vpnUninstallBtn;
    QPushButton      *_vpnNewProfileBtn;
    QPushButton      *_vpnEditProfileBtn;
    QPushButton      *_vpnDeleteProfileBtn;

    QDialogButtonBox *_buttonBox;
};

#endif // SETTINGSDIALOG_H
