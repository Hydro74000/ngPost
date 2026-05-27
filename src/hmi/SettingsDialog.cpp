//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "SettingsDialog.h"

#include "MainWindow.h"
#include "NgPost.h"
#include "VpnProfileEditDialog.h"
#include "nntp/NntpArticle.h"
#include "nntp/NntpServerParams.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"

#include <QComboBox>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

namespace
{
enum ServerColumn {
    ServerEnabled = 0,
    ServerLabel,
    ServerHost,
    ServerPort,
    ServerSsl,
    ServerUseVpn,
    ServerConnections,
    ServerUser,
    ServerPassword,
    ServerNzbCheck,
    ServerColumnCount
};
}

SettingsDialog::SettingsDialog(NgPost *ngPost, MainWindow *mainWindow,
                               QWidget *parent)
    : QDialog(parent)
    , _ngPost(ngPost)
    , _mainWindow(mainWindow)
    , _populating(false)
    , _fromEdit(nullptr)
    , _genPosterBtn(nullptr)
    , _saveFromCB(nullptr)
    , _fixedArchivePassCB(nullptr)
    , _fixedArchivePassEdit(nullptr)
    , _fixedArchivePassLengthSB(nullptr)
    , _genArchivePassBtn(nullptr)
    , _articleSizeEdit(nullptr)
    , _nbRetrySB(nullptr)
    , _threadSB(nullptr)
    , _obfuscateArticlesCB(nullptr)
    , _obfuscateFileNameCB(nullptr)
    , _autoCompressCB(nullptr)
    , _autoCloseCB(nullptr)
    , _keepNfoExtensionCB(nullptr)
    , _checkForUpdatesCB(nullptr)
    , _nzbPathEdit(nullptr)
    , _nzbPathBtn(nullptr)
    , _langCB(nullptr)
    , _serversTable(nullptr)
    , _addServerBtn(nullptr)
    , _removeServerBtn(nullptr)
    , _vpnRouteAllCB(nullptr)
    , _vpnDefaultProfileCB(nullptr)
    , _vpnInstallBtn(nullptr)
    , _vpnUninstallBtn(nullptr)
    , _vpnNewProfileBtn(nullptr)
    , _vpnEditProfileBtn(nullptr)
    , _vpnDeleteProfileBtn(nullptr)
    , _buttonBox(nullptr)
{
    setWindowTitle(tr("Settings"));
    resize(900, 640);

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->addTab(_buildGeneralTab(), tr("General"));
    tabs->addTab(_buildServersTab(), tr("Servers"));
    tabs->addTab(_buildVpnTab(), tr("VPN"));
    root->addWidget(tabs);

    _buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok
                                      | QDialogButtonBox::Apply
                                      | QDialogButtonBox::Cancel,
                                      this);
    root->addWidget(_buttonBox);

    connect(_buttonBox->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(_buttonBox, &QDialogButtonBox::accepted,
            this, &SettingsDialog::onAccept);
    connect(_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    if (_ngPost->vpnManager()) {
        connect(_ngPost->vpnManager(), &VpnManager::profilesChanged,
                this, &SettingsDialog::onVpnProfilesChanged);
        connect(_ngPost->vpnManager(), &VpnManager::installStateChanged,
                this, [this](bool) { _refreshVpnSetupUi(); });
    }

    _populateFromNgPost();
}

QWidget *SettingsDialog::_buildGeneralTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *identity = new QGroupBox(tr("Posting identity"), page);
    auto *identityLayout = new QFormLayout(identity);
    auto *posterRow = new QWidget(identity);
    auto *posterRowLayout = new QHBoxLayout(posterRow);
    posterRowLayout->setContentsMargins(0, 0, 0, 0);
    _fromEdit = new QLineEdit(posterRow);
    _fromEdit->setObjectName(QStringLiteral("settingsFromEdit"));
    _genPosterBtn = new QPushButton(QIcon(":/icons/genKey.png"), QString(), posterRow);
    _genPosterBtn->setObjectName(QStringLiteral("settingsGenPosterBtn"));
    _genPosterBtn->setToolTip(tr("Generate random poster"));
    _genPosterBtn->setMaximumWidth(30);
    posterRowLayout->addWidget(_fromEdit);
    posterRowLayout->addWidget(_genPosterBtn);
    identityLayout->addRow(tr("Poster:"), posterRow);
    _saveFromCB = new QCheckBox(tr("Save Email"), identity);
    _saveFromCB->setObjectName(QStringLiteral("settingsSaveFromCB"));
    identityLayout->addRow(QString(), _saveFromCB);
    layout->addWidget(identity);

    auto *archive = new QGroupBox(tr("Archive defaults"), page);
    auto *archiveLayout = new QFormLayout(archive);
    _fixedArchivePassCB = new QCheckBox(tr("Use fixed archive password"), archive);
    _fixedArchivePassCB->setObjectName(QStringLiteral("settingsFixedArchivePassCB"));
    archiveLayout->addRow(QString(), _fixedArchivePassCB);
    auto *passRow = new QWidget(archive);
    auto *passRowLayout = new QHBoxLayout(passRow);
    passRowLayout->setContentsMargins(0, 0, 0, 0);
    _fixedArchivePassEdit = new QLineEdit(passRow);
    _fixedArchivePassEdit->setObjectName(QStringLiteral("settingsFixedArchivePassEdit"));
    _fixedArchivePassEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    _fixedArchivePassLengthSB = new QSpinBox(passRow);
    _fixedArchivePassLengthSB->setObjectName(QStringLiteral("settingsFixedArchivePassLengthSB"));
    _fixedArchivePassLengthSB->setRange(5, 50);
    _genArchivePassBtn = new QPushButton(QIcon(":/icons/genKey.png"), QString(), passRow);
    _genArchivePassBtn->setObjectName(QStringLiteral("settingsGenArchivePassBtn"));
    _genArchivePassBtn->setToolTip(tr("Generate random password"));
    _genArchivePassBtn->setMaximumWidth(30);
    passRowLayout->addWidget(_fixedArchivePassEdit, 1);
    passRowLayout->addWidget(_fixedArchivePassLengthSB);
    passRowLayout->addWidget(_genArchivePassBtn);
    archiveLayout->addRow(tr("Password:"), passRow);
    _autoCompressCB = new QCheckBox(tr("Auto Compress"), archive);
    _autoCompressCB->setObjectName(QStringLiteral("settingsAutoCompressCB"));
    archiveLayout->addRow(QString(), _autoCompressCB);
    layout->addWidget(archive);

    auto *posting = new QGroupBox(tr("Posting parameters"), page);
    auto *postingLayout = new QFormLayout(posting);
    _articleSizeEdit = new QLineEdit(posting);
    _articleSizeEdit->setObjectName(QStringLiteral("settingsArticleSizeEdit"));
    _articleSizeEdit->setValidator(new QIntValidator(100000, 10000000, _articleSizeEdit));
    postingLayout->addRow(tr("Article Size:"), _articleSizeEdit);
    _nbRetrySB = new QSpinBox(posting);
    _nbRetrySB->setObjectName(QStringLiteral("settingsNbRetrySB"));
    _nbRetrySB->setRange(0, 15);
    postingLayout->addRow(tr("No. Retry:"), _nbRetrySB);
    _threadSB = new QSpinBox(posting);
    _threadSB->setObjectName(QStringLiteral("settingsThreadSB"));
    _threadSB->setRange(1, 50);
    postingLayout->addRow(tr("No. Threads:"), _threadSB);
    _obfuscateArticlesCB = new QCheckBox(tr("Article's Obfuscation: Subject changed to be a UUID + Random From"), posting);
    _obfuscateArticlesCB->setObjectName(QStringLiteral("settingsObfuscateArticlesCB"));
    postingLayout->addRow(QString(), _obfuscateArticlesCB);
    _obfuscateFileNameCB = new QCheckBox(tr("File Name Obfuscation"), posting);
    _obfuscateFileNameCB->setObjectName(QStringLiteral("settingsObfuscateFileNameCB"));
    postingLayout->addRow(QString(), _obfuscateFileNameCB);
    _keepNfoExtensionCB = new QCheckBox(tr("keep nfo visible"), posting);
    _keepNfoExtensionCB->setObjectName(QStringLiteral("settingsKeepNfoExtensionCB"));
    postingLayout->addRow(QString(), _keepNfoExtensionCB);
    _autoCloseCB = new QCheckBox(tr("Auto Close Tabs"), posting);
    _autoCloseCB->setObjectName(QStringLiteral("settingsAutoCloseCB"));
    postingLayout->addRow(QString(), _autoCloseCB);
    _checkForUpdatesCB = new QCheckBox(tr("Check for updates"), posting);
    _checkForUpdatesCB->setObjectName(QStringLiteral("settingsCheckForUpdatesCB"));
    postingLayout->addRow(QString(), _checkForUpdatesCB);
    layout->addWidget(posting);

    auto *paths = new QGroupBox(tr("Application"), page);
    auto *pathsLayout = new QFormLayout(paths);
    auto *nzbRow = new QWidget(paths);
    auto *nzbRowLayout = new QHBoxLayout(nzbRow);
    nzbRowLayout->setContentsMargins(0, 0, 0, 0);
    _nzbPathEdit = new QLineEdit(nzbRow);
    _nzbPathEdit->setObjectName(QStringLiteral("settingsNzbPathEdit"));
    _nzbPathBtn = new QPushButton(tr("..."), nzbRow);
    _nzbPathBtn->setObjectName(QStringLiteral("settingsNzbPathBtn"));
    _nzbPathBtn->setMaximumWidth(36);
    nzbRowLayout->addWidget(_nzbPathEdit, 1);
    nzbRowLayout->addWidget(_nzbPathBtn);
    pathsLayout->addRow(tr("NZB Destination Path:"), nzbRow);
    _langCB = new QComboBox(paths);
    _langCB->setObjectName(QStringLiteral("settingsLangCB"));
    pathsLayout->addRow(tr("Lang:"), _langCB);
    layout->addWidget(paths);
    layout->addStretch(1);

    connect(_genPosterBtn, &QPushButton::clicked,
            this, &SettingsDialog::onGeneratePoster);
    connect(_genArchivePassBtn, &QPushButton::clicked,
            this, &SettingsDialog::onGenerateArchivePassword);
    connect(_fixedArchivePassCB, &QCheckBox::toggled,
            this, &SettingsDialog::onFixedArchivePasswordToggled);
    connect(_nzbPathBtn, &QPushButton::clicked,
            this, &SettingsDialog::onBrowseNzbPath);

    return page;
}

