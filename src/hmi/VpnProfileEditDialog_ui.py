# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'VpnProfileEditDialog.ui'
##
## Created by: Qt User Interface Compiler version 6.11.0
##
## WARNING! All changes made in this file will be lost when recompiling UI file!
################################################################################

from PySide6.QtCore import (QCoreApplication, QDate, QDateTime, QLocale,
    QMetaObject, QObject, QPoint, QRect,
    QSize, QTime, QUrl, Qt)
from PySide6.QtGui import (QBrush, QColor, QConicalGradient, QCursor,
    QFont, QFontDatabase, QGradient, QIcon,
    QImage, QKeySequence, QLinearGradient, QPainter,
    QPalette, QPixmap, QRadialGradient, QTransform)
from PySide6.QtWidgets import (QAbstractButton, QApplication, QComboBox, QDialog,
    QDialogButtonBox, QFormLayout, QGroupBox, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QSizePolicy,
    QVBoxLayout, QWidget)

class Ui_VpnProfileEditDialog(object):
    def setupUi(self, VpnProfileEditDialog):
        if not VpnProfileEditDialog.objectName():
            VpnProfileEditDialog.setObjectName(u"VpnProfileEditDialog")
        VpnProfileEditDialog.resize(520, 340)
        self.rootLayout = QVBoxLayout(VpnProfileEditDialog)
        self.rootLayout.setObjectName(u"rootLayout")
        self.profileBox = QGroupBox(VpnProfileEditDialog)
        self.profileBox.setObjectName(u"profileBox")
        self.formLayout = QFormLayout(self.profileBox)
        self.formLayout.setObjectName(u"formLayout")
        self.nameLabel = QLabel(self.profileBox)
        self.nameLabel.setObjectName(u"nameLabel")

        self.formLayout.setWidget(0, QFormLayout.ItemRole.LabelRole, self.nameLabel)

        self.nameEdit = QLineEdit(self.profileBox)
        self.nameEdit.setObjectName(u"nameEdit")

        self.formLayout.setWidget(0, QFormLayout.ItemRole.FieldRole, self.nameEdit)

        self.backendLabel = QLabel(self.profileBox)
        self.backendLabel.setObjectName(u"backendLabel")

        self.formLayout.setWidget(1, QFormLayout.ItemRole.LabelRole, self.backendLabel)

        self.backendCB = QComboBox(self.profileBox)
        self.backendCB.addItem("")
        self.backendCB.addItem("")
        self.backendCB.setObjectName(u"backendCB")

        self.formLayout.setWidget(1, QFormLayout.ItemRole.FieldRole, self.backendCB)

        self.configLabel = QLabel(self.profileBox)
        self.configLabel.setObjectName(u"configLabel")

        self.formLayout.setWidget(2, QFormLayout.ItemRole.LabelRole, self.configLabel)

        self.configRow = QHBoxLayout()
        self.configRow.setObjectName(u"configRow")
        self.configPathEdit = QLineEdit(self.profileBox)
        self.configPathEdit.setObjectName(u"configPathEdit")
        self.configPathEdit.setReadOnly(True)

        self.configRow.addWidget(self.configPathEdit)

        self.browseBtn = QPushButton(self.profileBox)
        self.browseBtn.setObjectName(u"browseBtn")

        self.configRow.addWidget(self.browseBtn)


        self.formLayout.setLayout(2, QFormLayout.ItemRole.FieldRole, self.configRow)


        self.rootLayout.addWidget(self.profileBox)

        self.authBox = QGroupBox(VpnProfileEditDialog)
        self.authBox.setObjectName(u"authBox")
        self.authForm = QFormLayout(self.authBox)
        self.authForm.setObjectName(u"authForm")
        self.userLabel = QLabel(self.authBox)
        self.userLabel.setObjectName(u"userLabel")

        self.authForm.setWidget(0, QFormLayout.ItemRole.LabelRole, self.userLabel)

        self.userEdit = QLineEdit(self.authBox)
        self.userEdit.setObjectName(u"userEdit")

        self.authForm.setWidget(0, QFormLayout.ItemRole.FieldRole, self.userEdit)

        self.passLabel = QLabel(self.authBox)
        self.passLabel.setObjectName(u"passLabel")

        self.authForm.setWidget(1, QFormLayout.ItemRole.LabelRole, self.passLabel)

        self.passEdit = QLineEdit(self.authBox)
        self.passEdit.setObjectName(u"passEdit")
        self.passEdit.setEchoMode(QLineEdit.Password)

        self.authForm.setWidget(1, QFormLayout.ItemRole.FieldRole, self.passEdit)

        self.authNoteLabel = QLabel(self.authBox)
        self.authNoteLabel.setObjectName(u"authNoteLabel")
        self.authNoteLabel.setWordWrap(True)

        self.authForm.setWidget(2, QFormLayout.ItemRole.SpanningRole, self.authNoteLabel)


        self.rootLayout.addWidget(self.authBox)

        self.buttonBox = QDialogButtonBox(VpnProfileEditDialog)
        self.buttonBox.setObjectName(u"buttonBox")
        self.buttonBox.setStandardButtons(QDialogButtonBox.Cancel|QDialogButtonBox.Save)

        self.rootLayout.addWidget(self.buttonBox)


        self.retranslateUi(VpnProfileEditDialog)

        QMetaObject.connectSlotsByName(VpnProfileEditDialog)
    # setupUi

    def retranslateUi(self, VpnProfileEditDialog):
        VpnProfileEditDialog.setWindowTitle(QCoreApplication.translate("VpnProfileEditDialog", u"VPN profile", None))
        self.profileBox.setTitle(QCoreApplication.translate("VpnProfileEditDialog", u"Profile", None))
        self.nameLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Name:", None))
        self.nameEdit.setPlaceholderText(QCoreApplication.translate("VpnProfileEditDialog", u"Surfshark NL, Mullvad SE, ...", None))
        self.backendLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Backend:", None))
        self.backendCB.setItemText(0, QCoreApplication.translate("VpnProfileEditDialog", u"OpenVPN", None))
        self.backendCB.setItemText(1, QCoreApplication.translate("VpnProfileEditDialog", u"WireGuard", None))

        self.configLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Config file:", None))
        self.configPathEdit.setPlaceholderText(QCoreApplication.translate("VpnProfileEditDialog", u"Select a .ovpn or .conf to import", None))
        self.browseBtn.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Browse...", None))
        self.authBox.setTitle(QCoreApplication.translate("VpnProfileEditDialog", u"OpenVPN authentication (optional)", None))
        self.userLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Username:", None))
        self.passLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Password:", None))
        self.authNoteLabel.setText(QCoreApplication.translate("VpnProfileEditDialog", u"Credentials are stored in your system keychain (GNOME Keyring / KWallet / Windows Credential Vault / macOS Keychain). If the keychain is unavailable, ngPost will ask before falling back to writing credentials into the .ovpn copy (mode 600).", None))
    # retranslateUi

