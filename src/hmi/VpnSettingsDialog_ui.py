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
    QDialog, QDialogButtonBox, QFormLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QPlainTextEdit,
    QPushButton, QSizePolicy, QSpacerItem, QVBoxLayout,
    QWidget)

class Ui_VpnSettingsDialog(object):
    def setupUi(self, VpnSettingsDialog):
        if not VpnSettingsDialog.objectName():
            VpnSettingsDialog.setObjectName(u"VpnSettingsDialog")
        VpnSettingsDialog.resize(560, 440)
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

        self.configBox = QGroupBox(VpnSettingsDialog)
        self.configBox.setObjectName(u"configBox")
        self.formLayout = QFormLayout(self.configBox)
        self.formLayout.setObjectName(u"formLayout")
        self.backendLabel = QLabel(self.configBox)
        self.backendLabel.setObjectName(u"backendLabel")

        self.formLayout.setWidget(0, QFormLayout.ItemRole.LabelRole, self.backendLabel)

        self.backendCB = QComboBox(self.configBox)
        self.backendCB.addItem("")
        self.backendCB.addItem("")
        self.backendCB.setObjectName(u"backendCB")

        self.formLayout.setWidget(0, QFormLayout.ItemRole.FieldRole, self.backendCB)

        self.configLabel = QLabel(self.configBox)
        self.configLabel.setObjectName(u"configLabel")

        self.formLayout.setWidget(1, QFormLayout.ItemRole.LabelRole, self.configLabel)

        self.configRow = QHBoxLayout()
        self.configRow.setObjectName(u"configRow")
        self.configPathEdit = QLineEdit(self.configBox)
        self.configPathEdit.setObjectName(u"configPathEdit")

        self.configRow.addWidget(self.configPathEdit)

        self.browseBtn = QPushButton(self.configBox)
        self.browseBtn.setObjectName(u"browseBtn")

        self.configRow.addWidget(self.browseBtn)


        self.formLayout.setLayout(1, QFormLayout.ItemRole.FieldRole, self.configRow)

        self.enabledCB = QCheckBox(self.configBox)
        self.enabledCB.setObjectName(u"enabledCB")

        self.formLayout.setWidget(2, QFormLayout.ItemRole.SpanningRole, self.enabledCB)


        self.rootLayout.addWidget(self.configBox)

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
#if QT_CONFIG(tooltip)
        self.installBtn.setToolTip(QCoreApplication.translate("VpnSettingsDialog", u"Install the privileged VPN helper. After this you won't be prompted for a password on Connect / Disconnect.", None))
#endif // QT_CONFIG(tooltip)
        self.uninstallBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Uninstall...", None))
#if QT_CONFIG(tooltip)
        self.uninstallBtn.setToolTip(QCoreApplication.translate("VpnSettingsDialog", u"Remove the privileged VPN helper from the system.", None))
#endif // QT_CONFIG(tooltip)
        self.configBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Configuration", None))
        self.backendLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"Backend:", None))
        self.backendCB.setItemText(0, QCoreApplication.translate("VpnSettingsDialog", u"OpenVPN", None))
        self.backendCB.setItemText(1, QCoreApplication.translate("VpnSettingsDialog", u"WireGuard", None))

        self.configLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"Config file:", None))
        self.browseBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Browse...", None))
        self.enabledCB.setText(QCoreApplication.translate("VpnSettingsDialog", u"Auto-connect VPN when a job starts", None))
#if QT_CONFIG(tooltip)
        self.enabledCB.setToolTip(QCoreApplication.translate("VpnSettingsDialog", u"When checked, ngPost will bring up the VPN tunnel automatically when any job starts, and disconnect 30 seconds after the queue is empty. Per-server \"Use VPN\" still controls which connections route through the tunnel.", None))
#endif // QT_CONFIG(tooltip)
        self.statusBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Status", None))
        self.stateLabel.setText(QCoreApplication.translate("VpnSettingsDialog", u"disabled", None))
        self.startBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Connect", None))
        self.stopBtn.setText(QCoreApplication.translate("VpnSettingsDialog", u"Disconnect", None))
        self.logBox.setTitle(QCoreApplication.translate("VpnSettingsDialog", u"Log", None))
    # retranslateUi