QWidget *SettingsDialog::_buildServersTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    _serversTable = new QTableWidget(page);
    _serversTable->setObjectName(QStringLiteral("settingsServersTable"));
    _serversTable->setColumnCount(ServerColumnCount);
    _serversTable->setHorizontalHeaderLabels({
        tr("Use"), tr("Label"), tr("Host"), tr("Port"), tr("SSL"),
        tr("Use VPN"), tr("Connections"), tr("Username"), tr("Password"),
        tr("NZB Check")
    });
    _serversTable->verticalHeader()->hide();
    _serversTable->horizontalHeader()->setStretchLastSection(false);
    _serversTable->horizontalHeader()->setSectionResizeMode(ServerLabel, QHeaderView::Stretch);
    _serversTable->horizontalHeader()->setSectionResizeMode(ServerHost, QHeaderView::Stretch);
    _serversTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _serversTable->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(_serversTable, 1);

    auto *buttons = new QHBoxLayout;
    _addServerBtn = new QPushButton(QIcon(":/icons/plus.png"), tr("Add Server"), page);
    _addServerBtn->setObjectName(QStringLiteral("settingsAddServerBtn"));
    _removeServerBtn = new QPushButton(QIcon(":/icons/clear.png"), tr("Remove"), page);
    _removeServerBtn->setObjectName(QStringLiteral("settingsRemoveServerBtn"));
    buttons->addStretch(1);
    buttons->addWidget(_addServerBtn);
    buttons->addWidget(_removeServerBtn);
    layout->addLayout(buttons);

    connect(_addServerBtn, &QPushButton::clicked,
            this, &SettingsDialog::onAddServer);
    connect(_removeServerBtn, &QPushButton::clicked,
            this, &SettingsDialog::onRemoveServer);
    return page;
}

