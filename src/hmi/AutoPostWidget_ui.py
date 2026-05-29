# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'AutoPostWidget.ui'
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
from PySide6.QtWidgets import (QApplication, QCheckBox, QHBoxLayout, QLabel,
    QLineEdit, QListWidgetItem, QPushButton, QSizePolicy,
    QSpacerItem, QSpinBox, QVBoxLayout, QWidget)

from hmi.SignedListWidget import SignedListWidget
import resources_rc

class Ui_AutoPostWidget(object):
    def setupUi(self, AutoPostWidget):
        if not AutoPostWidget.objectName():
            AutoPostWidget.setObjectName(u"AutoPostWidget")
        AutoPostWidget.resize(1409, 507)
        self.verticalLayout = QVBoxLayout(AutoPostWidget)
        self.verticalLayout.setObjectName(u"verticalLayout")
        self.postingParamsLayout = QHBoxLayout()
        self.postingParamsLayout.setObjectName(u"postingParamsLayout")
        self.compressPathLayout_2 = QHBoxLayout()
        self.compressPathLayout_2.setObjectName(u"compressPathLayout_2")
        self.compressPathLbl = QLabel(AutoPostWidget)
        self.compressPathLbl.setObjectName(u"compressPathLbl")

        self.compressPathLayout_2.addWidget(self.compressPathLbl)

        self.compressPathEdit = QLineEdit(AutoPostWidget)
        self.compressPathEdit.setObjectName(u"compressPathEdit")
        sizePolicy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        sizePolicy.setHorizontalStretch(0)
        sizePolicy.setVerticalStretch(0)
        sizePolicy.setHeightForWidth(self.compressPathEdit.sizePolicy().hasHeightForWidth())
        self.compressPathEdit.setSizePolicy(sizePolicy)
        self.compressPathEdit.setMinimumSize(QSize(200, 0))

        self.compressPathLayout_2.addWidget(self.compressPathEdit)

        self.compressPathButton = QPushButton(AutoPostWidget)
        self.compressPathButton.setObjectName(u"compressPathButton")
        self.compressPathButton.setMaximumSize(QSize(30, 16777215))

        self.compressPathLayout_2.addWidget(self.compressPathButton)


        self.postingParamsLayout.addLayout(self.compressPathLayout_2)

        self.horizontalSpacer_8 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postingParamsLayout.addItem(self.horizontalSpacer_8)

        self.rarLayout = QHBoxLayout()
        self.rarLayout.setObjectName(u"rarLayout")
        self.rarPathLayout = QHBoxLayout()
        self.rarPathLayout.setObjectName(u"rarPathLayout")
        self.rarLbl = QLabel(AutoPostWidget)
        self.rarLbl.setObjectName(u"rarLbl")

        self.rarPathLayout.addWidget(self.rarLbl)

        self.rarEdit = QLineEdit(AutoPostWidget)
        self.rarEdit.setObjectName(u"rarEdit")

        self.rarPathLayout.addWidget(self.rarEdit)

        self.rarPathButton = QPushButton(AutoPostWidget)
        self.rarPathButton.setObjectName(u"rarPathButton")
        self.rarPathButton.setMaximumSize(QSize(30, 16777215))

        self.rarPathLayout.addWidget(self.rarPathButton)


        self.rarLayout.addLayout(self.rarPathLayout)

        self.rarSizeLayout = QHBoxLayout()
        self.rarSizeLayout.setObjectName(u"rarSizeLayout")
        self.rarSizeLbl = QLabel(AutoPostWidget)
        self.rarSizeLbl.setObjectName(u"rarSizeLbl")

        self.rarSizeLayout.addWidget(self.rarSizeLbl)

        self.rarSizeEdit = QLineEdit(AutoPostWidget)
        self.rarSizeEdit.setObjectName(u"rarSizeEdit")
        self.rarSizeEdit.setMaximumSize(QSize(50, 16777215))

        self.rarSizeLayout.addWidget(self.rarSizeEdit)

        self.rarMaxCB = QCheckBox(AutoPostWidget)
        self.rarMaxCB.setObjectName(u"rarMaxCB")

        self.rarSizeLayout.addWidget(self.rarMaxCB)


        self.rarLayout.addLayout(self.rarSizeLayout)


        self.postingParamsLayout.addLayout(self.rarLayout)

        self.horizontalSpacer_11 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postingParamsLayout.addItem(self.horizontalSpacer_11)

        self.redundancyLayout = QHBoxLayout()
        self.redundancyLayout.setObjectName(u"redundancyLayout")
        self.redundancyLbl = QLabel(AutoPostWidget)
        self.redundancyLbl.setObjectName(u"redundancyLbl")

        self.redundancyLayout.addWidget(self.redundancyLbl)

        self.redundancySB = QSpinBox(AutoPostWidget)
        self.redundancySB.setObjectName(u"redundancySB")

        self.redundancyLayout.addWidget(self.redundancySB)


        self.postingParamsLayout.addLayout(self.redundancyLayout)


        self.verticalLayout.addLayout(self.postingParamsLayout)

        self.filesList = SignedListWidget(AutoPostWidget)
        self.filesList.setObjectName(u"filesList")

        self.verticalLayout.addWidget(self.filesList)

        self.autoDirLayout = QHBoxLayout()
        self.autoDirLayout.setObjectName(u"autoDirLayout")
        self.autoDirLbl = QLabel(AutoPostWidget)
        self.autoDirLbl.setObjectName(u"autoDirLbl")

        self.autoDirLayout.addWidget(self.autoDirLbl)

        self.autoDirEdit = QLineEdit(AutoPostWidget)
        self.autoDirEdit.setObjectName(u"autoDirEdit")
        sizePolicy.setHeightForWidth(self.autoDirEdit.sizePolicy().hasHeightForWidth())
        self.autoDirEdit.setSizePolicy(sizePolicy)
        self.autoDirEdit.setMinimumSize(QSize(200, 0))

        self.autoDirLayout.addWidget(self.autoDirEdit)

        self.autoDirButton = QPushButton(AutoPostWidget)
        self.autoDirButton.setObjectName(u"autoDirButton")
        self.autoDirButton.setMaximumSize(QSize(30, 16777215))

        self.autoDirLayout.addWidget(self.autoDirButton)

        self.addMonitoringFolderButton = QPushButton(AutoPostWidget)
        self.addMonitoringFolderButton.setObjectName(u"addMonitoringFolderButton")
        self.addMonitoringFolderButton.setMaximumSize(QSize(35, 16777215))
        icon = QIcon()
        icon.addFile(u":/icons/plus.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.addMonitoringFolderButton.setIcon(icon)

        self.autoDirLayout.addWidget(self.addMonitoringFolderButton)

        self.horizontalSpacer_5 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.autoDirLayout.addItem(self.horizontalSpacer_5)

        self.horizontalLayout = QHBoxLayout()
        self.horizontalLayout.setObjectName(u"horizontalLayout")
        self.extentionFilterLbl = QLabel(AutoPostWidget)
        self.extentionFilterLbl.setObjectName(u"extentionFilterLbl")

        self.horizontalLayout.addWidget(self.extentionFilterLbl)

        self.extensionFilterEdit = QLineEdit(AutoPostWidget)
        self.extensionFilterEdit.setObjectName(u"extensionFilterEdit")

        self.horizontalLayout.addWidget(self.extensionFilterEdit)

        self.dirAllowedCB = QCheckBox(AutoPostWidget)
        self.dirAllowedCB.setObjectName(u"dirAllowedCB")

        self.horizontalLayout.addWidget(self.dirAllowedCB)

        self.copyNfoCB = QCheckBox(AutoPostWidget)
        self.copyNfoCB.setObjectName(u"copyNfoCB")

        self.horizontalLayout.addWidget(self.copyNfoCB)


        self.autoDirLayout.addLayout(self.horizontalLayout)

        self.horizontalSpacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.autoDirLayout.addItem(self.horizontalSpacer)

        self.latestFilesFirstCB = QCheckBox(AutoPostWidget)
        self.latestFilesFirstCB.setObjectName(u"latestFilesFirstCB")

        self.autoDirLayout.addWidget(self.latestFilesFirstCB)

        self.scanAutoDirButton = QPushButton(AutoPostWidget)
        self.scanAutoDirButton.setObjectName(u"scanAutoDirButton")
        icon1 = QIcon()
        icon1.addFile(u":/icons/scanFolder.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.scanAutoDirButton.setIcon(icon1)

        self.autoDirLayout.addWidget(self.scanAutoDirButton)


        self.verticalLayout.addLayout(self.autoDirLayout)

        self.optionsLayout = QHBoxLayout()
        self.optionsLayout.setObjectName(u"optionsLayout")
        self.compressCB = QCheckBox(AutoPostWidget)
        self.compressCB.setObjectName(u"compressCB")

        self.optionsLayout.addWidget(self.compressCB)

        self.genNameLayout = QHBoxLayout()
        self.genNameLayout.setSpacing(1)
        self.genNameLayout.setObjectName(u"genNameLayout")
        self.genNameCB = QCheckBox(AutoPostWidget)
        self.genNameCB.setObjectName(u"genNameCB")

        self.genNameLayout.addWidget(self.genNameCB)

        self.nameLengthSB = QSpinBox(AutoPostWidget)
        self.nameLengthSB.setObjectName(u"nameLengthSB")

        self.genNameLayout.addWidget(self.nameLengthSB)


        self.optionsLayout.addLayout(self.genNameLayout)

        self.compressOptionsLayout = QHBoxLayout()
        self.compressOptionsLayout.setSpacing(1)
        self.compressOptionsLayout.setObjectName(u"compressOptionsLayout")
        self.genPassCB = QCheckBox(AutoPostWidget)
        self.genPassCB.setObjectName(u"genPassCB")

        self.compressOptionsLayout.addWidget(self.genPassCB)

        self.passLengthSB = QSpinBox(AutoPostWidget)
        self.passLengthSB.setObjectName(u"passLengthSB")

        self.compressOptionsLayout.addWidget(self.passLengthSB)


        self.optionsLayout.addLayout(self.compressOptionsLayout)

        self.keepRarCB = QCheckBox(AutoPostWidget)
        self.keepRarCB.setObjectName(u"keepRarCB")

        self.optionsLayout.addWidget(self.keepRarCB)

        self.horizontalSpacer_2 = QSpacerItem(40, 20, QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Minimum)

        self.optionsLayout.addItem(self.horizontalSpacer_2)

        self.par2CB = QCheckBox(AutoPostWidget)
        self.par2CB.setObjectName(u"par2CB")

        self.optionsLayout.addWidget(self.par2CB)

        self.horizontalSpacer_4 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.optionsLayout.addItem(self.horizontalSpacer_4)

        self.delFilesCB = QCheckBox(AutoPostWidget)
        self.delFilesCB.setObjectName(u"delFilesCB")

        self.optionsLayout.addWidget(self.delFilesCB)


        self.verticalLayout.addLayout(self.optionsLayout)

        self.postLayout = QHBoxLayout()
        self.postLayout.setObjectName(u"postLayout")
        self.horizontalSpacer_14 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postLayout.addItem(self.horizontalSpacer_14)

        self.monitorButton = QPushButton(AutoPostWidget)
        self.monitorButton.setObjectName(u"monitorButton")
        icon2 = QIcon()
        icon2.addFile(u":/icons/monitor.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.monitorButton.setIcon(icon2)

        self.postLayout.addWidget(self.monitorButton)

        self.startJobsCB = QCheckBox(AutoPostWidget)
        self.startJobsCB.setObjectName(u"startJobsCB")

        self.postLayout.addWidget(self.startJobsCB)

        self.postButton = QPushButton(AutoPostWidget)
        self.postButton.setObjectName(u"postButton")
        self.postButton.setToolTipDuration(-5)
        icon3 = QIcon()
        icon3.addFile(u":/icons/ngPost.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.postButton.setIcon(icon3)

        self.postLayout.addWidget(self.postButton)

        self.horizontalSpacer_15 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postLayout.addItem(self.horizontalSpacer_15)


        self.verticalLayout.addLayout(self.postLayout)


        self.retranslateUi(AutoPostWidget)

        QMetaObject.connectSlotsByName(AutoPostWidget)
    # setupUi

    def retranslateUi(self, AutoPostWidget):
        AutoPostWidget.setWindowTitle(QCoreApplication.translate("AutoPostWidget", u"Form", None))
        self.compressPathLbl.setText(QCoreApplication.translate("AutoPostWidget", u"compress path: ", None))
#if QT_CONFIG(tooltip)
        self.compressPathEdit.setToolTip(QCoreApplication.translate("AutoPostWidget", u"temporary folder where the archives and par2 will be created (it will be cleaned once the post is done)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.compressPathButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"select the temporary folder where the archives and par2 will be created (it will be cleaned once the post is done)", None))
#endif // QT_CONFIG(tooltip)
        self.compressPathButton.setText(QCoreApplication.translate("AutoPostWidget", u"...", None))
        self.rarLbl.setText(QCoreApplication.translate("AutoPostWidget", u"rar path: ", None))
#if QT_CONFIG(tooltip)
        self.rarEdit.setToolTip(QCoreApplication.translate("AutoPostWidget", u"full path of the rar executable", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarPathButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"select rar executable", None))
#endif // QT_CONFIG(tooltip)
        self.rarPathButton.setText(QCoreApplication.translate("AutoPostWidget", u"...", None))
        self.rarSizeLbl.setText(QCoreApplication.translate("AutoPostWidget", u"vol size:", None))
#if QT_CONFIG(tooltip)
        self.rarSizeEdit.setToolTip(QCoreApplication.translate("AutoPostWidget", u"to split the rar archive in several volumes (0 to don't split)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarMaxCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Check the configuration file and define or comment RAR_MAX to activate/deactivate this feature", None))
#endif // QT_CONFIG(tooltip)
        self.rarMaxCB.setText(QCoreApplication.translate("AutoPostWidget", u"limit rar number", None))
        self.redundancyLbl.setText(QCoreApplication.translate("AutoPostWidget", u"par2 redundancy (%): ", None))
        self.autoDirLbl.setText(QCoreApplication.translate("AutoPostWidget", u"<b>Auto Dir</b> path: ", None))
#if QT_CONFIG(tooltip)
        self.autoDirEdit.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Auto Dir path", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.autoDirButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"select the Auto Directory", None))
#endif // QT_CONFIG(tooltip)
        self.autoDirButton.setText(QCoreApplication.translate("AutoPostWidget", u"...", None))
#if QT_CONFIG(tooltip)
        self.addMonitoringFolderButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"add another folder to monitor (you must be in monitoring already)", None))
#endif // QT_CONFIG(tooltip)
        self.addMonitoringFolderButton.setText("")
        self.extentionFilterLbl.setText(QCoreApplication.translate("AutoPostWidget", u"Monitor extension filter: ", None))
#if QT_CONFIG(tooltip)
        self.extensionFilterEdit.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Add monitoring extension filter (coma separated, not dot, no space, ex: mkv,mp4,avi,iso,tar)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.dirAllowedCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"shall we post incoming folders", None))
#endif // QT_CONFIG(tooltip)
        self.dirAllowedCB.setText(QCoreApplication.translate("AutoPostWidget", u"post Folders", None))
#if QT_CONFIG(tooltip)
        self.copyNfoCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"When a posted file has a sibling .nfo (same name, different extension) next to it, include that .nfo in the same post (the lone .nfo is not posted separately)", None))
