# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'PostingWidget.ui'
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
from PySide6.QtWidgets import (QApplication, QCheckBox, QFrame, QHBoxLayout,
    QLabel, QLineEdit, QListWidgetItem, QPushButton,
    QSizePolicy, QSpacerItem, QSpinBox, QVBoxLayout,
    QWidget)

from hmi.SignedListWidget import SignedListWidget
import resources_rc

class Ui_PostingWidget(object):
    def setupUi(self, PostingWidget):
        if not PostingWidget.objectName():
            PostingWidget.setObjectName(u"PostingWidget")
        PostingWidget.resize(1103, 480)
        self.verticalLayout = QVBoxLayout(PostingWidget)
        self.verticalLayout.setObjectName(u"verticalLayout")
        self.horizontalLayout_9 = QHBoxLayout()
        self.horizontalLayout_9.setObjectName(u"horizontalLayout_9")
        self.nzbFileLayout = QHBoxLayout()
        self.nzbFileLayout.setObjectName(u"nzbFileLayout")
        self.nzbFileLbl = QLabel(PostingWidget)
        self.nzbFileLbl.setObjectName(u"nzbFileLbl")

        self.nzbFileLayout.addWidget(self.nzbFileLbl)

        self.nzbFileEdit = QLineEdit(PostingWidget)
        self.nzbFileEdit.setObjectName(u"nzbFileEdit")
        sizePolicy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        sizePolicy.setHorizontalStretch(1)
        sizePolicy.setVerticalStretch(0)
        sizePolicy.setHeightForWidth(self.nzbFileEdit.sizePolicy().hasHeightForWidth())
        self.nzbFileEdit.setSizePolicy(sizePolicy)
        self.nzbFileEdit.setMinimumSize(QSize(120, 0))

        self.nzbFileLayout.addWidget(self.nzbFileEdit)

        self.nzbFileButton = QPushButton(PostingWidget)
        self.nzbFileButton.setObjectName(u"nzbFileButton")
        self.nzbFileButton.setMaximumSize(QSize(30, 16777215))

        self.nzbFileLayout.addWidget(self.nzbFileButton)


        self.horizontalLayout_9.addLayout(self.nzbFileLayout)

        self.sepNzbNfo = QFrame(PostingWidget)
        self.sepNzbNfo.setObjectName(u"sepNzbNfo")
        self.sepNzbNfo.setFrameShape(QFrame.Shape.VLine)
        self.sepNzbNfo.setFrameShadow(QFrame.Shadow.Sunken)

        self.horizontalLayout_9.addWidget(self.sepNzbNfo)

        self.copyNfoWithNzbCB = QCheckBox(PostingWidget)
        self.copyNfoWithNzbCB.setObjectName(u"copyNfoWithNzbCB")

        self.horizontalLayout_9.addWidget(self.copyNfoWithNzbCB)


        self.verticalLayout.addLayout(self.horizontalLayout_9)

        self.filesList = SignedListWidget(PostingWidget)
        self.filesList.setObjectName(u"filesList")

        self.verticalLayout.addWidget(self.filesList)

        self.horizontalLayout_4 = QHBoxLayout()
        self.horizontalLayout_4.setObjectName(u"horizontalLayout_4")
        self.horizontalSpacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_4.addItem(self.horizontalSpacer)

        self.selectFilesButton = QPushButton(PostingWidget)
        self.selectFilesButton.setObjectName(u"selectFilesButton")
        icon = QIcon()
        icon.addFile(u":/icons/file.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.selectFilesButton.setIcon(icon)

        self.horizontalLayout_4.addWidget(self.selectFilesButton)

        self.clearFilesButton = QPushButton(PostingWidget)
        self.clearFilesButton.setObjectName(u"clearFilesButton")
        icon1 = QIcon()
        icon1.addFile(u":/icons/clear.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.clearFilesButton.setIcon(icon1)

        self.horizontalLayout_4.addWidget(self.clearFilesButton)

        self.selectFolderButton = QPushButton(PostingWidget)
        self.selectFolderButton.setObjectName(u"selectFolderButton")
        icon2 = QIcon()
        icon2.addFile(u":/icons/folder.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.selectFolderButton.setIcon(icon2)

        self.horizontalLayout_4.addWidget(self.selectFolderButton)

        self.horizontalSpacer_2 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_4.addItem(self.horizontalSpacer_2)


        self.verticalLayout.addLayout(self.horizontalLayout_4)

        self.packingLayout = QHBoxLayout()
        self.packingLayout.setObjectName(u"packingLayout")
        self.compressCB = QCheckBox(PostingWidget)
        self.compressCB.setObjectName(u"compressCB")

        self.packingLayout.addWidget(self.compressCB)

        self.compressNameEdit = QLineEdit(PostingWidget)
        self.compressNameEdit.setObjectName(u"compressNameEdit")

        self.packingLayout.addWidget(self.compressNameEdit)

        self.nameLengthSB = QSpinBox(PostingWidget)
        self.nameLengthSB.setObjectName(u"nameLengthSB")
        self.nameLengthSB.setMaximumSize(QSize(60, 16777215))

        self.packingLayout.addWidget(self.nameLengthSB)

        self.genCompressName = QPushButton(PostingWidget)
        self.genCompressName.setObjectName(u"genCompressName")
        self.genCompressName.setMaximumSize(QSize(24, 24))
        icon3 = QIcon()
        icon3.addFile(u":/icons/genKey.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.genCompressName.setIcon(icon3)

        self.packingLayout.addWidget(self.genCompressName)

        self.sepPass = QFrame(PostingWidget)
        self.sepPass.setObjectName(u"sepPass")
        self.sepPass.setFrameShape(QFrame.Shape.VLine)
        self.sepPass.setFrameShadow(QFrame.Shadow.Sunken)

        self.packingLayout.addWidget(self.sepPass)

        self.nzbPassCB = QCheckBox(PostingWidget)
        self.nzbPassCB.setObjectName(u"nzbPassCB")

        self.packingLayout.addWidget(self.nzbPassCB)

        self.nzbPassEdit = QLineEdit(PostingWidget)
        self.nzbPassEdit.setObjectName(u"nzbPassEdit")
        self.nzbPassEdit.setEnabled(False)

        self.packingLayout.addWidget(self.nzbPassEdit)

        self.passLengthSB = QSpinBox(PostingWidget)
        self.passLengthSB.setObjectName(u"passLengthSB")
        self.passLengthSB.setMaximumSize(QSize(60, 16777215))

        self.packingLayout.addWidget(self.passLengthSB)

        self.genPass = QPushButton(PostingWidget)
        self.genPass.setObjectName(u"genPass")
        self.genPass.setMaximumSize(QSize(24, 24))
        self.genPass.setIcon(icon3)

        self.packingLayout.addWidget(self.genPass)

        self.sepPar2 = QFrame(PostingWidget)
        self.sepPar2.setObjectName(u"sepPar2")
        self.sepPar2.setFrameShape(QFrame.Shape.VLine)
        self.sepPar2.setFrameShadow(QFrame.Shadow.Sunken)

        self.packingLayout.addWidget(self.sepPar2)

        self.par2CB = QCheckBox(PostingWidget)
        self.par2CB.setObjectName(u"par2CB")

        self.packingLayout.addWidget(self.par2CB)

        self.redundancySB = QSpinBox(PostingWidget)
        self.redundancySB.setObjectName(u"redundancySB")
        self.redundancySB.setMaximumSize(QSize(70, 16777215))

        self.packingLayout.addWidget(self.redundancySB)

        self.sepKeepRar = QFrame(PostingWidget)
        self.sepKeepRar.setObjectName(u"sepKeepRar")
        self.sepKeepRar.setFrameShape(QFrame.Shape.VLine)
        self.sepKeepRar.setFrameShadow(QFrame.Shadow.Sunken)

        self.packingLayout.addWidget(self.sepKeepRar)

        self.keepRarCB = QCheckBox(PostingWidget)
        self.keepRarCB.setObjectName(u"keepRarCB")

        self.packingLayout.addWidget(self.keepRarCB)


        self.verticalLayout.addLayout(self.packingLayout)

        self.postLayout = QHBoxLayout()
        self.postLayout.setObjectName(u"postLayout")
        self.horizontalSpacer_14 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postLayout.addItem(self.horizontalSpacer_14)

        self.postButton = QPushButton(PostingWidget)
        self.postButton.setObjectName(u"postButton")
        icon4 = QIcon()
        icon4.addFile(u":/icons/ngPost.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.postButton.setIcon(icon4)

        self.postLayout.addWidget(self.postButton)

        self.horizontalSpacer_15 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.postLayout.addItem(self.horizontalSpacer_15)


        self.verticalLayout.addLayout(self.postLayout)


        self.retranslateUi(PostingWidget)

        QMetaObject.connectSlotsByName(PostingWidget)
    # setupUi

    def retranslateUi(self, PostingWidget):
        PostingWidget.setWindowTitle(QCoreApplication.translate("PostingWidget", u"Form", None))
        self.nzbFileLbl.setText(QCoreApplication.translate("PostingWidget", u"NZB file:", None))
#if QT_CONFIG(tooltip)
        self.nzbFileEdit.setToolTip(QCoreApplication.translate("PostingWidget", u"full path of the nzb file that would be created (check the nzbPath keyword in the config file to set the default directory)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.nzbFileButton.setToolTip(QCoreApplication.translate("PostingWidget", u"select the nzb file absolute file name", None))
#endif // QT_CONFIG(tooltip)
        self.nzbFileButton.setText(QCoreApplication.translate("PostingWidget", u" ... ", None))
#if QT_CONFIG(tooltip)
        self.copyNfoWithNzbCB.setToolTip(QCoreApplication.translate("PostingWidget", u"If a .nfo file is present in the original files (before rar/renames), copy it next to the generated nzb (with the same base name)", None))
#endif // QT_CONFIG(tooltip)
        self.copyNfoWithNzbCB.setText(QCoreApplication.translate("PostingWidget", u"Copy NFO alongside the nzb file (if available)", None))
#if QT_CONFIG(tooltip)
        self.selectFilesButton.setToolTip(QCoreApplication.translate("PostingWidget", u"Select the files to post (they may be compressed if needed) you can also right click on the files area just above", None))
#endif // QT_CONFIG(tooltip)
        self.selectFilesButton.setText(QCoreApplication.translate("PostingWidget", u"Select Files", None))
#if QT_CONFIG(tooltip)
        self.clearFilesButton.setToolTip(QCoreApplication.translate("PostingWidget", u"remove all files", None))
#endif // QT_CONFIG(tooltip)
        self.clearFilesButton.setText(QCoreApplication.translate("PostingWidget", u"Remove All", None))
#if QT_CONFIG(tooltip)
        self.selectFolderButton.setToolTip(QCoreApplication.translate("PostingWidget", u"select a folder (only if you use compression)", None))
#endif // QT_CONFIG(tooltip)
        self.selectFolderButton.setText(QCoreApplication.translate("PostingWidget", u"Select Folder", None))
#if QT_CONFIG(tooltip)
        self.compressCB.setToolTip(QCoreApplication.translate("PostingWidget", u"compress the selected files using rar before posting", None))
#endif // QT_CONFIG(tooltip)
        self.compressCB.setText(QCoreApplication.translate("PostingWidget", u"Compress this post:", None))
#if QT_CONFIG(tooltip)
        self.compressNameEdit.setToolTip(QCoreApplication.translate("PostingWidget", u"archive name (file name obfuscation)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.nameLengthSB.setToolTip(QCoreApplication.translate("PostingWidget", u"length of the archive name drawn by the dice button (config LENGTH_NAME)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genCompressName.setToolTip(QCoreApplication.translate("PostingWidget", u"generate random archive name", None))
#endif // QT_CONFIG(tooltip)
        self.genCompressName.setText("")
#if QT_CONFIG(tooltip)
        self.nzbPassCB.setToolTip(QCoreApplication.translate("PostingWidget", u"This should be the password of the archive you're posting", None))
#endif // QT_CONFIG(tooltip)
        self.nzbPassCB.setText(QCoreApplication.translate("PostingWidget", u"Archive password", None))
#if QT_CONFIG(tooltip)
        self.nzbPassEdit.setToolTip(QCoreApplication.translate("PostingWidget", u"password used in your archive that would also be added in the header of the nzb file", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.passLengthSB.setToolTip(QCoreApplication.translate("PostingWidget", u"length of the password drawn by the dice button (config LENGTH_PASS)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genPass.setToolTip(QCoreApplication.translate("PostingWidget", u"generate random password", None))
#endif // QT_CONFIG(tooltip)
        self.genPass.setText("")
#if QT_CONFIG(tooltip)
        self.par2CB.setToolTip(QCoreApplication.translate("PostingWidget", u"generate the par2 (the compress option must be selected)", None))
#endif // QT_CONFIG(tooltip)
        self.par2CB.setText(QCoreApplication.translate("PostingWidget", u"Gen PAR2", None))
#if QT_CONFIG(tooltip)
        self.keepRarCB.setToolTip(QCoreApplication.translate("PostingWidget", u"by default archives and par2 files are deleted uppon post success but you can choose to keep them", None))
#endif // QT_CONFIG(tooltip)
        self.keepRarCB.setText(QCoreApplication.translate("PostingWidget", u"Keep Archives", None))
#if QT_CONFIG(tooltip)
        self.postButton.setToolTip(QCoreApplication.translate("PostingWidget", u"Let's Post!", None))
#endif // QT_CONFIG(tooltip)
        self.postButton.setText(QCoreApplication.translate("PostingWidget", u"Post Files", None))
    # retranslateUi