QWidget *SettingsDialog::_buildVpnTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *setup = new QGroupBox(tr("Setup"), page);
    auto *setupLayout = new QHBoxLayout(setup);
    _vpnInstallBtn = new QPushButton(tr("Install..."), setup);
    _vpnInstallBtn->setObjectName(QStringLiteral("settingsVpnInstallBtn"));
    _vpnUninstallBtn = new QPushButton(tr("Uninstall..."), setup);
    _vpnUninstallBtn->setObjectName(QStringLiteral("settingsVpnUninstallBtn"));
    setupLayout->addWidget(new QLabel(tr("Install or remove the privileged VPN helper/service."), setup), 1);
    setupLayout->addWidget(_vpnInstallBtn);
    setupLayout->addWidget(_vpnUninstallBtn);
    layout->addWidget(setup);

    auto *profiles = new QGroupBox(tr("Profiles"), page);
    auto *profilesLayout = new QVBoxLayout(profiles);
    auto *defaultRow = new QHBoxLayout;
    _vpnDefaultProfileCB = new QComboBox(profiles);
    _vpnDefaultProfileCB->setObjectName(QStringLiteral("settingsVpnDefaultProfileCB"));
    defaultRow->addWidget(new QLabel(tr("Default profile:"), profiles));
    defaultRow->addWidget(_vpnDefaultProfileCB, 1);
    profilesLayout->addLayout(defaultRow);
    auto *profileButtons = new QHBoxLayout;
    _vpnNewProfileBtn = new QPushButton(tr("New..."), profiles);
    _vpnNewProfileBtn->setObjectName(QStringLiteral("settingsVpnNewProfileBtn"));
    _vpnEditProfileBtn = new QPushButton(tr("Edit..."), profiles);
    _vpnEditProfileBtn->setObjectName(QStringLiteral("settingsVpnEditProfileBtn"));
    _vpnDeleteProfileBtn = new QPushButton(tr("Delete"), profiles);
    _vpnDeleteProfileBtn->setObjectName(QStringLiteral("settingsVpnDeleteProfileBtn"));
    profileButtons->addStretch(1);
    profileButtons->addWidget(_vpnNewProfileBtn);
    profileButtons->addWidget(_vpnEditProfileBtn);
    profileButtons->addWidget(_vpnDeleteProfileBtn);
    profilesLayout->addLayout(profileButtons);
    layout->addWidget(profiles);

    auto *options = new QGroupBox(tr("Options"), page);
    auto *optionsLayout = new QVBoxLayout(options);
    _vpnRouteAllCB = new QCheckBox(tr("Route ALL ngPost connections through the VPN (override per-server)"), options);
    _vpnRouteAllCB->setObjectName(QStringLiteral("settingsVpnRouteAllCB"));
    optionsLayout->addWidget(_vpnRouteAllCB);
    layout->addWidget(options);
    layout->addStretch(1);

    connect(_vpnInstallBtn, &QPushButton::clicked,
            this, &SettingsDialog::onInstallVpn);
    connect(_vpnUninstallBtn, &QPushButton::clicked,
            this, &SettingsDialog::onUninstallVpn);
    connect(_vpnNewProfileBtn, &QPushButton::clicked,
            this, &SettingsDialog::onNewVpnProfile);
    connect(_vpnEditProfileBtn, &QPushButton::clicked,
            this, &SettingsDialog::onEditVpnProfile);
    connect(_vpnDeleteProfileBtn, &QPushButton::clicked,
            this, &SettingsDialog::onDeleteVpnProfile);

    return page;
}

