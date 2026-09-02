# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'CompressionSettingsDialog.ui'
##
## Created by: Qt User Interface Compiler version 6.11.1
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
from PySide6.QtWidgets import (QAbstractButton, QApplication, QCheckBox, QDialog,
    QDialogButtonBox, QFormLayout, QGroupBox, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QSizePolicy,
    QSpacerItem, QSpinBox, QVBoxLayout, QWidget)
import resources_rc

class Ui_CompressionSettingsDialog(object):
    def setupUi(self, CompressionSettingsDialog):
        if not CompressionSettingsDialog.objectName():
            CompressionSettingsDialog.setObjectName(u"CompressionSettingsDialog")
        CompressionSettingsDialog.resize(560, 280)
        self.rootLayout = QVBoxLayout(CompressionSettingsDialog)
        self.rootLayout.setObjectName(u"rootLayout")
        self.toolsBox = QGroupBox(CompressionSettingsDialog)
        self.toolsBox.setObjectName(u"toolsBox")
        self.toolsForm = QFormLayout(self.toolsBox)
        self.toolsForm.setObjectName(u"toolsForm")
        self.compressPathLbl = QLabel(self.toolsBox)
        self.compressPathLbl.setObjectName(u"compressPathLbl")

        self.toolsForm.setWidget(0, QFormLayout.ItemRole.LabelRole, self.compressPathLbl)

        self.compressPathLayout = QHBoxLayout()
        self.compressPathLayout.setObjectName(u"compressPathLayout")
        self.compressPathEdit = QLineEdit(self.toolsBox)
        self.compressPathEdit.setObjectName(u"compressPathEdit")

        self.compressPathLayout.addWidget(self.compressPathEdit)

        self.compressPathButton = QPushButton(self.toolsBox)
        self.compressPathButton.setObjectName(u"compressPathButton")
        self.compressPathButton.setMaximumSize(QSize(30, 16777215))

        self.compressPathLayout.addWidget(self.compressPathButton)


        self.toolsForm.setLayout(0, QFormLayout.ItemRole.FieldRole, self.compressPathLayout)

        self.rarLbl = QLabel(self.toolsBox)
        self.rarLbl.setObjectName(u"rarLbl")

        self.toolsForm.setWidget(1, QFormLayout.ItemRole.LabelRole, self.rarLbl)

        self.rarPathLayout = QHBoxLayout()
        self.rarPathLayout.setObjectName(u"rarPathLayout")
        self.rarEdit = QLineEdit(self.toolsBox)
        self.rarEdit.setObjectName(u"rarEdit")

        self.rarPathLayout.addWidget(self.rarEdit)

        self.rarPathButton = QPushButton(self.toolsBox)
        self.rarPathButton.setObjectName(u"rarPathButton")
        self.rarPathButton.setMaximumSize(QSize(30, 16777215))

        self.rarPathLayout.addWidget(self.rarPathButton)


        self.toolsForm.setLayout(1, QFormLayout.ItemRole.FieldRole, self.rarPathLayout)

        self.rarSizeLbl = QLabel(self.toolsBox)
        self.rarSizeLbl.setObjectName(u"rarSizeLbl")

        self.toolsForm.setWidget(2, QFormLayout.ItemRole.LabelRole, self.rarSizeLbl)

        self.rarSizeLayout = QHBoxLayout()
        self.rarSizeLayout.setObjectName(u"rarSizeLayout")
        self.rarSizeEdit = QLineEdit(self.toolsBox)
        self.rarSizeEdit.setObjectName(u"rarSizeEdit")
        self.rarSizeEdit.setMaximumSize(QSize(80, 16777215))

        self.rarSizeLayout.addWidget(self.rarSizeEdit)

        self.rarMaxCB = QCheckBox(self.toolsBox)
        self.rarMaxCB.setObjectName(u"rarMaxCB")

        self.rarSizeLayout.addWidget(self.rarMaxCB)

        self.rarSizeSpacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.rarSizeLayout.addItem(self.rarSizeSpacer)


        self.toolsForm.setLayout(2, QFormLayout.ItemRole.FieldRole, self.rarSizeLayout)

        self.keepRarDefaultCB = QCheckBox(self.toolsBox)
        self.keepRarDefaultCB.setObjectName(u"keepRarDefaultCB")

        self.toolsForm.setWidget(3, QFormLayout.ItemRole.FieldRole, self.keepRarDefaultCB)


        self.rootLayout.addWidget(self.toolsBox)

        self.passwordBox = QGroupBox(CompressionSettingsDialog)
        self.passwordBox.setObjectName(u"passwordBox")
        self.passwordLayout = QHBoxLayout(self.passwordBox)
        self.passwordLayout.setObjectName(u"passwordLayout")
        self.rarPassCB = QCheckBox(self.passwordBox)
        self.rarPassCB.setObjectName(u"rarPassCB")

        self.passwordLayout.addWidget(self.rarPassCB)

        self.rarPassEdit = QLineEdit(self.passwordBox)
        self.rarPassEdit.setObjectName(u"rarPassEdit")

        self.passwordLayout.addWidget(self.rarPassEdit)

        self.rarLengthSB = QSpinBox(self.passwordBox)
        self.rarLengthSB.setObjectName(u"rarLengthSB")
        self.rarLengthSB.setMaximumSize(QSize(60, 16777215))

        self.passwordLayout.addWidget(self.rarLengthSB)

        self.genPass = QPushButton(self.passwordBox)
        self.genPass.setObjectName(u"genPass")
        self.genPass.setMaximumSize(QSize(24, 24))
        icon = QIcon()
        icon.addFile(u":/icons/genKey.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.genPass.setIcon(icon)

        self.passwordLayout.addWidget(self.genPass)


        self.rootLayout.addWidget(self.passwordBox)

        self.verticalSpacer = QSpacerItem(20, 10, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding)

        self.rootLayout.addItem(self.verticalSpacer)

        self.buttonBox = QDialogButtonBox(CompressionSettingsDialog)
        self.buttonBox.setObjectName(u"buttonBox")
        self.buttonBox.setStandardButtons(QDialogButtonBox.Cancel|QDialogButtonBox.Save)

        self.rootLayout.addWidget(self.buttonBox)


        self.retranslateUi(CompressionSettingsDialog)

        QMetaObject.connectSlotsByName(CompressionSettingsDialog)
    # setupUi

    def retranslateUi(self, CompressionSettingsDialog):
        CompressionSettingsDialog.setWindowTitle(QCoreApplication.translate("CompressionSettingsDialog", u"Compression settings", None))
        self.toolsBox.setTitle(QCoreApplication.translate("CompressionSettingsDialog", u"Archives", None))
        self.compressPathLbl.setText(QCoreApplication.translate("CompressionSettingsDialog", u"Compress Path: ", None))
#if QT_CONFIG(tooltip)
        self.compressPathEdit.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"temporary folder where the archives and par2 will be created (it will be cleaned once the post is done)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.compressPathButton.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"select the temporary folder where the archives and par2 will be created (it will be cleaned once the post is done)", None))
#endif // QT_CONFIG(tooltip)
        self.compressPathButton.setText(QCoreApplication.translate("CompressionSettingsDialog", u"...", None))
        self.rarLbl.setText(QCoreApplication.translate("CompressionSettingsDialog", u"RAR Path: ", None))
#if QT_CONFIG(tooltip)
        self.rarEdit.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"full path of the rar executable", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarPathButton.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"select rar executable", None))
