# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'VpnSettingsDialog.ui'
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
from PySide6.QtWidgets import (QAbstractButton, QApplication, QCheckBox, QComboBox,
    QDialog, QDialogButtonBox, QGroupBox, QHBoxLayout,
    QLabel, QPlainTextEdit, QPushButton, QSizePolicy,
    QSpacerItem, QVBoxLayout, QWidget)

class Ui_VpnSettingsDialog(object):
    def setupUi(self, VpnSettingsDialog):
        if not VpnSettingsDialog.objectName():
            VpnSettingsDialog.setObjectName(u"VpnSettingsDialog")
        VpnSettingsDialog.resize(620, 520)
        self.rootLayout = QVBoxLayout(VpnSettingsDialog)
        self.rootLayout.setObjectName(u"rootLayout")
        self.setupBox = QGroupBox(VpnSettingsDialog)
        self.setupBox.setObjectName(u"setupBox")
        self.setupLayout = QHBoxLayout(self.setupBox)
        self.setupLayout.setObjectName(u"setupLayout")
        self.setupLabel = QLabel(self.setupBox)
        self.setupLabel.setObjectName(u"setupLabel")
        self.setupLabel.setWordWrap(True)

        self.setupLayout.addWidget(self.setupLabel)

        self.setupSpacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.setupLayout.addItem(self.setupSpacer)

        self.installBtn = QPushButton(self.setupBox)
        self.installBtn.setObjectName(u"installBtn")

        self.setupLayout.addWidget(self.installBtn)

        self.uninstallBtn = QPushButton(self.setupBox)
        self.uninstallBtn.setObjectName(u"uninstallBtn")

        self.setupLayout.addWidget(self.uninstallBtn)


        self.rootLayout.addWidget(self.setupBox)

        self.profilesBox = QGroupBox(VpnSettingsDialog)
        self.profilesBox.setObjectName(u"profilesBox")
        self.profilesLayout = QHBoxLayout(self.profilesBox)
        self.profilesLayout.setObjectName(u"profilesLayout")
        self.profileLabel = QLabel(self.profilesBox)
        self.profileLabel.setObjectName(u"profileLabel")

        self.profilesLayout.addWidget(self.profileLabel)

        self.profileCB = QComboBox(self.profilesBox)
        self.profileCB.setObjectName(u"profileCB")
        sizePolicy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        sizePolicy.setHorizontalStretch(1)
        sizePolicy.setVerticalStretch(0)
        sizePolicy.setHeightForWidth(self.profileCB.sizePolicy().hasHeightForWidth())
        self.profileCB.setSizePolicy(sizePolicy)

        self.profilesLayout.addWidget(self.profileCB)

        self.newProfileBtn = QPushButton(self.profilesBox)
        self.newProfileBtn.setObjectName(u"newProfileBtn")

        self.profilesLayout.addWidget(self.newProfileBtn)

        self.editProfileBtn = QPushButton(self.profilesBox)
        self.editProfileBtn.setObjectName(u"editProfileBtn")

        self.profilesLayout.addWidget(self.editProfileBtn)

        self.deleteProfileBtn = QPushButton(self.profilesBox)
        self.deleteProfileBtn.setObjectName(u"deleteProfileBtn")

        self.profilesLayout.addWidget(self.deleteProfileBtn)


        self.rootLayout.addWidget(self.profilesBox)

        self.optionsBox = QGroupBox(VpnSettingsDialog)
        self.optionsBox.setObjectName(u"optionsBox")
        self.optionsLayout = QVBoxLayout(self.optionsBox)
        self.optionsLayout.setObjectName(u"optionsLayout")
        self.autoConnectCB = QCheckBox(self.optionsBox)
        self.autoConnectCB.setObjectName(u"autoConnectCB")

        self.optionsLayout.addWidget(self.autoConnectCB)


        self.rootLayout.addWidget(self.optionsBox)

        self.statusBox = QGroupBox(VpnSettingsDialog)
        self.statusBox.setObjectName(u"statusBox")
        self.statusLayout = QHBoxLayout(self.statusBox)
        self.statusLayout.setObjectName(u"statusLayout")
        self.stateLabel = QLabel(self.statusBox)
        self.stateLabel.setObjectName(u"stateLabel")

        self.statusLayout.addWidget(self.stateLabel)

        self.hspacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.statusLayout.addItem(self.hspacer)

        self.startBtn = QPushButton(self.statusBox)
        self.startBtn.setObjectName(u"startBtn")

        self.statusLayout.addWidget(self.startBtn)

        self.stopBtn = QPushButton(self.statusBox)
        self.stopBtn.setObjectName(u"stopBtn")

        self.statusLayout.addWidget(self.stopBtn)


        self.rootLayout.addWidget(self.statusBox)

        self.logBox = QGroupBox(VpnSettingsDialog)
        self.logBox.setObjectName(u"logBox")
        self.logLayout = QVBoxLayout(self.logBox)
        self.logLayout.setObjectName(u"logLayout")
        self.logView = QPlainTextEdit(self.logBox)
        self.logView.setObjectName(u"logView")
        self.logView.setReadOnly(True)

        self.logLayout.addWidget(self.logView)


        self.rootLayout.addWidget(self.logBox)

        self.buttonBox = QDialogButtonBox(VpnSettingsDialog)
        self.buttonBox.setObjectName(u"buttonBox")
        self.buttonBox.setStandardButtons(QDialogButtonBox.Close)

        self.rootLayout.addWidget(self.buttonBox)


        self.retranslateUi(VpnSettingsDialog)

        QMetaObject.connectSlotsByName(VpnSettingsDialog)
    # setupUi

    def retranslateUi(self, VpnSettingsDialog):
        VpnSettingsDialog.setWindowTitle(QCoreApplication.translate("VpnSettingsDialog", u"VPN tunnel", None))
        self.setupBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Setup", None))
        self.setupLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"VPN tunnel requires a one-time setup.", None))
        self.installBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Install...", None))
        self.uninstallBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Uninstall...", None))
        self.profilesBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Profiles", None))
        self.profileLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"Active profile:", None))
        self.newProfileBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"New...", None))
        self.editProfileBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Edit...", None))
        self.deleteProfileBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Delete", None))
        self.optionsBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Options", None))
        self.autoConnectCB.setText(QCoreApplication.translate("VpnSettingsDialog", u"Auto-connect VPN when a job starts", None))
#if QT_CONFIG(tooltip)
        self.autoConnectCB.setToolTip(QCoreApplication.translate("VpnSettingsDialog", u"When checked, ngPost will bring up the VPN tunnel automatically when any job starts, and disconnect 30 seconds after the queue is empty. Per-server \"Use VPN\" still controls which connections route through the tunnel.", None))
#endif // QT_CONFIG(tooltip)
        self.statusBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Status", None))
        self.stateLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"disabled", None))
        self.startBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Connect", None))
        self.stopBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Disconnect", None))
        self.logBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Log", None))
    # retranslateUi

