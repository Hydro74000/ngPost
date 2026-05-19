# -*- coding: utf-8 -*-

################################################################################
## Form generated from reading UI file 'MainWindow.ui'
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
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFrame,
    QGroupBox, QHBoxLayout, QHeaderView, QLabel,
    QLineEdit, QMainWindow, QMenuBar, QProgressBar,
    QPushButton, QSizePolicy, QSpacerItem, QSpinBox,
    QSplitter, QTabWidget, QTableWidget, QTableWidgetItem,
    QTextBrowser, QTextEdit, QVBoxLayout, QWidget)
import resources_rc

class Ui_MainWindow(object):
    def setupUi(self, MainWindow):
        if not MainWindow.objectName():
            MainWindow.setObjectName(u"MainWindow")
        MainWindow.resize(1119, 709)
        self.centralwidget = QWidget(MainWindow)
        self.centralwidget.setObjectName(u"centralwidget")
        self.verticalLayout_5 = QVBoxLayout(self.centralwidget)
        self.verticalLayout_5.setObjectName(u"verticalLayout_5")
        self.vSplitter = QSplitter(self.centralwidget)
        self.vSplitter.setObjectName(u"vSplitter")
        self.vSplitter.setOrientation(Qt.Orientation.Vertical)
        self.hSplitter = QSplitter(self.vSplitter)
        self.hSplitter.setObjectName(u"hSplitter")
        self.hSplitter.setOrientation(Qt.Orientation.Horizontal)
        self.serverBox = QGroupBox(self.hSplitter)
        self.serverBox.setObjectName(u"serverBox")
        self.verticalLayout_2 = QVBoxLayout(self.serverBox)
        self.verticalLayout_2.setObjectName(u"verticalLayout_2")
        self.verticalLayout_2.setContentsMargins(-1, 20, -1, -1)
        self.serversTable = QTableWidget(self.serverBox)
        self.serversTable.setObjectName(u"serversTable")
        sizePolicy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        sizePolicy.setHorizontalStretch(0)
        sizePolicy.setVerticalStretch(0)
        sizePolicy.setHeightForWidth(self.serversTable.sizePolicy().hasHeightForWidth())
        self.serversTable.setSizePolicy(sizePolicy)

        self.verticalLayout_2.addWidget(self.serversTable)

        self.horizontalLayout_5 = QHBoxLayout()
        self.horizontalLayout_5.setObjectName(u"horizontalLayout_5")
        self.horizontalSpacer_3 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_5.addItem(self.horizontalSpacer_3)

        self.addServerButton = QPushButton(self.serverBox)
        self.addServerButton.setObjectName(u"addServerButton")
        icon = QIcon()
        icon.addFile(u":/icons/plus.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.addServerButton.setIcon(icon)

        self.horizontalLayout_5.addWidget(self.addServerButton)

        self.horizontalSpacer_4 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_5.addItem(self.horizontalSpacer_4)


        self.verticalLayout_2.addLayout(self.horizontalLayout_5)

        self.hSplitter.addWidget(self.serverBox)
        self.postingBox = QGroupBox(self.hSplitter)
        self.postingBox.setObjectName(u"postingBox")
        self.verticalLayout_4 = QVBoxLayout(self.postingBox)
        self.verticalLayout_4.setObjectName(u"verticalLayout_4")
        self.verticalLayout_4.setContentsMargins(-1, 20, -1, -1)
        self.posterLayout = QHBoxLayout()
        self.posterLayout.setObjectName(u"posterLayout")
        self.fromLbl = QLabel(self.postingBox)
        self.fromLbl.setObjectName(u"fromLbl")

        self.posterLayout.addWidget(self.fromLbl)

        self.fromEdit = QLineEdit(self.postingBox)
        self.fromEdit.setObjectName(u"fromEdit")
        self.fromEdit.setEnabled(True)

        self.posterLayout.addWidget(self.fromEdit)

        self.genPoster = QPushButton(self.postingBox)
        self.genPoster.setObjectName(u"genPoster")
        self.genPoster.setMaximumSize(QSize(24, 24))
        icon1 = QIcon()
        icon1.addFile(u":/icons/genKey.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.genPoster.setIcon(icon1)

        self.posterLayout.addWidget(self.genPoster)

        self.saveFromCB = QCheckBox(self.postingBox)
        self.saveFromCB.setObjectName(u"saveFromCB")

        self.posterLayout.addWidget(self.saveFromCB)

        self.uniqueFromCB = QCheckBox(self.postingBox)
        self.uniqueFromCB.setObjectName(u"uniqueFromCB")

        self.posterLayout.addWidget(self.uniqueFromCB)

        self.horizontalSpacer_6 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.posterLayout.addItem(self.horizontalSpacer_6)

        self.rarPassLayout = QHBoxLayout()
        self.rarPassLayout.setObjectName(u"rarPassLayout")
        self.rarPassCB = QCheckBox(self.postingBox)
        self.rarPassCB.setObjectName(u"rarPassCB")

        self.rarPassLayout.addWidget(self.rarPassCB)

        self.rarPassEdit = QLineEdit(self.postingBox)
        self.rarPassEdit.setObjectName(u"rarPassEdit")
        self.rarPassEdit.setEnabled(False)

        self.rarPassLayout.addWidget(self.rarPassEdit)

        self.rarLengthSB = QSpinBox(self.postingBox)
        self.rarLengthSB.setObjectName(u"rarLengthSB")

        self.rarPassLayout.addWidget(self.rarLengthSB)

        self.genPass = QPushButton(self.postingBox)
        self.genPass.setObjectName(u"genPass")
        self.genPass.setMaximumSize(QSize(24, 24))
        self.genPass.setIcon(icon1)

        self.rarPassLayout.addWidget(self.genPass)


        self.posterLayout.addLayout(self.rarPassLayout)


        self.verticalLayout_4.addLayout(self.posterLayout)

        self.groupsLayout = QHBoxLayout()
        self.groupsLayout.setObjectName(u"groupsLayout")
        self.groupsLbl = QLabel(self.postingBox)
        self.groupsLbl.setObjectName(u"groupsLbl")
        self.groupsLbl.setMaximumSize(QSize(16777215, 50))

        self.groupsLayout.addWidget(self.groupsLbl)

        self.groupsEdit = QTextEdit(self.postingBox)
        self.groupsEdit.setObjectName(u"groupsEdit")
        self.groupsEdit.setMaximumSize(QSize(16777215, 30))

        self.groupsLayout.addWidget(self.groupsEdit)


        self.verticalLayout_4.addLayout(self.groupsLayout)

        self.frame_3 = QFrame(self.postingBox)
        self.frame_3.setObjectName(u"frame_3")
        self.frame_3.setFrameShape(QFrame.Shape.HLine)
        self.frame_3.setFrameShadow(QFrame.Shadow.Raised)

        self.verticalLayout_4.addWidget(self.frame_3)

        self.horizontalLayout_3 = QHBoxLayout()
        self.horizontalLayout_3.setObjectName(u"horizontalLayout_3")
        self.articleSizeLbl = QLabel(self.postingBox)
        self.articleSizeLbl.setObjectName(u"articleSizeLbl")

        self.horizontalLayout_3.addWidget(self.articleSizeLbl)

        self.articleSizeEdit = QLineEdit(self.postingBox)
        self.articleSizeEdit.setObjectName(u"articleSizeEdit")
        self.articleSizeEdit.setMaximumSize(QSize(80, 16777215))

        self.horizontalLayout_3.addWidget(self.articleSizeEdit)

        self.nbRetryLbl = QLabel(self.postingBox)
        self.nbRetryLbl.setObjectName(u"nbRetryLbl")

        self.horizontalLayout_3.addWidget(self.nbRetryLbl)

        self.nbRetrySB = QSpinBox(self.postingBox)
        self.nbRetrySB.setObjectName(u"nbRetrySB")

        self.horizontalLayout_3.addWidget(self.nbRetrySB)

        self.horizontalSpacer_5 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_3.addItem(self.horizontalSpacer_5)

        self.threadLbl = QLabel(self.postingBox)
        self.threadLbl.setObjectName(u"threadLbl")

        self.horizontalLayout_3.addWidget(self.threadLbl)

        self.threadSB = QSpinBox(self.postingBox)
        self.threadSB.setObjectName(u"threadSB")

        self.horizontalLayout_3.addWidget(self.threadSB)


        self.verticalLayout_4.addLayout(self.horizontalLayout_3)

        self.frame_2 = QFrame(self.postingBox)
        self.frame_2.setObjectName(u"frame_2")
        self.frame_2.setFrameShape(QFrame.Shape.HLine)
        self.frame_2.setFrameShadow(QFrame.Shadow.Raised)

        self.verticalLayout_4.addWidget(self.frame_2)

        self.horizontalLayout_6 = QHBoxLayout()
        self.horizontalLayout_6.setObjectName(u"horizontalLayout_6")
        self.obfuscateMsgIdCB = QCheckBox(self.postingBox)
        self.obfuscateMsgIdCB.setObjectName(u"obfuscateMsgIdCB")
        self.obfuscateMsgIdCB.setChecked(False)

        self.horizontalLayout_6.addWidget(self.obfuscateMsgIdCB)

        self.obfuscateFileNameCB = QCheckBox(self.postingBox)
        self.obfuscateFileNameCB.setObjectName(u"obfuscateFileNameCB")

        self.horizontalLayout_6.addWidget(self.obfuscateFileNameCB)

        self.horizontalSpacer = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_6.addItem(self.horizontalSpacer)

        self.autoCompressCB = QCheckBox(self.postingBox)
        self.autoCompressCB.setObjectName(u"autoCompressCB")

        self.horizontalLayout_6.addWidget(self.autoCompressCB)

        self.autoCloseCB = QCheckBox(self.postingBox)
        self.autoCloseCB.setObjectName(u"autoCloseCB")

        self.horizontalLayout_6.addWidget(self.autoCloseCB)


        self.verticalLayout_4.addLayout(self.horizontalLayout_6)

        self.horizontalLayout_keepNfo = QHBoxLayout()
        self.horizontalLayout_keepNfo.setObjectName(u"horizontalLayout_keepNfo")
        self.keepNfoExtensionCB = QCheckBox(self.postingBox)
        self.keepNfoExtensionCB.setObjectName(u"keepNfoExtensionCB")

        self.horizontalLayout_keepNfo.addWidget(self.keepNfoExtensionCB)

        self.horizontalSpacer_keepNfo = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_keepNfo.addItem(self.horizontalSpacer_keepNfo)

        self.checkForUpdatesCB = QCheckBox(self.postingBox)
        self.checkForUpdatesCB.setObjectName(u"checkForUpdatesCB")

        self.horizontalLayout_keepNfo.addWidget(self.checkForUpdatesCB)

        self.vpnSettingsBtn = QPushButton(self.postingBox)
        self.vpnSettingsBtn.setObjectName(u"vpnSettingsBtn")

        self.horizontalLayout_keepNfo.addWidget(self.vpnSettingsBtn)

        self.vpnStateLbl = QLabel(self.postingBox)
        self.vpnStateLbl.setObjectName(u"vpnStateLbl")

        self.horizontalLayout_keepNfo.addWidget(self.vpnStateLbl)


        self.verticalLayout_4.addLayout(self.horizontalLayout_keepNfo)

        self.frame_4 = QFrame(self.postingBox)
        self.frame_4.setObjectName(u"frame_4")
        self.frame_4.setFrameShape(QFrame.Shape.HLine)
        self.frame_4.setFrameShadow(QFrame.Shadow.Raised)

        self.verticalLayout_4.addWidget(self.frame_4)

        self.horizontalLayout_8 = QHBoxLayout()
        self.horizontalLayout_8.setObjectName(u"horizontalLayout_8")
        self.horizontalLayout_4 = QHBoxLayout()
        self.horizontalLayout_4.setObjectName(u"horizontalLayout_4")
        self.nzbPathLbl = QLabel(self.postingBox)
        self.nzbPathLbl.setObjectName(u"nzbPathLbl")

        self.horizontalLayout_4.addWidget(self.nzbPathLbl)

        self.nzbPathEdit = QLineEdit(self.postingBox)
        self.nzbPathEdit.setObjectName(u"nzbPathEdit")
        self.nzbPathEdit.setMinimumSize(QSize(0, 0))

        self.horizontalLayout_4.addWidget(self.nzbPathEdit)

        self.nzbPathButton = QPushButton(self.postingBox)
        self.nzbPathButton.setObjectName(u"nzbPathButton")
        self.nzbPathButton.setMaximumSize(QSize(30, 16777215))

        self.horizontalLayout_4.addWidget(self.nzbPathButton)


        self.horizontalLayout_8.addLayout(self.horizontalLayout_4)

        self.horizontalSpacer_12 = QSpacerItem(40, 20, QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_8.addItem(self.horizontalSpacer_12)

        self.shutdownCB = QCheckBox(self.postingBox)
        self.shutdownCB.setObjectName(u"shutdownCB")

        self.horizontalLayout_8.addWidget(self.shutdownCB)

        self.horizontalSpacer_2 = QSpacerItem(40, 20, QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Minimum)

        self.horizontalLayout_8.addItem(self.horizontalSpacer_2)

        self.horizontalLayout_7 = QHBoxLayout()
        self.horizontalLayout_7.setObjectName(u"horizontalLayout_7")
        self.langLbl = QLabel(self.postingBox)
        self.langLbl.setObjectName(u"langLbl")

        self.horizontalLayout_7.addWidget(self.langLbl)

        self.langCB = QComboBox(self.postingBox)
        self.langCB.setObjectName(u"langCB")

        self.horizontalLayout_7.addWidget(self.langCB)


        self.horizontalLayout_8.addLayout(self.horizontalLayout_7)

        self.saveButton = QPushButton(self.postingBox)
        self.saveButton.setObjectName(u"saveButton")
        icon2 = QIcon()
        icon2.addFile(u":/icons/save.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.saveButton.setIcon(icon2)

        self.horizontalLayout_8.addWidget(self.saveButton)


        self.verticalLayout_4.addLayout(self.horizontalLayout_8)

        self.verticalSpacer = QSpacerItem(20, 40, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding)

        self.verticalLayout_4.addItem(self.verticalSpacer)

        self.hSplitter.addWidget(self.postingBox)
        self.vSplitter.addWidget(self.hSplitter)
        self.postSplitter = QSplitter(self.vSplitter)
        self.postSplitter.setObjectName(u"postSplitter")
        self.postSplitter.setOrientation(Qt.Orientation.Horizontal)
        self.fileBox = QGroupBox(self.postSplitter)
        self.fileBox.setObjectName(u"fileBox")
        self.verticalLayout_3 = QVBoxLayout(self.fileBox)
        self.verticalLayout_3.setObjectName(u"verticalLayout_3")
        self.verticalLayout_3.setContentsMargins(-1, 20, -1, -1)
        self.postTabWidget = QTabWidget(self.fileBox)
        self.postTabWidget.setObjectName(u"postTabWidget")
        self.tab = QWidget()
        self.tab.setObjectName(u"tab")
        self.postTabWidget.addTab(self.tab, "")
        self.tab_2 = QWidget()
        self.tab_2.setObjectName(u"tab_2")
        self.postTabWidget.addTab(self.tab_2, icon, "")

        self.verticalLayout_3.addWidget(self.postTabWidget)

        self.postSplitter.addWidget(self.fileBox)
        self.logBox = QGroupBox(self.postSplitter)
        self.logBox.setObjectName(u"logBox")
        self.verticalLayout = QVBoxLayout(self.logBox)
        self.verticalLayout.setObjectName(u"verticalLayout")
        self.verticalLayout.setContentsMargins(-1, 20, -1, -1)
        self.logBrowser = QTextBrowser(self.logBox)
        self.logBrowser.setObjectName(u"logBrowser")

        self.verticalLayout.addWidget(self.logBrowser)

        self.horizontalLayout = QHBoxLayout()
        self.horizontalLayout.setObjectName(u"horizontalLayout")
        self.debugBox = QCheckBox(self.logBox)
        self.debugBox.setObjectName(u"debugBox")

        self.horizontalLayout.addWidget(self.debugBox)

        self.debugSB = QSpinBox(self.logBox)
        self.debugSB.setObjectName(u"debugSB")
        self.debugSB.setMaximum(2)

        self.horizontalLayout.addWidget(self.debugSB)

        self.horizontalSpacer_7 = QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)

        self.horizontalLayout.addItem(self.horizontalSpacer_7)

        self.clearLogButton = QPushButton(self.logBox)
        self.clearLogButton.setObjectName(u"clearLogButton")
        icon3 = QIcon()
        icon3.addFile(u":/icons/clear.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.clearLogButton.setIcon(icon3)

        self.horizontalLayout.addWidget(self.clearLogButton)


        self.verticalLayout.addLayout(self.horizontalLayout)

        self.postSplitter.addWidget(self.logBox)
        self.vSplitter.addWidget(self.postSplitter)

        self.verticalLayout_5.addWidget(self.vSplitter)

        self.uploadFrame = QFrame(self.centralwidget)
        self.uploadFrame.setObjectName(u"uploadFrame")
        self.uploadFrame.setMaximumSize(QSize(16777215, 50))
        self.uploadFrame.setFrameShape(QFrame.Shape.NoFrame)
        self.uploadFrame.setFrameShadow(QFrame.Shadow.Raised)
        self.uploadFrame.setLineWidth(0)
        self.horizontalLayout_2 = QHBoxLayout(self.uploadFrame)
        self.horizontalLayout_2.setObjectName(u"horizontalLayout_2")
        self.horizontalLayout_2.setContentsMargins(0, 5, 0, 5)
        self.jobLabel = QLabel(self.uploadFrame)
        self.jobLabel.setObjectName(u"jobLabel")

        self.horizontalLayout_2.addWidget(self.jobLabel, 0, Qt.AlignmentFlag.AlignVCenter)

        self.pauseButton = QPushButton(self.uploadFrame)
        self.pauseButton.setObjectName(u"pauseButton")
        icon4 = QIcon()
        icon4.addFile(u":/icons/pause.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.pauseButton.setIcon(icon4)
        self.pauseButton.setIconSize(QSize(24, 24))

        self.horizontalLayout_2.addWidget(self.pauseButton, 0, Qt.AlignmentFlag.AlignVCenter)

        self.progressBar = QProgressBar(self.uploadFrame)
        self.progressBar.setObjectName(u"progressBar")
        self.progressBar.setMaximumSize(QSize(16777215, 25))
        self.progressBar.setValue(24)

        self.horizontalLayout_2.addWidget(self.progressBar, 0, Qt.AlignmentFlag.AlignVCenter)

        self.uploadLbl = QLabel(self.uploadFrame)
        self.uploadLbl.setObjectName(u"uploadLbl")

        self.horizontalLayout_2.addWidget(self.uploadLbl, 0, Qt.AlignmentFlag.AlignRight|Qt.AlignmentFlag.AlignVCenter)

        self.goCmdButton = QPushButton(self.uploadFrame)
        self.goCmdButton.setObjectName(u"goCmdButton")
        icon5 = QIcon()
        icon5.addFile(u":/icons/cmd.png", QSize(), QIcon.Mode.Normal, QIcon.State.Off)
        self.goCmdButton.setIcon(icon5)
        self.goCmdButton.setIconSize(QSize(32, 24))
        self.goCmdButton.setFlat(True)

        self.horizontalLayout_2.addWidget(self.goCmdButton, 0, Qt.AlignmentFlag.AlignRight|Qt.AlignmentFlag.AlignVCenter)


        self.verticalLayout_5.addWidget(self.uploadFrame)

        MainWindow.setCentralWidget(self.centralwidget)
        self.menubar = QMenuBar(MainWindow)
        self.menubar.setObjectName(u"menubar")
        self.menubar.setGeometry(QRect(0, 0, 1119, 17))
        MainWindow.setMenuBar(self.menubar)

        self.retranslateUi(MainWindow)

        self.postTabWidget.setCurrentIndex(1)


        QMetaObject.connectSlotsByName(MainWindow)
    # setupUi

    def retranslateUi(self, MainWindow):
#if QT_CONFIG(tooltip)
        self.addServerButton.setToolTip(QCoreApplication.translate("MainWindow", u"Add a server (you can use as many as you want)", None))
#endif // QT_CONFIG(tooltip)
        self.addServerButton.setText(QCoreApplication.translate("MainWindow", u"Add Server", None))
        self.fromLbl.setText(QCoreApplication.translate("MainWindow", u"Poster:", None))
#if QT_CONFIG(tooltip)
        self.fromEdit.setToolTip(QCoreApplication.translate("MainWindow", u"poster used for all the Articles and all the Posts", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genPoster.setToolTip(QCoreApplication.translate("MainWindow", u"generate random Poster", None))
#endif // QT_CONFIG(tooltip)
        self.genPoster.setText("")
#if QT_CONFIG(tooltip)
        self.saveFromCB.setToolTip(QCoreApplication.translate("MainWindow", u"use this poster email everytime you launch ngPost", None))
#endif // QT_CONFIG(tooltip)
        self.saveFromCB.setText(QCoreApplication.translate("MainWindow", u"Save Email", None))
#if QT_CONFIG(tooltip)
        self.uniqueFromCB.setToolTip(QCoreApplication.translate("MainWindow", u"generate a new random email for each Post", None))
#endif // QT_CONFIG(tooltip)
        self.uniqueFromCB.setText(QCoreApplication.translate("MainWindow", u"New Random Email For Each Post", None))
#if QT_CONFIG(tooltip)
        self.rarPassCB.setToolTip(QCoreApplication.translate("MainWindow", u"Use a Fixed password for all the Posts", None))
#endif // QT_CONFIG(tooltip)
        self.rarPassCB.setText(QCoreApplication.translate("MainWindow", u"Archive Password:", None))
#if QT_CONFIG(tooltip)
        self.rarPassEdit.setToolTip(QCoreApplication.translate("MainWindow", u"archive password", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.rarLengthSB.setToolTip(QCoreApplication.translate("MainWindow", u"length of the password", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.genPass.setToolTip(QCoreApplication.translate("MainWindow", u"generate random password", None))
#endif // QT_CONFIG(tooltip)
        self.genPass.setText("")
#if QT_CONFIG(tooltip)
        self.groupsLbl.setToolTip(QCoreApplication.translate("MainWindow", u"add the list of groups separated with a coma (no space)", None))
#endif // QT_CONFIG(tooltip)
        self.groupsLbl.setText(QCoreApplication.translate("MainWindow", u"News Groups:", None))
#if QT_CONFIG(tooltip)
        self.groupsEdit.setToolTip(QCoreApplication.translate("MainWindow", u"list of groups where to post (coma separated with no space)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.articleSizeLbl.setToolTip(QCoreApplication.translate("MainWindow", u"don't touch if you don't know what it is", None))
#endif // QT_CONFIG(tooltip)
        self.articleSizeLbl.setText(QCoreApplication.translate("MainWindow", u"Article Size:", None))
#if QT_CONFIG(tooltip)
        self.articleSizeEdit.setToolTip(QCoreApplication.translate("MainWindow", u"don't touch if you don't know what it is", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.nbRetryLbl.setToolTip(QCoreApplication.translate("MainWindow", u"Number of retry if an Article FAILS to be posted (probably its msg-id is already in use)", None))
#endif // QT_CONFIG(tooltip)
        self.nbRetryLbl.setText(QCoreApplication.translate("MainWindow", u"No. Retry", None))
#if QT_CONFIG(tooltip)
        self.threadLbl.setToolTip(QCoreApplication.translate("MainWindow", u"Number of Threads on which all the Connections will be spreads", None))
#endif // QT_CONFIG(tooltip)
        self.threadLbl.setText(QCoreApplication.translate("MainWindow", u"No. Threads:", None))
#if QT_CONFIG(tooltip)
        self.obfuscateMsgIdCB.setToolTip(QCoreApplication.translate("MainWindow", u"CAREFUL: you won't be able to find your post without the NZB file", None))
#endif // QT_CONFIG(tooltip)
        self.obfuscateMsgIdCB.setText(QCoreApplication.translate("MainWindow", u"Article's Obfuscation: Subject changed to be a UUID + Random From", None))
#if QT_CONFIG(tooltip)
        self.obfuscateFileNameCB.setToolTip(QCoreApplication.translate("MainWindow", u"when using compression with random name, rename also the file inside the archive", None))
#endif // QT_CONFIG(tooltip)
        self.obfuscateFileNameCB.setText(QCoreApplication.translate("MainWindow", u"File Name Obfuscation", None))
#if QT_CONFIG(tooltip)
        self.autoCompressCB.setToolTip(QCoreApplication.translate("MainWindow", u"compress QuickPosts with a random archive name and password and generate the par2", None))
#endif // QT_CONFIG(tooltip)
        self.autoCompressCB.setText(QCoreApplication.translate("MainWindow", u"Auto Compress", None))
#if QT_CONFIG(tooltip)
        self.autoCloseCB.setToolTip(QCoreApplication.translate("MainWindow", u"Auto close Tabs when posted successfully", None))
#endif // QT_CONFIG(tooltip)
        self.autoCloseCB.setText(QCoreApplication.translate("MainWindow", u"Auto Close Tabs", None))
#if QT_CONFIG(tooltip)
        self.keepNfoExtensionCB.setToolTip(QCoreApplication.translate("MainWindow", u"Keep .nfo file(s) visible on the post: the nfo stays inside the rar AND is posted alongside (named like the archive)", None))
#endif // QT_CONFIG(tooltip)
        self.keepNfoExtensionCB.setText(QCoreApplication.translate("MainWindow", u"keep nfo visible", None))
#if QT_CONFIG(tooltip)
        self.checkForUpdatesCB.setToolTip(QCoreApplication.translate("MainWindow", u"Check once a day for a new ngPost release on GitHub", None))
#endif // QT_CONFIG(tooltip)
        self.checkForUpdatesCB.setText(QCoreApplication.translate("MainWindow", u"Check for updates", None))
#if QT_CONFIG(tooltip)
        self.vpnSettingsBtn.setToolTip(QCoreApplication.translate("MainWindow", u"Tunnel ngPost connections through OpenVPN or WireGuard (the rest of the system is unaffected)", None))
#endif // QT_CONFIG(tooltip)
        self.vpnSettingsBtn.setText(QCoreApplication.translate("MainWindow", u"VPN...", None))
#if QT_CONFIG(tooltip)
        self.vpnStateLbl.setToolTip(QCoreApplication.translate("MainWindow", u"Current VPN tunnel state", None))
#endif // QT_CONFIG(tooltip)
        self.vpnStateLbl.setText(QCoreApplication.translate("MainWindow", u"VPN: disabled", None))
        self.nzbPathLbl.setText(QCoreApplication.translate("MainWindow", u"NZB Destination Path: ", None))
#if QT_CONFIG(tooltip)
        self.nzbPathEdit.setToolTip(QCoreApplication.translate("MainWindow", u"set the destination path of all the nzb file", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.nzbPathButton.setToolTip(QCoreApplication.translate("MainWindow", u"set the destination path of all the nzb file", None))
#endif // QT_CONFIG(tooltip)
        self.nzbPathButton.setText(QCoreApplication.translate("MainWindow", u" ... ", None))
        self.shutdownCB.setText(QCoreApplication.translate("MainWindow", u"Shutdown Computer", None))
#if QT_CONFIG(tooltip)
        self.langLbl.setToolTip(QCoreApplication.translate("MainWindow", u"language", None))
#endif // QT_CONFIG(tooltip)
        self.langLbl.setText(QCoreApplication.translate("MainWindow", u"Lang:", None))
#if QT_CONFIG(tooltip)
        self.saveButton.setToolTip(QCoreApplication.translate("MainWindow", u"save all the parameters including servers in the config file", None))
#endif // QT_CONFIG(tooltip)
        self.saveButton.setText(QCoreApplication.translate("MainWindow", u"Save", None))
        self.postTabWidget.setTabText(self.postTabWidget.indexOf(self.tab), QCoreApplication.translate("MainWindow", u"Job 1", None))
        self.postTabWidget.setTabText(self.postTabWidget.indexOf(self.tab_2), QCoreApplication.translate("MainWindow", u"Add", None))
#if QT_CONFIG(tooltip)
        self.debugBox.setToolTip(QCoreApplication.translate("MainWindow", u"show some debug information", None))
#endif // QT_CONFIG(tooltip)
        self.debugBox.setText(QCoreApplication.translate("MainWindow", u"Show Debug Info", None))
#if QT_CONFIG(tooltip)
        self.debugSB.setToolTip(QCoreApplication.translate("MainWindow", u"debug level (0 nothing, 1 few extra info, 2  debugging level)", None))
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        self.clearLogButton.setToolTip(QCoreApplication.translate("MainWindow", u"clear log", None))
#endif // QT_CONFIG(tooltip)
        self.clearLogButton.setText(QCoreApplication.translate("MainWindow", u"Clear", None))
        self.jobLabel.setText(QCoreApplication.translate("MainWindow", u"TextLabel", None))
        self.pauseButton.setText("")
#if QT_CONFIG(tooltip)
        self.goCmdButton.setToolTip(QCoreApplication.translate("MainWindow", u"go command line (close the GUI and continue in the shell)", None))
#endif // QT_CONFIG(tooltip)
        self.goCmdButton.setText("")
        pass
    # retranslateUi