#endif // QT_CONFIG(tooltip)
        self.copyNfoCB.setText(QCoreApplication.translate("AutoPostWidget", u"copy nfo alongside other files", None))
#if QT_CONFIG(tooltip)
        self.latestFilesFirstCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Show latest files first (otherwise it'll be sorted by names)", None))
#endif // QT_CONFIG(tooltip)
        self.latestFilesFirstCB.setText(QCoreApplication.translate("AutoPostWidget", u"latest files first", None))
#if QT_CONFIG(tooltip)
        self.scanAutoDirButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Scan the Auto directory. Delete manually the files/folder you don't want to post (using DEL)", None))
#endif // QT_CONFIG(tooltip)
        self.scanAutoDirButton.setText(QCoreApplication.translate("AutoPostWidget", u" Scan", None))
        self.compressCB.setText(QCoreApplication.translate("AutoPostWidget", u"compress", None))
#if QT_CONFIG(tooltip)
        self.genNameCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"generate a random name for the archive", None))
#endif // QT_CONFIG(tooltip)
        self.genNameCB.setText(QCoreApplication.translate("AutoPostWidget", u"generate random name", None))
#if QT_CONFIG(tooltip)
        self.nameLengthSB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"length of the random archive name", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genPassCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"generate a random password for the archive or use the fixed one", None))