void SettingsDialog::_populateFromNgPost()
{
    _populating = true;
    _fromEdit->setText(_ngPost->xml2txt(_ngPost->_from.c_str()));
    _saveFromCB->setChecked(_ngPost->_saveFrom);
    _fixedArchivePassCB->setChecked(!_ngPost->_rarPassFixed.isEmpty());
    _fixedArchivePassEdit->setText(_ngPost->_rarPassFixed);
    _fixedArchivePassLengthSB->setValue(static_cast<int>(_ngPost->_lengthPass));
    onFixedArchivePasswordToggled(_fixedArchivePassCB->isChecked());
    _autoCompressCB->setChecked(_ngPost->_packAuto);
    _articleSizeEdit->setText(QString::number(_ngPost->articleSize()));
    _nbRetrySB->setValue(NntpArticle::nbMaxTrySending());
    _threadSB->setValue(_ngPost->_nbThreads);
    _obfuscateArticlesCB->setChecked(_ngPost->_obfuscateArticles);
    _obfuscateFileNameCB->setChecked(_ngPost->_obfuscateFileName);
    _keepNfoExtensionCB->setChecked(_ngPost->_keepNfoExtension);
    _autoCloseCB->setChecked(_ngPost->_autoCloseTabs);
    _checkForUpdatesCB->setChecked(_ngPost->_checkForUpdates);
    _nzbPathEdit->setText(_ngPost->_nzbPath);

    _langCB->clear();
    for (const QString &lang : _ngPost->languages())
        _langCB->addItem(QIcon(QString(":/icons/flag_%1.png").arg(lang.toUpper())),
                         lang.toUpper(), lang.toLower());
    _langCB->setCurrentText(_ngPost->_lang.toUpper());

    _populateServers();
    _populateVpnProfiles();
    _refreshVpnSetupUi();
    _populating = false;
}

