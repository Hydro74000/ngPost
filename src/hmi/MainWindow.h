//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "vpn/VpnManager.h"
#include "history/PostHistoryStore.h"

#include <QMainWindow>
#include <QFileInfoList>
#include <QSet>
#include <QUrl>
class NgPost;
struct NntpServerParams;
class NntpFile;
class PostingWidget;
class AutoPostWidget;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;
class StartupTabBar;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QChart;
#else
namespace QtCharts { class QChart; }
using QtCharts::QChart;
#endif

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

private:
    enum class STATE {IDLE, POSTING, STOPPING};

    Ui::MainWindow *_ui;
    NgPost         *_ngPost;
    STATE           _state;
    PostingWidget  *_quickJobTab;
    AutoPostWidget *_autoPostTab;

    //! Tab opened when ngPost starts: 0 quick post, 1 folder monitoring,
    //! 2 history, and -1 when the user never picked one -- which is the quick
    //! post tab, without a setting written anywhere.
    int             _startupTab;

    static const bool sDefaultServerSSL   = true;
    static const int  sDefaultConnections = 5;
    static const int  sDefaultServerPort  = 563;
    static const int  sDeleteColumnWidth  = 30;
    static const int  sHistoryPageSize    = 200;

    static const QList<const char *> sServerListHeaders;
    static const QVector<int> sServerListSizes;

    // History tab widgets and state
    QTabWidget   *_innerHistoryTabs   = nullptr;  //!< inner tabs (History/Stats/Resume)
    QTableWidget *_historyTable       = nullptr;
    QTableWidget *_resumeTable        = nullptr;
    QLineEdit    *_historySearchEdit  = nullptr;
    QComboBox    *_historyStatusFilter = nullptr;
    QCheckBox    *_historyPassFilter  = nullptr;
    QDateEdit    *_historyDateFrom    = nullptr;
    QDateEdit    *_historyDateTo      = nullptr;
    QLineEdit    *_historyGroupEdit   = nullptr;
    QCheckBox    *_historyErrorsFilter = nullptr;
    QLabel       *_historyDetailInfo  = nullptr;
    QPushButton  *_histRegenNzbBtn    = nullptr;
    QPushButton  *_histExportInfoBtn  = nullptr;
    QPushButton  *_histCopyPassBtn    = nullptr;
    QPushButton  *_histPurgePassBtn   = nullptr;
    QPushButton  *_histDeleteBtn      = nullptr;
    QPushButton  *_histOpenNzbBtn     = nullptr;
    QPushButton  *_resumeResumeBtn    = nullptr;
    QPushButton  *_resumeAbandonBtn   = nullptr;
    QPushButton  *_resumePurgeBtn     = nullptr;
    QPushButton  *_resumeIgnoreBtn    = nullptr;
    QPushButton  *_resumeDeleteBtn    = nullptr;
    QLabel       *_resumeDetailInfo   = nullptr;
    QLabel       *_statsLabel         = nullptr;
    QComboBox    *_statsPeriodFilter  = nullptr;
    QComboBox    *_statsGroupFilter   = nullptr;
    QChart       *_statsTimelineChart = nullptr;
    QChart       *_statsGroupChart    = nullptr;
    QTableWidget *_statsTopTable      = nullptr;
    qint64        _selectedHistoryId  = 0;
    QSet<qint64>  _ignoredResumeIds;
    int           _historyPageOffset  = 0;
    int           _historyRefreshGeneration = 0;

    //! The user dragged a history column: ngPost stops sizing them.
    bool          _historyColumnsResizedByUser = false;
    //! Set while _fitHistoryColumns() resizes, so its own sections do not read
    //! as a drag.
    bool          _resizingHistoryColumns      = false;
    //! Debounces the write of the column widths: a drag fires one signal per
    //! pixel, the settings are written once it stops.
    QTimer       *_historyColumnsSaveTimer     = nullptr;

    // Additional history tab widgets needed for retranslation
    QLabel       *_bannerLabel        = nullptr;
    QPushButton  *_bannerResumeBtn    = nullptr;
    QLabel       *_histSearchLabel    = nullptr;
    QLabel       *_histStatusLabel    = nullptr;
    QPushButton  *_histRefreshBtn     = nullptr;
    QPushButton  *_histExportCsvBtn   = nullptr;
    QLabel       *_histFromLabel      = nullptr;
    QLabel       *_histToLabel        = nullptr;
    QLabel       *_histGroupLabel     = nullptr;
    QPushButton  *_histClearBtn       = nullptr;
    QPushButton  *_histPrevPageBtn    = nullptr;
    QPushButton  *_histNextPageBtn    = nullptr;
    QLabel       *_histPageLabel      = nullptr;
    QLabel       *_statsPeriodLabel   = nullptr;
    QLabel       *_statsGroupLabel    = nullptr;
    QPushButton  *_statsRefreshBtn    = nullptr;
    QTabWidget   *_statsInnerTabs     = nullptr;
    QPushButton  *_resumeRefreshBtn   = nullptr;



public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void init(NgPost *ngPost);
#ifdef NGPOST_TESTING
    QWidget *buildHistoryTabForTest();
    void     fitHistoryColumnsForTest(bool toContents) { _fitHistoryColumns(toContents); }
    int      startupTabForTest() const { return _startupTab; }
    void     fillTabContextMenuForTest(QMenu &menu, int tabIndex) { _fillTabContextMenu(menu, tabIndex); }