#endif // QT_CONFIG(tooltip)
        self.genPassCB.setText(QCoreApplication.translate("AutoPostWidget", u"generate random password", None))
#if QT_CONFIG(tooltip)
        self.passLengthSB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"length of the random archive password", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.keepRarCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"by default archives and par2 files are deleted uppon post success but you can choose to keep them", None))
#endif // QT_CONFIG(tooltip)
        self.keepRarCB.setText(QCoreApplication.translate("AutoPostWidget", u"keep archives", None))
#if QT_CONFIG(tooltip)
        self.par2CB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"generate par2", None))
#endif // QT_CONFIG(tooltip)
        self.par2CB.setText(QCoreApplication.translate("AutoPostWidget", u"generate par2", None))
#if QT_CONFIG(tooltip)
        self.delFilesCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"delete files/folders once they have been successfully posted (careful with that it's irreversible!!!)", None))
#endif // QT_CONFIG(tooltip)
        self.delFilesCB.setText(QCoreApplication.translate("AutoPostWidget", u"delete files once posted (only for Monitoring)", None))
        self.monitorButton.setText(QCoreApplication.translate("AutoPostWidget", u"Monitor Folder", None))
#if QT_CONFIG(tooltip)
        self.startJobsCB.setToolTip(QCoreApplication.translate("AutoPostWidget", u"start all posts when generating them", None))
#endif // QT_CONFIG(tooltip)
        self.startJobsCB.setText(QCoreApplication.translate("AutoPostWidget", u"start all Posts", None))
#if QT_CONFIG(tooltip)
        self.postButton.setToolTip(QCoreApplication.translate("AutoPostWidget", u"Generate Quick Posting Tabs for each file/folder", None))
#endif // QT_CONFIG(tooltip)
        self.postButton.setText(QCoreApplication.translate("AutoPostWidget", u"Generate Posts", None))
    # retranslateUi