void SettingsDialog::_populateServers()
{
    _serversTable->setRowCount(0);
    for (NntpServerParams *srv : _ngPost->_nntpServers) {
        const int row = _serversTable->rowCount();
        _serversTable->insertRow(row);
        _serversTable->setItem(row, ServerEnabled, _checkItem(srv->enabled));
        _serversTable->setItem(row, ServerLabel, _textItem(srv->label));
        _serversTable->setItem(row, ServerHost, _textItem(srv->host));
        _serversTable->setItem(row, ServerPort, _textItem(QString::number(srv->port)));
        _serversTable->setItem(row, ServerSsl, _checkItem(srv->useSSL));
        _serversTable->setItem(row, ServerUseVpn, _checkItem(srv->useVpn));
        _serversTable->setItem(row, ServerConnections, _textItem(QString::number(srv->nbCons)));
        _serversTable->setItem(row, ServerUser, _textItem(QString::fromStdString(srv->user)));
        _serversTable->setItem(row, ServerPassword, _textItem(QString::fromStdString(srv->pass)));
        _serversTable->setItem(row, ServerNzbCheck, _checkItem(srv->nzbCheck));
    }
}

void SettingsDialog::_populateVpnProfiles()
{
    _vpnDefaultProfileCB->clear();
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    for (const VpnProfile &p : vpn->profiles()) {
        _vpnDefaultProfileCB->addItem(
            QString("%1 [%2]").arg(p.name, VpnManager::backendToString(p.backend)),
            p.name);
    }
    const int activeIdx = vpn->findProfileIndex(vpn->activeProfileName());
    if (activeIdx >= 0)
        _vpnDefaultProfileCB->setCurrentIndex(activeIdx);
    const bool hasProfile = _vpnDefaultProfileCB->count() > 0;
    _vpnEditProfileBtn->setEnabled(hasProfile);
    _vpnDeleteProfileBtn->setEnabled(hasProfile);
}

void SettingsDialog::_refreshVpnSetupUi()
{
    VpnManager *vpn = _ngPost->vpnManager();
    const bool installed = vpn && vpn->isHelperInstalled();
    _vpnInstallBtn->setVisible(!installed);
    _vpnUninstallBtn->setVisible(installed);
    if (vpn)
        _vpnRouteAllCB->setChecked(vpn->autoConnect());
}

