//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNPROFILEEDITDIALOG_H
#define VPNPROFILEEDITDIALOG_H

#include "vpn/VpnProfile.h"

#include <QDialog>

namespace Ui
{
class VpnProfileEditDialog;
}
class VpnManager;

//! Modal sub-dialog used to create or edit a single VpnProfile. On accept,
//! the profile metadata is committed back via VpnManager (add or update),
//! the selected .ovpn/.conf file is COPIED into <configDir>/vpn/, and the
//! credentials (if any) are stored in the system keychain.
class VpnProfileEditDialog : public QDialog
{
    Q_OBJECT
public:
    //! Edit an existing profile (oldName = current name, edit = its data) or
    //! create a new one (pass empty oldName and a default VpnProfile).
    explicit VpnProfileEditDialog(VpnManager *manager,
                                  QString const &oldName,
                                  VpnProfile const &edit,
                                  QWidget *parent = nullptr);
    ~VpnProfileEditDialog() override;

private slots:
    void onBrowse();
    void onAccept();
    void onBackendChanged(int idx);

private:
    //! Pre-fill credential fields from the keychain (best-effort, async).
    void _loadCredentialsFromKeychain();
    //! Save credentials to keychain. Returns true if stored cleanly; false
    //! if we had to fall back to inline (user accepted) or skip (user refused).
    //! Sets `wroteInlineFallback` if the inline path was chosen.
    bool _persistCredentials(QString const &profileName,
                             QString const &user, QString const &pass,
                             QString const &configFileAbsPath,
                             bool *wroteInlineFallback);

    Ui::VpnProfileEditDialog *_ui;
    VpnManager               *_manager;
    QString                   _oldName;        //!< empty when creating
    QString                   _stagingFilePath; //!< file picked but not yet copied
    bool                      _stagingFileChanged;
};

#endif // VPNPROFILEEDITDIALOG_H