#endif

    void updateProgressBar(uint nbArticlesTotal, uint nbArticlesUploaded, const QString &avgSpeed = "0 B/s"
                       #ifdef __COMPUTE_IMMEDIATE_SPEED__
                           , const QString &immediateSpeed = "0 B/s"
                       #endif
                           );

    void updateServers();
    void updateParams();
    void updateAutoPostingParams();
    void updateConfigFromUi();

    QString fixedArchivePassword() const;

    PostingWidget *addNewQuickTab(int lastTabIdx, const QFileInfoList &files = QFileInfoList());

    void setTab(QWidget *postWidget);
    void clearJobTab(QWidget *postWidget);
    void updateJobTab(QWidget *postWidget, const QColor &color, const QIcon &icon, const QString &tooltip = "");

    void setJobLabel(int jobNumber);

    void log(const QString &aMsg, bool newline = true) const; //!< log function for QString
    void logError(const QString &error) const; //!< log function for QString

    bool useFixedPassword() const;
    bool hasAutoCompress() const;


    bool hasFinishedPosts() const;

    inline AutoPostWidget *autoWidget() const;
    void closeTab(PostingWidget *postWidget);

    void setPauseIcon(bool pause);

    static const QColor  sPostingColor;
    static const QString sPostingIcon;
    static const QColor  sPendingColor;
    static const QString sPendingIcon;
    static const QColor  sDoneOKColor;
    static const QString sDoneOKIcon;
    static const QColor  sDoneKOColor;
    static const QString sDoneKOIcon;
    static const QColor  sArticlesFailedColor;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent* event) override;

public slots:
    void onSetProgressBarRange(int nbArticles);
    void onNewVersionAvailable(const QString &tag, const QString &notes, const QUrl &releasePage);


private slots:
    void onAddServer();
    void onDelServer();

    void onObfucateToggled(bool checked);

    void onTabContextMenu(const QPoint &point);
    void onCloseAllFinishedQuickTabs();

    //! Tab context menu: pin \a tabIndex as the tab to open at startup, or
    //! unpin it when it already is the one.
    void onToggleStartupTab(int tabIndex);



    void onGenPoster();
    void onUniqueFromToggled(bool checked);
    void onRarPassUpdated(const QString &fixedPass);
    void onCompressionSettingsClicked();
    void onAutoCompressToggled(bool checked);

    void onDebugToggled(bool checked);
    void onDebugValue(int value);

    void onSaveConfig();

    void onJobTabClicked(int index);
    void onCloseJob(int index);

    void toBeImplemented();

    void onNzbPathClicked();

    void onLangChanged(const QString &lang);

    void onShutdownToggled(bool checked);

    void onPauseClicked();

    void onVpnSettingsClicked();
    void onVpnStateChanged(VpnManager::State newState);

    void _onHistoryRefresh();
    void _onHistoryPreviousPage();
    void _onHistoryNextPage();
    void _onHistoryRowSelected(int row);
    void _onHistoryRegenNzb();
    void _onHistoryExportInfo();
    void _onHistoryExportCsv();
    void _onHistoryCopyPassword();
    void _onHistoryPurgePassword();
    void _onHistoryDeleteEntry();
    void _onHistoryOpenNzb();
    void _onHistoryResumePost();
    void _onStatsRefresh();
    void _onResumeSelectionChanged();
    void _onResumePost();
    void _onResumeAbandon();
    void _onResumePurge();
    void _onResumeIgnore();
    void _onResumeDeleteEntries();

    void _onHistoryContextMenu(const QPoint &pos);
    //! Right click on the history header: offers to hand the column widths
    //! back to ngPost.
    void _onHistoryHeaderContextMenu(const QPoint &pos);
    //! Forget the widths the user set, here and in the settings, and size the
    //! columns again.
    void _resetHistoryColumns();

    //! Phase 5d/6: a per-row server field (checkbox or text field) changed.
    //! Persist immediately so adding, editing or toggling a server survives
    //! a restart without forcing the user through "Save Config".
    void _onServerFieldEdited();

private:
    void _initServerBox();
    void _initPostingBox();
    QWidget *_buildHistoryTab();
    void     _retranslateHistoryTab();
    void _refreshHistoryViews(bool rewindEmptyPage = true);
    //! Size the history columns to their content (\a toContents) and hand the
    //! name column the room the others leave. Does nothing once the user has
    //! resized a column themselves.
    void _fitHistoryColumns(bool toContents);
    //! Column widths kept from the last run, and their write back. Restoring
    //! any leaves them to the user: _fitHistoryColumns() then stands aside.
    void _restoreHistoryColumns();
    void _saveHistoryColumns();
    void _showHistoryDetails(const PostHistoryStore::PostDetails &details);
    bool _startResumePost(qint64 postId, bool askConfirmation = true);

    void _addServer(NntpServerParams *serverParam);
    int  _serverRow(QObject *delButton);
    PostingWidget *_getPostWidget(int tabIndex) const;
    int _getPostWidgetIndex(PostingWidget *postWidget) const;

    StartupTabBar *_startupTabBar() const;
    void _fillTabContextMenu(QMenu &menu, int tabIndex);
    void _applyStartupTab();


    static const QString sGroupBoxStyle;
    static const QString sTabWidgetStyle;

};

AutoPostWidget *MainWindow::autoWidget() const { return _autoPostTab; }

#endif // MAINWINDOW_H