bool SettingsDialog::_applyToNgPost()
{
    QString from = _fromEdit->text().trimmed();
    if (!from.isEmpty()) {
        QRegularExpression email("\\w+@\\w+\\.\\w+");
        if (!email.match(from).hasMatch())
            from += QString("@%1.com").arg(_ngPost->aticleSignature().c_str());
        _ngPost->_from = _ngPost->escapeXML(from).toStdString();
    }
    _ngPost->_saveFrom = _saveFromCB->isChecked();

    _ngPost->_rarPassFixed = _fixedArchivePassCB->isChecked()
        ? _fixedArchivePassEdit->text()
        : QString();
    if (!_ngPost->_rarPassFixed.isEmpty())
        _ngPost->_rarPass = _ngPost->_rarPassFixed;
    _ngPost->_lengthPass = static_cast<uint>(_fixedArchivePassLengthSB->value());

    _ngPost->enableAutoPacking(_autoCompressCB->isChecked());
    _ngPost->_autoCloseTabs = _autoCloseCB->isChecked();
    _ngPost->_checkForUpdates = _checkForUpdatesCB->isChecked();
    _ngPost->_obfuscateArticles = _obfuscateArticlesCB->isChecked();
    _ngPost->_obfuscateFileName = _obfuscateFileNameCB->isChecked();
    _ngPost->_keepNfoExtension = _keepNfoExtensionCB->isChecked();

    bool ok = false;
    uint articleSize = _articleSizeEdit->text().toUInt(&ok);
    if (ok)
        NgPost::sArticleSize = articleSize;
    NntpArticle::setNbMaxRetry(static_cast<ushort>(_nbRetrySB->value()));
    _ngPost->_nbThreads = qMax(1, _threadSB->value());

    QFileInfo nzbPath(_nzbPathEdit->text());
    if (nzbPath.exists() && nzbPath.isDir() && nzbPath.isWritable())
        _ngPost->_nzbPath = nzbPath.absoluteFilePath();

    const QString lang = _langCB->currentData().toString();
    if (!lang.isEmpty() && lang != _ngPost->_lang)
        _ngPost->changeLanguage(lang);

    qDeleteAll(_ngPost->_nntpServers);
    _ngPost->_nntpServers.clear();
    for (int row = 0; row < _serversTable->rowCount(); ++row) {
        const QString host = _serverCellText(row, ServerHost).trimmed();
        if (host.isEmpty())
            continue;
        bool portOk = false;
        ushort port = _serverCellText(row, ServerPort).toUShort(&portOk);
        if (!portOk)
            port = NntpServerParams::sDefaultSslPort;
        auto *srv = new NntpServerParams(host, port);
        srv->label = _serverCellText(row, ServerLabel).trimmed();
        srv->enabled = _serverCellChecked(row, ServerEnabled);
        srv->useSSL = _serverCellChecked(row, ServerSsl);
        srv->useVpn = _serverCellChecked(row, ServerUseVpn);
        srv->nzbCheck = _serverCellChecked(row, ServerNzbCheck);
        srv->nbCons = qMax(1, _serverCellText(row, ServerConnections).toInt());
        const QString user = _serverCellText(row, ServerUser);
        if (!user.isEmpty()) {
            srv->auth = true;
            srv->user = user.toStdString();
            srv->pass = _serverCellText(row, ServerPassword).toStdString();
        }
        _ngPost->_nntpServers << srv;
    }

    if (VpnManager *vpn = _ngPost->vpnManager()) {
        const bool vpnSignalsWereBlocked = vpn->blockSignals(true);
        vpn->setAutoConnect(_vpnRouteAllCB->isChecked());
        const QString active = _vpnDefaultProfileCB->currentData().toString();
        if (!active.isEmpty())
            vpn->setActiveProfileName(active);
        vpn->blockSignals(vpnSignalsWereBlocked);
    }

    if (_mainWindow)
        _mainWindow->refreshConfigDependentWidgets();
    _ngPost->saveConfig();
    return true;
}

QTableWidgetItem *SettingsDialog::_textItem(const QString &text,
                                            Qt::ItemFlags extraFlags) const
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | extraFlags);
    return item;
}

QTableWidgetItem *SettingsDialog::_checkItem(bool checked) const
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

QString SettingsDialog::_serverCellText(int row, int col) const
{
    QTableWidgetItem *item = _serversTable->item(row, col);
    return item ? item->text() : QString();
}

bool SettingsDialog::_serverCellChecked(int row, int col) const
{
    QTableWidgetItem *item = _serversTable->item(row, col);
    return item && item->checkState() == Qt::Checked;
}

void SettingsDialog::onApply()
{
    _applyToNgPost();
}

void SettingsDialog::onAccept()
{
    if (_applyToNgPost())
        accept();
}

void SettingsDialog::onBrowseNzbPath()
{
    QString path = QFileDialog::getExistingDirectory(
        this, tr("Select a Folder"), _nzbPathEdit->text(),
        QFileDialog::ShowDirsOnly);
    if (!path.isEmpty())
        _nzbPathEdit->setText(path);
}

void SettingsDialog::onGeneratePoster()
{
    _fromEdit->setText(_ngPost->randomFrom());
}