#endif // QT_CONFIG(tooltip)
        self.rarPathButton.setText(QCoreApplication.translate("CompressionSettingsDialog", u"...", None))
        self.rarSizeLbl.setText(QCoreApplication.translate("CompressionSettingsDialog", u"Vol size (MB):", None))
#if QT_CONFIG(tooltip)
        self.rarSizeEdit.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"to split the rar archive in several volumes (0 to don't split)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarMaxCB.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"cap the number of volumes: ngPost raises the volume size rather than making more of them (config RAR_MAX)", None))
#endif // QT_CONFIG(tooltip)
        self.rarMaxCB.setText(QCoreApplication.translate("CompressionSettingsDialog", u"Limit RAR Number", None))
#if QT_CONFIG(tooltip)
        self.keepRarDefaultCB.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"Archives and par2 are deleted once the post succeeds.\n"
"Ticked, they are left in the compression path folder instead.\n"
"This is the default every new post starts with; each post can still decide otherwise (config KEEP_RAR).", None))
#endif // QT_CONFIG(tooltip)
        self.keepRarDefaultCB.setText(QCoreApplication.translate("CompressionSettingsDialog", u"Keep the archives on disk", None))
        self.passwordBox.setTitle(QCoreApplication.translate("CompressionSettingsDialog", u"Archive password", None))
#if QT_CONFIG(tooltip)
        self.rarPassCB.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"use the same password for every post (config RAR_PASS)", None))
#endif // QT_CONFIG(tooltip)
        self.rarPassCB.setText(QCoreApplication.translate("CompressionSettingsDialog", u"Enable the default archive password", None))
#if QT_CONFIG(tooltip)
        self.rarPassEdit.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"password used for all the archives (it is also written in the header of the nzb file)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarLengthSB.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"length of the password drawn by the dice button (config LENGTH_PASS)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genPass.setToolTip(QCoreApplication.translate("CompressionSettingsDialog", u"generate random password", None))
#endif // QT_CONFIG(tooltip)
        self.genPass.setText("")
    # retranslateUi