void SettingsDialog::onGenerateArchivePassword()
{
    _fixedArchivePassEdit->setText(
        _ngPost->randomPass(static_cast<uint>(_fixedArchivePassLengthSB->value())));
}

void SettingsDialog::onFixedArchivePasswordToggled(bool checked)
{
    _fixedArchivePassEdit->setEnabled(checked);
    _fixedArchivePassLengthSB->setEnabled(checked);
    _genArchivePassBtn->setEnabled(checked);
}

void SettingsDialog::onAddServer()
{
    const int row = _serversTable->rowCount();
    _serversTable->insertRow(row);
    _serversTable->setItem(row, ServerEnabled, _checkItem(true));
    _serversTable->setItem(row, ServerLabel, _textItem(QString()));
    _serversTable->setItem(row, ServerHost, _textItem(QString()));
    _serversTable->setItem(row, ServerPort, _textItem(QString::number(NntpServerParams::sDefaultSslPort)));
    _serversTable->setItem(row, ServerSsl, _checkItem(true));
    _serversTable->setItem(row, ServerUseVpn, _checkItem(false));
    _serversTable->setItem(row, ServerConnections, _textItem(QStringLiteral("5")));
    _serversTable->setItem(row, ServerUser, _textItem(QString()));
    _serversTable->setItem(row, ServerPassword, _textItem(QString()));
    _serversTable->setItem(row, ServerNzbCheck, _checkItem(false));
    _serversTable->setCurrentCell(row, ServerLabel);
}

void SettingsDialog::onRemoveServer()
{
    const int row = _serversTable->currentRow();
    if (row >= 0)
        _serversTable->removeRow(row);
}

void SettingsDialog::onInstallVpn()
{
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    setEnabled(false);
    const bool ok = vpn->runInstall();
    setEnabled(true);
    if (!ok)
        QMessageBox::warning(this, tr("VPN install"),
                             tr("VPN install failed. See the VPN actions log for details."));
    _refreshVpnSetupUi();
}

void SettingsDialog::onUninstallVpn()
{
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    if (QMessageBox::question(this, tr("Uninstall VPN helper"),
            tr("This will remove the privileged VPN helper and its Polkit rule. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    setEnabled(false);
    const bool ok = vpn->runUninstall();
    setEnabled(true);
    if (!ok)
        QMessageBox::warning(this, tr("VPN uninstall"),
                             tr("VPN uninstall failed. See the VPN actions log for details."));
    _refreshVpnSetupUi();
}

void SettingsDialog::onNewVpnProfile()
{
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    VpnProfile fresh;
    VpnProfileEditDialog dlg(vpn, QString(), fresh, this);
    dlg.exec();
    _populateVpnProfiles();
}

void SettingsDialog::onEditVpnProfile()
{
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    const QString name = _vpnDefaultProfileCB->currentData().toString();
    if (name.isEmpty())
        return;
    if (vpn->isProfileInUse(name)) {
        QMessageBox::information(this, tr("VPN profile in use"),
                                 tr("Disconnect the VPN before editing this profile."));
        return;
    }
    const int idx = vpn->findProfileIndex(name);
    if (idx < 0)
        return;
    VpnProfileEditDialog dlg(vpn, name, vpn->profiles().at(idx), this);
    dlg.exec();
    _populateVpnProfiles();
}

void SettingsDialog::onDeleteVpnProfile()
{
    VpnManager *vpn = _ngPost->vpnManager();
    if (!vpn)
        return;
    const QString name = _vpnDefaultProfileCB->currentData().toString();
    if (name.isEmpty())
        return;
    if (vpn->isProfileInUse(name)) {
        QMessageBox::information(this, tr("VPN profile in use"),
                                 tr("Disconnect the VPN before deleting this profile."));
        return;
    }
    if (QMessageBox::question(this, tr("Delete VPN profile"),
            tr("Delete profile '%1'?\n\nIts imported config file and credentials will be removed.").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!vpn->removeProfile(name))
        QMessageBox::warning(this, tr("Delete VPN profile"),
                             tr("Could not delete profile '%1'.").arg(name));
    _populateVpnProfiles();
}

void SettingsDialog::onVpnProfilesChanged()
{
    if (!_populating)
        _populateVpnProfiles();
}
